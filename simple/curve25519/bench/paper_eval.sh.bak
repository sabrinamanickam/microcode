#!/usr/bin/env bash
#
# paper_eval.sh — produce every Curve25519 table of the paper's performance
# evaluation (keccak/gold489.tex, "Case Study 2") in one session, on one core.
#
#   1. field kernels   5x51  bench/bench_kernel        -> bench/results/kernel_5x51.txt
#                      4x64  bench/bench_kernel_4x64   -> bench/results/kernel_4x64.txt
#                      built and run under the SAME 24 compiler configurations
#                      as the X25519 sweep (lib/build_run.sh CONFIGS); each
#                      contender keeps its best configuration (lib/kernel_best.py),
#                      raw per-config runs in bench/results/kernel_sweep/.
#                      KERNEL_SWEEP=0 restores the old single default-flags build.
#   2. full X25519     24 compiler configs x 3 runs    -> RESULTS.md, PAPER_TABLES.md
#                      (bench/bench_supercop_matrix.sh: inline2 hosts every 5x51
#                       contender; the 4x64 control runs same-process in
#                       bench/bench_table_4x64)
#   3. LaTeX           lib/gen_paper_tex.py            -> paper_tables_c25519.tex
#
# Measurement method (matches the Keccak evaluation):
#   * one core (BENCH_CORE, default 1); every harness re-pins itself there and
#     prints the core it landed on, and a run on any other core is rejected
#   * turbo off, core pinned to its 1100 MHz base, delivered frequency and TSC
#     rate verified under load with APERF/MPERF (turbostat) before AND after
#   * movable IRQs steered to the other core, irqbalance stopped, SCHED_FIFO 99
#   * microcode debug unlock re-asserted on the bench core before every run
#   * contenders interleaved in one process; medians with min / p10 / p90
#   * every patched arm verified (RFC 7748 + randomized kernel checks) before
#     it is timed
#
# Usage (from anywhere; needs sudo; ~25 min for the X25519 sweep, plus the
# kernels x 24 configurations):
#   ./bench/paper_eval.sh                  kernels + sweep + tables, core 1
#   ./bench/paper_eval.sh --kernels-only   kernels + tables from existing RESULTS.md
#   ./bench/paper_eval.sh --tables-only    regenerate the .tex from existing outputs
#   ./bench/paper_eval.sh --skip-kernels   sweep + tables, reusing bench/results/kernel_*.txt
#   BENCH_CORE=0 ./bench/paper_eval.sh     same on core 0
#
# NOTE: step 2 is bench_supercop_matrix.sh unchanged, which ends by committing
# RESULTS.md and running `git push`.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."        # curve25519/
SCRIPT_DIR="$PWD"

MODE=all
case "${1:-}" in
    "")             ;;
    --kernels-only) MODE=kernels ;;
    --tables-only)  MODE=tables ;;
    --skip-kernels) MODE=sweep ;;
    -h|--help)      sed -n '2,32p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 64 ;;
esac

export BENCH_CORE="${BENCH_CORE:-1}"
OUT=bench/results
mkdir -p "$OUT"

source lib/freq_guard.sh
source lib/isolation.sh       # sets HOUSEKEEPING_CORE to the other core, BENCH_RUN

gen_tables() {
    echo
    echo "== LaTeX tables =="
    python3 lib/gen_paper_tex.py RESULTS.md "$OUT/kernel_5x51.txt" \
            "$OUT/kernel_4x64.txt" paper_tables_c25519.tex
}

if [[ "$MODE" == tables ]]; then gen_tables; exit 0; fi

echo "== Curve25519 paper evaluation on core $BENCH_CORE (IRQs -> core $HOUSEKEEPING_CORE) =="
sudo -v
sudo modprobe msr 2>/dev/null || true
sudo wrmsr -p "$BENCH_CORE" 0x1e6 0x200 || {
    echo "ERROR: could not set the microcode debug unlock on core $BENCH_CORE" >&2; exit 1; }

check_cpu_frequency           # exits unless turbo off + pinned; sets CYCLE_CORRECTION

