# lib/parse.sh — scoreboard bookkeeping.
#
# Sourced by bench_supercop_matrix.sh. Owns how a single measurement is
# recorded into the global result tables that the renderers later read.
#
# Global state (declared in the orchestrator):
#   cell["cfg|label"]      -> min cycles for that (config, contender) pair
#   cell_med["cfg|label"]  -> median cycles for that pair (the headline stat)
#   cell_p10["cfg|label"]  -> 10th-percentile cycles for that pair (dispersion)
#   cell_p90["cfg|label"]  -> 90th-percentile cycles for that pair (dispersion)
#   best_min[label]        -> best (smallest) min seen for a contender
#   best_cfg[label]        -> config that achieved best_min
#   best_med[label]        -> best (smallest) median seen  (SUPERCOP style, headline)
#   best_med_cfg[label]    -> config that achieved best_med

# record_result <cfg> <label> <min> <median> [<p10>] [<p90>]
#
# Stores one measurement and updates the running per-contender best. The median
# is the headline statistic (best-per-contender is chosen by lowest median); min
# is kept for reference and p10/p90 for the dispersion columns. <median>/<p10>/
# <p90> may be empty (some outputs report only a min) — those tables are simply
# left untouched. A missing <min> is a no-op.
record_result() {
    local cfg="$1" label="$2" mn="$3" md="$4" p10="${5:-}" p90="${6:-}"
    [ -z "$mn" ] && return 0

    cell["$cfg|$label"]="$mn"
    [ -n "$md" ]  && cell_med["$cfg|$label"]="$md"
    [ -n "$p10" ] && cell_p10["$cfg|$label"]="$p10"
    [ -n "$p90" ] && cell_p90["$cfg|$label"]="$p90"

    local prev="${best_min[$label]:-}"
    if [ -z "$prev" ] || [ "$mn" -lt "$prev" ]; then
        best_min[$label]="$mn"
        best_cfg[$label]="$cfg"
    fi

    local prev_med="${best_med[$label]:-}"
    if [ -n "$md" ] && { [ -z "$prev_med" ] || [ "$md" -lt "$prev_med" ]; }; then
        best_med[$label]="$md"
        best_med_cfg[$label]="$cfg"
    fi
}
