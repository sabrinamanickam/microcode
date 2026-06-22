#!/usr/bin/env bash
# sweep_adcx.sh — run adcx_probe over a list of candidate opcodes.
# Each opcode runs in its own process with a timeout, so a wedged core
# only loses that one slot.  Failed/hung opcodes are recorded separately.
#
# Usage:
#   sudo ./sweep_adcx.sh                          # default candidate list
#   sudo ./sweep_adcx.sh 0x33b 0x33d 0x37b        # explicit opcodes
#   sudo ./sweep_adcx.sh --range 0x300 0x3ff      # inclusive range
#   sudo ./sweep_adcx.sh --resume 0x35e           # default list, skip < 0x35e
#   sudo ./sweep_adcx.sh --bigband                # 0x381..0x3ad long band
#
# Requires: adcx_probe_static in cwd (build with `make PROG=adcx_probe`).

set -u

PROBE=./adcx_probe_static
TIMEOUT_S=5
LOG=adcx_sweep.log
HITS=adcx_hits.log
HANGS=adcx_hangs.log
APPEND=0

# Skip known/decoded opcodes — sweeping them is noise.
declare -A SKIP=(
    [0x33e]=ADC      [0x37e]=ADC_DSZ64
    [0x33f]=SBB      [0x37f]=SBB_DSZ64
    [0x37d]=GENARITHFLAGS
    [0x380]=READAFLAGS
    [0x338]=CLC      [0x339]=CMC     [0x33a]=STC
    [0x33c]=BSWAP32
    [0x26c]=MUL_DSZ64
    [0x000]=ADD32    [0x040]=ADD64
    [0x005]=SUB32    [0x045]=SUB64
)

# DANGER list — confirmed or strongly-suspected NUC-killers / process crashers.
# Edit this as you learn more; entries are NEVER probed.
declare -A DANGER=(
    [0x34c]="SIGSEGV in probe (2026-05-26)"
    # 0x350..0x35f is the UJMPCC/MJMPCC band — branch-on-flags ops.
    # Firing one with garbage targets while inside a vmwrite trap can wedge
    # the kernel.  0x35e is the confirmed culprit from the 2026-05-26 run.
    [0x350]=UJMPCC   [0x351]=UJMPCC   [0x352]=UJMPCC   [0x353]=UJMPCC
    [0x354]=UJMPCC?  [0x355]=UJMPCC?  [0x356]=UJMPCC?  [0x357]=UJMPCC?
    [0x358]=MJMPCC   [0x359]=MJMPCC   [0x35a]=MJMPCC   [0x35b]=MJMPCC
    [0x35c]=MJMPCC?  [0x35d]=MJMPCC?  [0x35e]="NUC crash 2026-05-26"
    [0x35f]=MJMPCC?
)

run_one() {
    local op="$1"
    if [[ -n "${DANGER[$op]:-}" ]]; then
        printf "OPC=%s DANGER=%s\n" "$op" "${DANGER[$op]}" | tee -a "$LOG"
        return
    fi
    if [[ -n "${SKIP[$op]:-}" ]]; then
        printf "OPC=%s SKIP=%s\n" "$op" "${SKIP[$op]}" | tee -a "$LOG"
        return
    fi
    # Each invocation is a fresh process — the patch from the prior probe
    # is replaced on install, so we don't need to clean up between runs.
    if out=$(timeout "$TIMEOUT_S" taskset -c 0 "$PROBE" "$op" 2>&1); then
        printf "%s\n" "$out" >> "$LOG"
        verdict=$(printf "%s" "$out" | awk -F'VERDICT=' '/^OPC=/{print $2; exit}')
        printf "OPC=%s VERDICT=%s\n" "$op" "$verdict"
        case "$verdict" in
            *ADCX*|*ADOX*|"non-add"*|"ADD-flag-clean"*|"ADC-flag-clean"*|*weird-flags*)
                printf "%s\n" "$out" >> "$HITS"
                ;;
        esac
    else
        rc=$?
        printf "OPC=%s HANG/FAIL rc=%d\n" "$op" "$rc" | tee -a "$LOG" -a "$HANGS"
    fi
}