if [[ "$MODE" != sweep ]]; then
# ── 1. kernels ───────────────────────────────────────────────────────────
# Same compiler configurations and flags as the X25519 sweep, read from
# lib/build_run.sh so the two can never drift apart. Every configuration is a
# clean rebuild: the SUPERCOP objects cache the flags they were compiled with.
KERNEL_SWEEP="${KERNEL_SWEEP:-1}"
clean_kernel_objs() {
    rm -f amd64-51_*.o amd64-51-ucode_*.o amd64-51-asmclad_*.o amd64-51-ucodeclad_*.o \
          amd64-64_*.o amd64-64-ucode_*.o amd64-64-asmclad_*.o \
          bench/bench_kernel_static bench/bench_kernel_4x64_static
}
build_kernels() {   # build_kernels <logdir> [CC CFLAGS]
    local dir=$1 p; shift
    for p in bench/bench_kernel bench/bench_kernel_4x64; do
        if (( $# )); then
            make -s PROG=$p CC="$1" CFLAGS="$2" >"$dir/build_${p##*/}.log" 2>&1 || return 1
        else
            make -s PROG=$p >"$dir/build_${p##*/}.log" 2>&1 || return 1
        fi
    done
}

trap restore_isolation EXIT
setup_isolation

run_kernel() {
    local bin=$1 data=$2 log=$3
    echo
    echo "== running $bin =="
    sudo wrmsr -p "$BENCH_CORE" 0x1e6 0x200 || true
    if ! $BENCH_RUN "./$bin" "$data" >"$log" 2>&1; then
        echo "RUN FAILED: $bin (see $log)" >&2; tail -30 "$log" >&2; exit 1
    fi
    if ! grep -q "^PINNED core=$BENCH_CORE sched_getcpu=$BENCH_CORE\$" "$log"; then
        echo "WRONG CORE: $bin reported $(grep -m1 '^PINNED' "$log" || echo 'no PINNED line')" >&2
        exit 1
    fi
    grep -qE "RFC 7748 (FAILED|failed)" "$log" && { echo "RFC 7748 failed in $bin" >&2; exit 1; }
    grep -E "PINNED|mismatches|RFC 7748 OK|drift check|TSC / core" "$log" | sed 's/^/  /'
}

if [[ "$KERNEL_SWEEP" == 1 ]]; then
    mapfile -t KCONFIGS < <(bash -c 'source lib/build_run.sh; printf "%s\n" "${CONFIGS[@]}"')
    KBASE="$(bash -c 'source lib/build_run.sh; printf "%s" "$SUPERCOP_BASE"')"
    rm -rf "$OUT/kernel_sweep"; mkdir -p "$OUT/kernel_sweep"
    K51_ARGS=(); K64_ARGS=()
    for cfg in "${KCONFIGS[@]}"; do
        cc="${cfg%% *}"; opt="${cfg##* }"
        command -v "$cc" >/dev/null 2>&1 || { echo "[skip] $cc not installed"; continue; }
        cflags="$opt $KBASE -masm=intel"                # identical to run_matrix
        if [[ "$cc" == clang* ]]; then cflags="$cflags -Qunused-arguments"
        else cflags="$cflags -mtune=native"; fi
        tag="${cc}_${opt#-}"; dir="$OUT/kernel_sweep/$tag"; mkdir -p "$dir"
        echo
        echo "== kernels, config $cfg =="
        clean_kernel_objs
        if ! build_kernels "$dir" "$cc" "$cflags"; then
            echo "  [BUILD FAILED] $cfg -- skipped (see $dir/build_*.log)" >&2; continue
        fi
        run_kernel bench/bench_kernel_static      "$dir/kernel_5x51.txt" "$dir/kernel_5x51.log"
        run_kernel bench/bench_kernel_4x64_static "$dir/kernel_4x64.txt" "$dir/kernel_4x64.log"
        K51_ARGS+=("$cfg=$dir/kernel_5x51.txt"); K64_ARGS+=("$cfg=$dir/kernel_4x64.txt")
    done
    (( ${#K51_ARGS[@]} )) || { echo "no kernel configuration built" >&2; exit 1; }
    echo
    echo "== kernels: best configuration per contender (${#K51_ARGS[@]} configs) =="
    python3 lib/kernel_best.py "$OUT/kernel_5x51.txt" "${K51_ARGS[@]}"
    python3 lib/kernel_best.py "$OUT/kernel_4x64.txt" "${K64_ARGS[@]}"
    clean_kernel_objs          # leave no config-specific objects for step 2
else
    echo
    echo "== building kernel harnesses (default Makefile flags) =="
    clean_kernel_objs
    build_kernels "$OUT" || { echo "BUILD FAILED: kernels" >&2; exit 1; }
    run_kernel bench/bench_kernel_static      "$OUT/kernel_5x51.txt" "$OUT/kernel_5x51.log"
    run_kernel bench/bench_kernel_4x64_static "$OUT/kernel_4x64.txt" "$OUT/kernel_4x64.log"
fi
# The kernel harness also feeds the Table 1 prose of PAPER_TABLES.md.
cp "$OUT/kernel_5x51.txt" bench/bench_kernel_out.txt

restore_isolation
trap - EXIT

fi   # MODE != sweep
# ── 2. full X25519 sweep ─────────────────────────────────────────────────
if [[ "$MODE" == all || "$MODE" == sweep ]]; then
    [[ "$MODE" == sweep && ! -s "$OUT/kernel_5x51.txt" ]] && {
        echo "--skip-kernels needs $OUT/kernel_5x51.txt from an earlier kernel run" >&2; exit 1; }
    # Keep the previous core-0 audit record next to the new one, once.
    if [[ -f RESULTS.md && ! -f RESULTS.core0.md ]] && grep -q 'Core isolation:\*\* core 0' RESULTS.md; then
        cp RESULTS.md RESULTS.core0.md
        [[ -f PAPER_TABLES.md ]] && cp PAPER_TABLES.md PAPER_TABLES.core0.md
        echo "archived the core-0 RESULTS.md as RESULTS.core0.md"
    fi
    echo
    echo "== full X25519 sweep (bench/bench_supercop_matrix.sh) =="
    KERNEL51_OUT="$OUT/kernel_5x51.txt" ./bench/bench_supercop_matrix.sh
    if ! grep -q "Core isolation:\*\* core $BENCH_CORE" RESULTS.md; then
        echo "RESULTS.md does not report core $BENCH_CORE -- refusing to build tables" >&2
        exit 1
    fi
fi

# ── 3. tables ────────────────────────────────────────────────────────────
gen_tables
echo
echo "Done. Kernel logs: $OUT/*.log   Tables: paper_tables_c25519.tex"
