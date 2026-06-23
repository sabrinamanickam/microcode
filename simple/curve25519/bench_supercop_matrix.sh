#!/usr/bin/env bash
#
# bench_supercop_matrix.sh — match SUPERCOP's measurement methodology:
#   for each (compiler, -O level) combination SUPERCOP tries, rebuild the
#   binaries and run the benchmark, then report the best `min cycles` per
#   contender along with which flag combo achieved it.
#
# SUPERCOP picks the fastest result per implementation across all of these
# flag sets — so should we, for a fair comparison.
#
# Run from simple/ directory:
#   ./bench_supercop_matrix.sh
#
# Requires sudo (for taskset's effective core pinning). Total run time is
# roughly N_configs * (build + bench) ≈ 24 * ~30s ≈ 12 minutes.
#
# ─────────────────────────────────────────────────────────────────────────
# This file is a thin orchestrator. The actual work lives in lib/:
#   lib/freq_guard.sh    check_cpu_frequency()        — RDTSC-vs-core-clock guard
#   lib/build_run.sh     CONFIGS / CONTENDERS,
#                        select_active_configs(), run_matrix()  — build + bench
#   lib/parse.sh         record_result()               — scoreboard bookkeeping
#   lib/print_matrix.sh  print_*/md_*, report_terminal,
#                        emit_results_md               — all rendering
# Run order: guard → select configs → sweep → print → write RESULTS.md → commit.
# ─────────────────────────────────────────────────────────────────────────

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source lib/freq_guard.sh
source lib/parse.sh
source lib/build_run.sh
source lib/print_matrix.sh

# ── Shared result tables (populated by record_result, read by the renderers) ──
# We track median cycles only.
declare -A cell_med      # "cfg|label" -> median cycles
declare -A best_med      # label -> best (smallest) median cycles
declare -A best_med_cfg  # label -> winning config (for median)
declare -a ran_cfgs      # configs that produced a benchmark (excludes build failures)

# ── The single end-to-end table: every contender, median cycles, one matrix.
# (Same-ladder sub-tables and ratio matrices were removed — the asm baselines
#  amd64-51/asm and amd64-64/asm are already in this one table.)
TABLE=("ours/ucode" "amd64-64/asm" "amd64-64/ucode" "amd64-51/asm" "amd64-51/ucode" "ours/cryptopt" "ours/fiat" "ours/hand-C" "donna_c64")

# ── Same-ladder field-op isolation: these four use the IDENTICAL C Montgomery
# ladder (driver/invert/cswap/pack all held constant); only fe_mul/fe_sq
# differ. Attributes the microcode win end-to-end with zero confounds.
# (ucode/C-ladder = microcode field ops on the C ladder, NOT the inline ladder
#  that the headline ours/ucode uses.)
FIELDOP_ISO=("ucode/C-ladder" "ours/cryptopt" "ours/fiat" "ours/hand-C")

# ── Pipeline ──
check_cpu_frequency
select_active_configs

# Warm sudo so taskset doesn't prompt mid-loop.
echo "Priming sudo (will be needed for taskset on each run)..."
sudo -v

run_matrix
report_terminal

# ─────────────────────────── write RESULTS.md ─────────────────────────────
RESULTS_FILE="${RESULTS_FILE:-RESULTS.md}"
emit_results_md "$RESULTS_FILE"
echo "═══════════════════════════════════════════════════════════════"
echo "  Markdown tables written to:  $RESULTS_FILE"
echo "═══════════════════════════════════════════════════════════════"
echo

# ───────────────────── auto-commit RESULTS.md to git ──────────────────────
# Commit RESULTS.md on EVERY run. The commit is scoped to RESULTS.md via an
# explicit pathspec AND --allow-empty, so:
#   - any staged/modified WIP elsewhere in the tree is never swept in, and
#   - a commit is still recorded even in the unlikely case RESULTS.md is
#     byte-identical to the previous run.
if git rev-parse --git-dir >/dev/null 2>&1; then
    git add -- "$RESULTS_FILE"
    configs_ran=$(grep -m1 -oP 'Configs that ran:\*\* \K[0-9]+ */ *[0-9]+' "$RESULTS_FILE" 2>/dev/null || echo "?/?")
    commit_msg="Auto-update RESULTS.md ($configs_ran configs, $(date '+%Y-%m-%d %H:%M %Z'))"
    if git commit --allow-empty -m "$commit_msg" -- "$RESULTS_FILE"; then
        if git push; then
            echo "  Committed + pushed RESULTS.md to $(git rev-parse --abbrev-ref @{u} 2>/dev/null || echo 'remote')."
        else
            echo "  [warn] commit succeeded but 'git push' failed — push manually when ready."
        fi
    else
        echo "  [warn] git commit failed; RESULTS.md left for manual review."
    fi
else
    echo "  (not a git checkout — skipping auto-commit)"
fi

echo
echo "Done."
