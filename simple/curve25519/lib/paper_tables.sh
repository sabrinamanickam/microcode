# lib/paper_tables.sh — emit the paper-format tables at the end of a sweep.
#
# Sourced by bench_supercop_matrix.sh. Defines emit_paper_tables(); the
# orchestrator calls it after emit_results_md. No top-level work happens here.
#
# Why this is a separate artifact from RESULTS.md
# -----------------------------------------------
# RESULTS.md is the audit record: every contender, raw RDTSC ticks, every
# config. PAPER_TABLES.md is the presentation layer fixed by
# curve25519_results_handoff.md — three main-text tables in core kcycles with
# the TSC correction applied, plus the appendices that let a reviewer audit the
# selection rule. Both are generated from the SAME sweep, so they cannot
# disagree; RESULTS.md is the input to the paper tables, not a parallel copy.
#
# The run context (frequency correction, governor, turbo state, delivered and
# TSC frequencies) is passed through the environment from freq_guard.sh's
# globals, so the methodology table in the paper always describes the run that
# actually produced the numbers. If the guard could not measure f_core/f_TSC,
# the generator says so in the output rather than applying a stale constant.

emit_paper_tables() {
    local results="${1:-RESULTS.md}"
    local out="${2:-PAPER_TABLES.md}"
    local gen="$SCRIPT_DIR/lib/gen_paper_tables.py"

    if [[ ! -r "$results" ]]; then
        echo "  [warn] $results not readable — skipping paper tables."
        return 0
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        echo "  [warn] python3 not found — skipping paper tables."
        echo "         Generate later with: python3 lib/gen_paper_tables.py"
        return 0
    fi

    # Run context, read live so the methodology table cannot drift from reality.
    CYCLE_CORRECTION="${CYCLE_CORRECTION:-}" \
    DELIVERED_FREQ_MHZ="${DELIVERED_FREQ_MHZ:-}" \
    TSC_FREQ_MHZ="${TSC_FREQ_MHZ:-}" \
    PINNED_FREQ_KHZ="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo '?')" \
    GOVERNOR="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo '?')" \
    NO_TURBO="$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo '?')" \
    CPU_MODEL="$(awk -F': ' '/model name/{print $2; exit}' /proc/cpuinfo)" \
    CONFIGS_RAN="${#ran_cfgs[@]}" \
    FREQ_DRIFT_STATUS="${FREQ_DRIFT_STATUS:-}" \
    POST_DELIVERED_FREQ_MHZ="${POST_DELIVERED_FREQ_MHZ:-}" \
    POST_TSC_FREQ_MHZ="${POST_TSC_FREQ_MHZ:-}" \
    ISOLATION_STATUS="${ISOLATION_STATUS:-}" \
    NOHZ_FULL_STATUS="${NOHZ_FULL_STATUS:-}" \
    RUNS_PER_CONFIG="${RUNS_PER_CONFIG:-1}" \
    REPRO_WORST_PCT="${REPRO_WORST_PCT:-}" \
    REPRO_WORST_WHERE="${REPRO_WORST_WHERE:-}" \
        python3 "$gen" "$results" "$out" || {
            echo "  [warn] paper-table generation failed; $results is unaffected."
            return 0
        }

    if [[ -z "${CYCLE_CORRECTION:-}" ]]; then
        echo "  [warn] f_core/f_TSC was not measured this run — $out reports RAW ticks."
        echo "         Absolute kcycles are NOT paper-ready until turbostat is available."
    fi
}
