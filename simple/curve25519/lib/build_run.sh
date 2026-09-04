# lib/build_run.sh — what to build, and the build+bench loop.
#
# Sourced by bench_supercop_matrix.sh. Defines the (compiler, -O) grid and the
# contender list, then run_matrix() which — for every config — rebuilds the
# binaries, runs them under taskset, and feeds each measurement to
# record_result() (see lib/parse.sh).
#
# SUPERCOP picks the fastest result per implementation across all of these flag
# sets, so we sweep the same grid and keep the best per contender.
#
# Each configuration is built once and then MEASURED RUNS_PER_CONFIG times; the
# recorded median is the median of those runs. Binaries are launched through
# $BENCH_RUN (lib/isolation.sh), not a bare taskset, so the measurement core is
# shielded from steerable interrupts and from preemption.

# Flags every config gets. SUPERCOP's gcc line adds -mtune=native too, but the
# clang line does NOT (clang's -march=native already implies -mtune=native).
# To match exactly, -mtune=native is appended only for gcc in run_matrix below.
SUPERCOP_BASE="-fwrapv -fPIC -fPIE -gdwarf-4 -Wall -march=native -I include/"

# (compiler, -O level) pairs — same -O levels SUPERCOP tries (okcompilers/c).
# We test multiple gcc/clang versions side-by-side; any compiler whose binary
# isn't installed is silently skipped by select_active_configs.
CONFIGS=(
    "gcc-11 -O3"   "gcc-11 -O2"   "gcc-11 -Os"   "gcc-11 -O"
    "gcc-12 -O3"   "gcc-12 -O2"   "gcc-12 -Os"   "gcc-12 -O"
    "gcc-13 -O3"   "gcc-13 -O2"   "gcc-13 -Os"   "gcc-13 -O"
    "clang-14 -O3" "clang-14 -O2" "clang-14 -Os" "clang-14 -O"
    "clang-17 -O3" "clang-17 -O2" "clang-17 -Os" "clang-17 -O"
    "clang-18 -O3" "clang-18 -O2" "clang-18 -Os" "clang-18 -O"
)

# Contender labels printed by the benchmark (must exactly match the start of
# each "label:" printf line in full_curve25519_inline2.c, which is now the
# primary multi-contender binary — full_curve25519 is kept on disk but no
# longer benched, so full_curve25519_static's row left the table). One
# contender is special — it comes from a standalone binary with its own
# main() and a bare "min:"/"median:" output, built + run separately per config:
#   - "amd64-64/ucode"  -> full_curve25519_amd64_64_ucode_static
#       (the 4x64 microcode patch can't coexist with the 5x51 patches that
#        full_curve25519_inline2 installs, so it lives in its own binary)
#   - "amd64-64/asm-Clad" -> full_curve25519_amd64_64_asmclad_static
#       (the CONTROL that separates the field-op backend from the ladder
#        rewrite: same C ladderstep.c as amd64-64/ucode, but amd64-64's own
#        asm fe25519_mul.S/square.S. Pure native — no patch.)
CONTENDERS=(
    "ours/hand-C"
    "ours/fiat"
    "ours/cryptopt"
    "donna_c64"
    "amd64-51/asm"
    "amd64-51/ucode"
    "amd64-64/asm"
    "amd64-64/asm-Clad"
    "amd64-64/ucode"
    "ours/ucode"
    "ucode/C-ladder"   # microcode field ops on the SAME C ladder as hand-C/fiat/cryptopt
    "a51ops/C-ladder"  # amd64-51 hand-asm field ops on that SAME C ladder (the control)
    "amd64-51/asm-Clad"    # amd64-51 framework + C ladder + amd64-51 asm field ops
    "amd64-51/ucode-Clad"  # amd64-51 framework + C ladder + 5x51 microcode
)

