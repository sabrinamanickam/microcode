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
declare -A cell          # "cfg|label" -> min cycles
declare -A cell_med      # "cfg|label" -> median cycles
declare -A best_min      # label -> best min cycles
declare -A best_med      # label -> best median cycles
declare -A best_cfg      # label -> winning config (for min)
declare -A best_med_cfg  # label -> winning config (for median)
declare -a ran_cfgs      # configs that produced a benchmark (excludes build failures)

# ── How the report is grouped (reviewer-2 fix #1: separate matrices) ──
#
# (a) Same-ladder field-op comparison: the variants in each SAME_LADDER_*
#     group share their ladder + invert + cswap + framework, so differences
#     reflect the FIELD-OP backend only.
# (b) End-to-end: differing whole-function implementations (own ladders, own
#     invert chains, own cswap) — differences reflect total implementation.
SAME_LADDER_OURS=("ours/hand-C" "ours/fiat" "ours/cryptopt" "ours/ucode")
SAME_LADDER_A51=("amd64-51/asm" "amd64-51/ucode")
SAME_LADDER_A64=("amd64-64/asm" "amd64-64/ucode")
END_TO_END=("donna_c64" "amd64-51/asm" "amd64-51/ucode" "amd64-64/asm" "amd64-64/ucode" "ours/cryptopt" "ours/ucode" "ours/ucode-inline")

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
