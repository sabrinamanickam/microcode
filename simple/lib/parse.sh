# lib/parse.sh — scoreboard bookkeeping.
#
# Sourced by bench_supercop_matrix.sh. Owns how a single measurement is
# recorded into the global result tables that the renderers later read.
#
# Global state (declared in the orchestrator):
#   cell["cfg|label"]      -> min cycles for that (config, contender) pair
#   cell_med["cfg|label"]  -> median cycles for that pair
#   best_min[label]        -> best (smallest) min seen for a contender
#   best_cfg[label]        -> config that achieved best_min   (SUPERCOP style)
#   best_med[label]        -> best median seen for a contender
#   best_med_cfg[label]    -> config that achieved best_med

# record_result <cfg> <label> <min> <median>
#
# Stores one measurement and updates the running per-contender best. <median>
# may be empty (some outputs only report a min) — the median tables/bests are
# simply left untouched in that case. A missing <min> is a no-op.
record_result() {
    local cfg="$1" label="$2" mn="$3" md="$4"
    [ -z "$mn" ] && return 0

    cell["$cfg|$label"]="$mn"
    [ -n "$md" ] && cell_med["$cfg|$label"]="$md"

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