# How many times each binary is re-run within one configuration. A single pass
# measures every (config, contender) pair once, so nothing in the output
# separates a real difference from one process's bad luck. Re-running inside the
# configuration reduces that to a median-of-runs and yields a run-to-run
# reproducibility figure (REPRO_WORST_PCT) for the methodology section. The
# build — which dominates the per-config cost — happens once regardless.
RUNS_PER_CONFIG="${RUNS_PER_CONFIG:-3}"

# Each timed binary installs and tears down microcode patches on every
# invocation. That path is very occasionally flaky — one failure was observed
# after ~162 consecutive successful invocations, and the identical binary then
# passed when re-run by hand. A transient like that must not cost a whole
# configuration, so a non-zero exit is retried before the config is abandoned.
BENCH_RETRIES="${BENCH_RETRIES:-3}"
BENCH_RETRY_COUNT=0     # total retries used across the sweep (reported at the end)

# run_bench <binary> — run it under $BENCH_RUN, retrying a non-zero exit up to
# BENCH_RETRIES times. Sets RUN_OUT (captured stdout+stderr) and RUN_RC. Returns
# 0 if any attempt succeeded, 1 if every attempt failed.
#
# The output goes into a global rather than to stdout: a command substitution
# would run this in a subshell and lose RUN_RC and the retry tally.
RUN_OUT=""
RUN_RC=0
run_bench() {
    local bin="$1" attempt rc
    for (( attempt = 1; attempt <= BENCH_RETRIES; attempt++ )); do
        rc=0
        RUN_OUT=$($BENCH_RUN "$bin" 2>&1) || rc=$?
        if (( rc == 0 )); then
            RUN_RC=0
            (( attempt > 1 )) && echo "    [recovered on attempt $attempt/$BENCH_RETRIES]"
            return 0
        fi
        RUN_RC=$rc
        BENCH_RETRY_COUNT=$((BENCH_RETRY_COUNT + 1))
        echo "    [retry $attempt/$BENCH_RETRIES] ${bin##*/} exited $rc"
    done
    return 1
}

# Configs whose compiler is actually installed. Populated by
# select_active_configs, consumed by run_matrix.
ACTIVE_CONFIGS=()

