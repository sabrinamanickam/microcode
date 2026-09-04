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
#   lib/freq_guard.sh    check_cpu_frequency(),
#                        recheck_effective_freq()     — RDTSC-vs-core-clock guard,
#                                                       bracketing the sweep
#   lib/isolation.sh     setup_isolation()/restore_isolation(), $BENCH_RUN
#   lib/build_run.sh     CONFIGS / CONTENDERS,
#                        select_active_configs(), run_matrix()  — build + bench
#   lib/parse.sh         record_result()               — scoreboard bookkeeping
#   lib/print_matrix.sh  print_*/md_*, report_terminal,
#                        emit_results_md               — all rendering
#   lib/paper_tables.sh  emit_paper_tables()           — paper-format tables
#   lib/gen_paper_tables.py                            — the generator itself
# Run order: guard → isolate → select configs → sweep → re-check frequency →
#            WRITE RESULTS.md + PAPER_TABLES.md → print → commit both.
# The write happens before any rendering or summary: a finished sweep is ~16
# minutes of data held only in shell variables, so nothing cosmetic downstream
# is allowed to be able to discard it.
# ─────────────────────────────────────────────────────────────────────────

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source lib/freq_guard.sh
source lib/isolation.sh
source lib/parse.sh
source lib/build_run.sh
source lib/print_matrix.sh
source lib/paper_tables.sh

# ── Shared result tables (populated by record_result, read by the renderers) ──
# We track median cycles only.
declare -A cell_med=()    # "cfg|label" -> median cycles (headline stat)
declare -A cell_min=()    # "cfg|label" -> min cycles
declare -A cell_p10=()    # "cfg|label" -> 10th-percentile cycles (dispersion)
declare -A cell_p90=()    # "cfg|label" -> 90th-percentile cycles (dispersion)
declare -A cell_runs=()     # "cfg|label" -> per-run medians, when RUNS_PER_CONFIG>1
declare -A best_med=()    # label -> best (smallest) median cycles
declare -A best_med_cfg=()  # label -> winning config (for median)
declare -a ran_cfgs=()    # configs that produced a benchmark (excludes build failures)
declare -a failed_cfgs=() # configs/contenders skipped because a binary exited non-zero

# ── The single end-to-end table: every contender, median cycles, one matrix.
# (Same-ladder sub-tables and ratio matrices were removed — the asm baselines
#  amd64-51/asm and amd64-64/asm are already in this one table.)
TABLE=("ours/ucode" "amd64-64/asm" "amd64-64/asm-Clad" "amd64-64/ucode" "amd64-51/asm" "amd64-51/asm-Clad" "amd64-51/ucode-Clad" "amd64-51/ucode" "ours/cryptopt" "ours/fiat" "ours/hand-C" "donna_c64")

# ── Saturated-representation ladder control. amd64-64/ucode replaces THREE of
# amd64-64's files (ladderstep + mul + square), so a64/asm vs a64/ucode mixes
# the field-op backend with a qhasm->C ladder rewrite. amd64-64/asm-Clad is the
# missing middle point (C ladder + amd64-64's own asm field ops):
#   asm-Clad / ucode  = field-op effect, ladder held constant
#   asm / asm-Clad    = the ladder-rewrite tax, measured on its own
SATURATED_ISO=("amd64-64/asm" "amd64-64/asm-Clad" "amd64-64/ucode")

# ── Unsaturated-representation ladder square. amd64-51/ucode uses an inline-asm
# ladder with the microcode fired inline (no fe25519_mul() call to substitute),
# so its field ops cannot be swapped under a fixed ladder the way amd64-64's
# can. These four corners give the decomposition inside amd64-51's framework:
#   asm-Clad   / asm        = ladder tax (qhasm ladderstep.S -> C)
#   asm-Clad   / ucode-Clad = field-op effect, ladder AND framework constant
#   ucode-Clad / ucode      = ladder coding style, field ops constant
UNSATURATED_ISO=("amd64-51/asm" "amd64-51/asm-Clad" "amd64-51/ucode-Clad" "amd64-51/ucode")