candidates=()
resume_from=""
mode="${1:-default}"

case "$mode" in
    --range)
        lo=$(( $2 )) ; hi=$(( $3 ))
        for (( o = lo; o <= hi; o++ )); do
            candidates+=( "$(printf '0x%03x' "$o")" )
        done
        ;;
    --resume)
        resume_from="$2"
        ;;
    --bigband)
        # Long unmapped 0x381..0x3ad band between READAFLAGS and RCL_DSZ16.
        # Separated out because it's far from the arithmetic neighborhood and
        # less likely to host ADCX/ADOX — sweep it on its own pass.
        for (( o = 0x381; o <= 0x3ad; o++ )); do
            candidates+=( "$(printf '0x%03x' "$o")" )
        done
        ;;
    --append)
        APPEND=1
        ;;
esac

if [[ ${#candidates[@]} -eq 0 ]]; then
    if [[ "$mode" != "default" && "$mode" != "--resume" && "$mode" != "--append" \
          && "${mode#0x}" == "$mode" ]]; then
        # Unrecognized first arg that isn't a 0x... opcode — bail to avoid surprise.
        echo "unrecognized arg: $mode" >&2
        exit 2
    fi
    if [[ $# -gt 0 && "${1#0x}" != "$1" ]]; then
        candidates=( "$@" )
    else
        # Default: gaps in the arithmetic neighborhood (around ADC/SBB/GFL).
        # Excludes 0x350..0x35f (UJMPCC/MJMPCC — DANGER) and the long 0x381..0x3ad
        # band (run that with --bigband on a fresh boot).
        candidates=(
            # near ADC_DSZ32 (0x33e) and SBB_DSZ32 (0x33f)
            0x33b 0x33d
            # 0x340..0x34f — large unmapped band
            0x340 0x341 0x342 0x343 0x344 0x345 0x346 0x347
            0x348 0x349 0x34a 0x34b           0x34d 0x34e 0x34f
            # 0x360..0x36f between MJMPCC band and SELECTCC_64
            0x360 0x361 0x362 0x363 0x364 0x365 0x366 0x367
            0x368 0x369 0x36a 0x36b 0x36c 0x36d 0x36e 0x36f
            # 0x378..0x37c right before GENARITHFLAGS / ADC_DSZ64
            0x378 0x379 0x37a 0x37b 0x37c
        )
    fi
fi

if [[ -n "$resume_from" ]]; then
    # Re-use the default list but filter to ops >= resume_from.
    rf=$(( resume_from ))
    base=(
        0x33b 0x33d
        0x340 0x341 0x342 0x343 0x344 0x345 0x346 0x347
        0x348 0x349 0x34a 0x34b           0x34d 0x34e 0x34f
        0x360 0x361 0x362 0x363 0x364 0x365 0x366 0x367
        0x368 0x369 0x36a 0x36b 0x36c 0x36d 0x36e 0x36f
        0x378 0x379 0x37a 0x37b 0x37c
    )
    candidates=()
    for op in "${base[@]}"; do
        if (( $(( op )) >= rf )); then candidates+=( "$op" ); fi
    done
fi

# Truncate logs only when starting a fresh sweep.  --append, --resume keep them.
if [[ "$APPEND" -eq 0 && -z "$resume_from" ]]; then
    : > "$LOG" ; : > "$HITS" ; : > "$HANGS"
fi

for op in "${candidates[@]}"; do
    run_one "$op"
done

echo
echo "=== summary ==="
echo "log:   $LOG"
echo "hits:  $HITS"
echo "hangs: $HANGS"
echo "hit count:  $(grep -c '^OPC=' "$HITS" 2>/dev/null || echo 0)"
echo "hang count: $(grep -c '^OPC=' "$HANGS" 2>/dev/null || echo 0)"