# select_active_configs — filter CONFIGS down to installed compilers.
# Aborts if nothing usable is found.
select_active_configs() {
    local cfg cc
    for cfg in "${CONFIGS[@]}"; do
        cc="${cfg%% *}"
        if command -v "$cc" >/dev/null 2>&1; then
            ACTIVE_CONFIGS+=("$cfg")
        else
            echo "[skip] $cc not installed — dropping config: $cfg"
        fi
    done

    if [ ${#ACTIVE_CONFIGS[@]} -eq 0 ]; then
        echo "No usable compilers found — abort."
        exit 1
    fi
}

# run_standalone <cfg> <prog> <label> <cc> <cflags> <fail_tag>
#
# Build + run one of the standalone-binary contenders (inline2, amd64-64-ucode)
# that print a bare "min:"/"median:" rather than a "label:" line. Echoes the
# binary's "--- Bench … p90:" section live, then records the result.
run_standalone() {
    local cfg="$1" prog="$2" label="$3" cc="$4" cflags="$5" fail_tag="$6"
    local errlog="${prog}_err.log"

    if make -s PROG="$prog" CC="$cc" CFLAGS="$cflags" >/dev/null 2>"$errlog"; then
        rm -f "$errlog"
        local out md mn run
        local -a mds=() mns=()
        for (( run = 1; run <= RUNS_PER_CONFIG; run++ )); do
            if ! run_bench ./"${prog}_static"; then
                echo "[RUN FAILED after $BENCH_RETRIES attempts — $prog exit $RUN_RC;" \
                     "skipping '$label' for this config]"
                echo "$RUN_OUT" | tail -30 | sed 's/^/    /'
                failed_cfgs+=("$cfg / $label (exit $RUN_RC)")
                return 0
            fi
            out="$RUN_OUT"
            [ "$run" -eq 1 ] && echo "$out" | sed -n '/--- Bench/,/p90:/p'
            md=$(echo "$out" | awk '/^median:/ {print $2; exit}')
            mn=$(echo "$out" | awk '/^min:/ {print $2; exit}')
            [ -n "$md" ] && mds+=("$md")
            [ -n "$mn" ] && mns+=("$mn")
        done
        if (( ${#mds[@]} )); then
            note_repro "$cfg" "$label" "${mds[@]}"
            # This standalone binary reports only min/median (no p10/p90).
            record_result "$cfg" "$label" "$(median_of "${mds[@]}")" \
                          "$( (( ${#mns[@]} )) && min_of "${mns[@]}" )"
        fi
    else
        echo "$fail_tag"
        sed 's/^/    /' "$errlog"
        rm -f "$errlog"
    fi
}

# run_matrix — the main sweep. For each active config: clean-rebuild
# full_curve25519_inline2, run it, scrape every contender's "label: … min …
# median …" line, then build+run the one standalone-binary contender
# (amd64-64/ucode). Populates the global result tables (via record_result)
# and the ran_cfgs list.
run_matrix() {
    local cfg cc opt cflags output line label md mn p10 p90

    for cfg in "${ACTIVE_CONFIGS[@]}"; do
        cc="${cfg%% *}"
        opt="${cfg##* }"

        cflags="$opt $SUPERCOP_BASE -masm=intel"
        if [[ "$cc" == clang* ]]; then
            cflags="$cflags -Qunused-arguments"
        else
            cflags="$cflags -mtune=native"
        fi

        echo
        echo "═══════════════════════════════════════════════════════════════"
        echo "  Config: $cfg"
        echo "  CFLAGS: $cflags"
        echo "═══════════════════════════════════════════════════════════════"

        # Force clean rebuild — .o files cache previous flags. (The
        # amd64-51-ucode_*.o glob is separate: 'amd64-51_*' does NOT match
        # 'amd64-51-ucode_*', and the inline binary now links those objects.)
        # NOTE: these globs must cover every namespace prefix — 'amd64-51_*'
        # does NOT match 'amd64-51-ucode_*', and neither of those matches
        # 'amd64-51-asmclad_*' / 'amd64-51-ucodeclad_*'. A missed prefix
        # silently reuses objects built with the PREVIOUS config's flags.
        rm -f amd64-51_*.o amd64-51-ucode_*.o \
              amd64-51-asmclad_*.o amd64-51-ucodeclad_*.o \
              amd64-64_*.o amd64-64-ucode_*.o amd64-64-asmclad_*.o \
              full_curve25519_static full_curve25519_inline2_static \
              full_curve25519_amd64_64_ucode_static \
              full_curve25519_amd64_64_asmclad_static

        if ! make -s PROG=full_curve25519_inline2 CC="$cc" CFLAGS="$cflags" >/dev/null 2>build_err.log ; then
            echo "[BUILD FAILED]"
            sed 's/^/    /' build_err.log
            continue
        fi
        rm -f build_err.log

        # Run the multi-contender binary RUNS_PER_CONFIG times, then reduce
        # each contender's runs to a median-of-medians. Accumulators are
        # re-declared (and so cleared) per configuration.
        local run
        declare -A md_runs=() mn_runs=() p10_runs=() p90_runs=()
        for (( run = 1; run <= RUNS_PER_CONFIG; run++ )); do
            # Never let one bad invocation abort the sweep. Under `set -e` a
            # bare output=$(...) with a failing binary kills the whole script,
            # and because the output is captured you get no diagnostics at all
            # — a 16-minute run dies at config 19 with nothing to go on.
            # run_bench retries the transient case and reports the rest.
            if ! run_bench ./full_curve25519_inline2_static; then
                echo "[RUN FAILED after $BENCH_RETRIES attempts — exit $RUN_RC on run" \
                     "$run/$RUNS_PER_CONFIG; skipping config '$cfg']"
                echo "$RUN_OUT" | tail -40 | sed 's/^/    /'
                failed_cfgs+=("$cfg (inline2 exit $RUN_RC)")
                continue 2
            fi
            output="$RUN_OUT"

            # Echo the first run's bench section so per-config numbers show live.
            [ "$run" -eq 1 ] && \
                echo "$output" | sed -n '/X25519 Benchmark/,/^$/p' | head -20

            # Contenders that print a "label: … min N … median M …" line inside
            # full_curve25519_inline2's output (all of them except amd64-64/ucode
            # and amd64-64/asm-Clad, the standalone binaries handled below).
            for label in "${CONTENDERS[@]}"; do
                [ "$label" = "amd64-64/ucode" ]    && continue
                [ "$label" = "amd64-64/asm-Clad" ] && continue
                line=$(echo "$output" | grep -E "^${label}:" || true)
                [ -z "$line" ] && continue
                md=$(echo "$line"  | grep -oE 'median[[:space:]]+[0-9]+' | awk '{print $2}')
                mn=$(echo "$line"  | grep -oE 'min[[:space:]]+[0-9]+'    | awk '{print $2}')
                p10=$(echo "$line" | grep -oE 'p10[[:space:]]+[0-9]+'    | awk '{print $2}')
                p90=$(echo "$line" | grep -oE 'p90[[:space:]]+[0-9]+'    | awk '{print $2}')
                [ -n "$md" ]  && md_runs["$label"]+="$md "
                [ -n "$mn" ]  && mn_runs["$label"]+="$mn "
                [ -n "$p10" ] && p10_runs["$label"]+="$p10 "
                [ -n "$p90" ] && p90_runs["$label"]+="$p90 "
            done
        done

        ran_cfgs+=("$cfg")

        # Word splitting on the accumulated lists is intentional below.
        for label in "${CONTENDERS[@]}"; do
            [ "$label" = "amd64-64/ucode" ]    && continue
            [ "$label" = "amd64-64/asm-Clad" ] && continue
            [ -z "${md_runs[$label]:-}" ] && continue
            note_repro "$cfg" "$label" ${md_runs[$label]}
            record_result "$cfg" "$label" \
                "$(median_of ${md_runs[$label]})" \
                "$(min_of    ${mn_runs[$label]:-})" \
                "$(median_of ${p10_runs[$label]:-})" \
                "$(median_of ${p90_runs[$label]:-})"
        done

        # ── amd64-64/ucode: amd64-64 framework (driver, invert, pack/unpack,
        #    cswap) with ladderstep+mul+square swapped for 4×64 chained-ADC
        #    microcode. Same 4×64 saturated representation as amd64-64/asm —
        #    but NOT the same ladder: ladderstep.S is replaced too, so
        #    amd64-64/asm vs amd64-64/ucode is NOT a same-ladder field-op
        #    comparison. The amd64-64/asm-Clad control below is the
        #    same-ladder arm (see CONTROLS.md). Separate binary because the
        #    4×64 patch can't coexist with inline2's 5×51 patches.
        run_standalone "$cfg" full_curve25519_amd64_64_ucode "amd64-64/ucode" \
                       "$cc" "$cflags" "[AMD64-64/UCODE BUILD FAILED]"

        # ── amd64-64/asm-Clad: the CONTROL for the ladder-rewrite confound.
        #    Same C ladderstep.c object source as amd64-64/ucode, but
        #    amd64-64's own asm fe25519_mul.S / fe25519_square.S. So:
        #      asm-Clad vs ucode = field-op backend, ladder held constant
        #      asm      vs asm-Clad = the ladder-rewrite tax, on its own
        #    Pure native (installs no patch); also prints an in-process
        #    amd64-64/asm arm for a same-process ladder-tax ratio.
        run_standalone "$cfg" full_curve25519_amd64_64_asmclad "amd64-64/asm-Clad" \
                       "$cc" "$cflags" "[AMD64-64/ASM-CLAD BUILD FAILED]"
    done
}