# ── Whole-stack implementations, for the end-to-end standing table. Excludes
# every *-Clad / C-ladder arm: those are controls, not implementations anyone
# would ship, and listing them alongside would invite exactly the confounded
# reading the controls exist to prevent.
STANDING=("ours/ucode" "amd64-64/asm" "amd64-51/asm" "amd64-51/ucode" "donna_c64" "ours/fiat" "ours/cryptopt" "ours/hand-C")

# ── Same-ladder field-op isolation: these four use the IDENTICAL C Montgomery
# ladder (driver/invert/cswap/pack all held constant); only fe_mul/fe_sq
# differ. Attributes the microcode win end-to-end with zero confounds.
# (ucode/C-ladder = microcode field ops on the C ladder, NOT the inline ladder
#  that the headline ours/ucode uses.)
# a51ops/C-ladder is the CONTROL that matters most here: Bernstein-Schwabe's
# amd64-51 hand-asm field ops on this same C ladder. ucode/C-ladder vs
# a51ops/C-ladder is the microcode-vs-hand-asm field-op claim with the ladder
# held constant — the headline amd64-51/asm vs amd64-51/ucode ratio does NOT
# hold it constant (that pair also swaps qhasm ladderstep.S for the inline-asm
# ladder), so it cannot carry the claim on its own.
FIELDOP_ISO=("ucode/C-ladder" "a51ops/C-ladder" "ours/cryptopt" "ours/fiat" "ours/hand-C")

# ── Pipeline ──
check_cpu_frequency
select_active_configs

# Warm sudo so taskset doesn't prompt mid-loop.
echo "Priming sudo (will be needed for taskset on each run)..."
sudo -v

# Shield the measurement core, and put it back however the sweep ends.
trap restore_isolation EXIT
setup_isolation
echo

run_matrix

# Bracket the sweep: prove the core/TSC ratio held for its whole duration, not
# just at the start. Sets FREQ_DRIFT_*, which the results header reports.
echo
echo "── Post-sweep frequency verification ──────────────────────────────"
recheck_effective_freq

# ── Write the artifacts NOW, before anything cosmetic ─────────────────────
# A finished sweep is ~16 minutes of data living only in shell variables.
# Nothing downstream of this point may be capable of discarding it — a bug in
# a summary line once killed the script with all 24 configs already measured.
# Rendering and reporting happen after the files are on disk.
RESULTS_FILE="${RESULTS_FILE:-RESULTS.md}"
emit_results_md "$RESULTS_FILE"

PAPER_FILE="${PAPER_FILE:-PAPER_TABLES.md}"
emit_paper_tables "$RESULTS_FILE" "$PAPER_FILE"

report_terminal

echo "═══════════════════════════════════════════════════════════════"
echo "  Audit record written to:     $RESULTS_FILE"
echo "  Paper tables written to:     $PAPER_FILE"
if (( BENCH_RETRY_COUNT > 0 )); then
    echo "  Transient failures retried:  $BENCH_RETRY_COUNT (succeeded on a later attempt)"
fi
if (( ${#failed_cfgs[@]} > 0 )); then
    printf '  Measurement SKIPPED:         %s\n' "${failed_cfgs[@]}"
fi
echo "═══════════════════════════════════════════════════════════════"
echo

# ───────────────────── auto-commit RESULTS.md to git ──────────────────────
# Commit RESULTS.md on EVERY run. The commit is scoped to RESULTS.md via an
# explicit pathspec AND --allow-empty, so:
#   - any staged/modified WIP elsewhere in the tree is never swept in, and
#   - a commit is still recorded even in the unlikely case RESULTS.md is
#     byte-identical to the previous run.
if git rev-parse --git-dir >/dev/null 2>&1; then
    git add -- "$RESULTS_FILE" ${PAPER_FILE:+"$PAPER_FILE"}
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
