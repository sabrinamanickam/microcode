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
