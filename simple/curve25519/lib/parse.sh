# lib/parse.sh — scoreboard bookkeeping.
#
# Sourced by bench_supercop_matrix.sh. Owns how a single measurement is
# recorded into the global result tables that the renderers later read.
#
# Global state (declared in the orchestrator). We track median cycles only.
#   cell_med["cfg|label"]  -> median cycles for that (config, contender) pair
#   best_med[label]        -> best (smallest) median seen for a contender
#   best_med_cfg[label]    -> config that achieved best_med

# record_result <cfg> <label> <median>
#
# Stores one median measurement and updates the running per-contender best.
# A missing/empty <median> is a no-op.
record_result() {
    local cfg="$1" label="$2" md="$3"
    [ -z "$md" ] && return 0

    cell_med["$cfg|$label"]="$md"

    local prev_med="${best_med[$label]:-}"
    if [ -z "$prev_med" ] || [ "$md" -lt "$prev_med" ]; then
        best_med[$label]="$md"
        best_med_cfg[$label]="$cfg"
    fi
}
