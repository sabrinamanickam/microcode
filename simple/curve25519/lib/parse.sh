# lib/parse.sh — scoreboard bookkeeping.
#
# Sourced by bench_supercop_matrix.sh. Owns how a single measurement is
# recorded into the global result tables that the renderers later read.
#
# Global state (declared in the orchestrator). Median is the headline statistic;
# min and p10/p90 are kept for the dispersion table.
#   cell_med["cfg|label"]  -> median cycles for that (config, contender) pair
#   cell_min["cfg|label"]  -> min cycles for that pair
#   cell_p10["cfg|label"]  -> 10th-percentile cycles for that pair
#   cell_p90["cfg|label"]  -> 90th-percentile cycles for that pair
#   best_med[label]        -> best (smallest) median seen for a contender
#   best_med_cfg[label]    -> config that achieved best_med

# record_result <cfg> <label> <median> [<min>] [<p10>] [<p90>]
#
# Stores one measurement and updates the running per-contender best (by median).
# A missing/empty <median> is a no-op; missing min/p10/p90 just leave those
# tables untouched (e.g. the standalone amd64-64/ucode binary reports no p10/p90).
record_result() {
    local cfg="$1" label="$2" md="$3" mn="${4:-}" p10="${5:-}" p90="${6:-}"
    [ -z "$md" ] && return 0

    cell_med["$cfg|$label"]="$md"
    [ -n "$mn" ]  && cell_min["$cfg|$label"]="$mn"
    [ -n "$p10" ] && cell_p10["$cfg|$label"]="$p10"
    [ -n "$p90" ] && cell_p90["$cfg|$label"]="$p90"

    local prev_med="${best_med[$label]:-}"
    if [ -z "$prev_med" ] || [ "$md" -lt "$prev_med" ]; then
        best_med[$label]="$md"
        best_med_cfg[$label]="$cfg"
    fi
}

# ── repeat support (n>1 sweeps of each configuration) ─────────────────────
#
# A single pass over the grid measures each (config, contender) pair once, so
# nothing in the output distinguishes a real difference from one process's bad
# luck. RUNS_PER_CONFIG re-runs each binary within the same configuration and
# reduces the runs to one number, which also yields a run-to-run reproducibility
# figure for the methodology section.
#
#   cell_runs["cfg|label"]   -> space-separated medians, one per run
#   REPRO_WORST_PCT          -> largest (max-min)/min spread seen, in percent
#   REPRO_WORST_WHERE        -> the pair that produced it

REPRO_WORST_PCT="0.000"
REPRO_WORST_WHERE="—"

# median_of <v1> [v2 ...] — median of the given integers on stdout, using the
# same convention as bench_stats() in the C harness (upper median for even n).
median_of() {
    printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{ if(NR) print a[int(NR/2)+1] }'
}

# min_of <v1> [v2 ...] — smallest of the given integers on stdout.
min_of() { printf '%s\n' "$@" | sort -n | head -1; }

# note_repro <cfg> <label> <v1> [v2 ...] — record the run-to-run spread for one
# pair and keep the worst seen across the whole sweep.
note_repro() {
    local cfg="$1" label="$2"; shift 2
    (( $# > 1 )) || return 0
    cell_runs["$cfg|$label"]="$*"
    local lo hi pct
    lo=$(min_of "$@"); hi=$(printf '%s\n' "$@" | sort -n | tail -1)
    [[ "$lo" -gt 0 ]] || return 0
    pct=$(awk -v a="$lo" -v b="$hi" 'BEGIN{printf "%.3f", 100*(b-a)/a}')
    if awk -v p="$pct" -v w="$REPRO_WORST_PCT" 'BEGIN{exit !(p>w)}'; then
        REPRO_WORST_PCT="$pct"
        REPRO_WORST_WHERE="$label @ $cfg"
    fi
}
