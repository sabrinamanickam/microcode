#!/usr/bin/env bash
#
# bench_keccak_matrix.sh — the SUPERCOP-style compiler sweep (same idea as
# ../bench_supercop_matrix.sh) applied to the Keccak head-to-head.
#
# For each (compiler, -O level) combination SUPERCOP tries, rebuild
# asm_op_keccak_vs — which recompiles the SUPERCOP scalar Keccak baselines under
# those flags and links the (fixed) microcode patch — run it, and record each
# contender's min/median cyc/perm. SUPERCOP's autotuner keeps the fastest result
# per implementation across all flag sets, so we keep the best per contender and
# compare our microcode against the best-tuned SUPERCOP baseline.
#
# Note on what the sweep actually tunes:
#   - x86_64_asm / x86_64_shld are hand-written .s — assembled identically at
#     every config (compiler/-O is irrelevant to them).
#   - opt64lcu24 / opt64lcu24shld are C — these are what the compiler grid tunes.
#   - microcode is a fixed patch RAM image — compiler-independent; its number is
#     a flat sanity baseline across configs.
# So the sweep's job here is to find the genuinely fastest SUPERCOP config (what
# the autotuner would pick) and prove microcode beats it.
#
# Run from keccak/ :   sudo ./bench_keccak_matrix.sh
# (needs sudo: taskset core-pinning + installing the microcode patch.)
#
# RDTSC counts at the TSC rate, not the core clock — pin to base first or the
# freq guard aborts (override with ALLOW_UNPINNED=1, but cross-config absolute
# numbers are then meaningless; the same-process ratio inside each run stays
# valid regardless).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Generic helpers shared with the curve matrix (no curve-specific state in them).
source ../lib/freq_guard.sh    # check_cpu_frequency()
source ../lib/parse.sh         # record_result()

# ── Result tables (read by record_result in lib/parse.sh) ──
declare -A cell          # "cfg|label" -> min cyc/perm
declare -A cell_med      # "cfg|label" -> median cyc/perm
declare -A best_min      # label -> best (smallest) min
declare -A best_med      # label -> best median
declare -A best_cfg      # label -> config achieving best_min
declare -A best_med_cfg  # label -> config achieving best_med
declare -a ran_cfgs      # configs that produced a benchmark

# Same flag base SUPERCOP uses for its gcc/clang lines, minus '-I include/'
# (the keccak sources reference headers by explicit relative path; they need no
# -I). -masm=intel is appended for the harness compile (it contains Intel-syntax
# inline asm); the Makefile re-derives -masm=att for the SUPERCOP .o baselines.
SUPERCOP_BASE="-fwrapv -fPIC -fPIE -gdwarf-4 -Wall -march=native"

# (compiler, -O level) grid — same -O levels SUPERCOP tries (okcompilers/c).
# Any compiler not installed is silently dropped by select_active_configs.
CONFIGS=(
    "gcc-11 -O3"   "gcc-11 -O2"   "gcc-11 -Os"   "gcc-11 -O"
    "gcc-12 -O3"   "gcc-12 -O2"   "gcc-12 -Os"   "gcc-12 -O"
    "gcc-13 -O3"   "gcc-13 -O2"   "gcc-13 -Os"   "gcc-13 -O"
    "clang-14 -O3" "clang-14 -O2" "clang-14 -Os" "clang-14 -O"
    "clang-17 -O3" "clang-17 -O2" "clang-17 -Os" "clang-17 -O"
    "clang-18 -O3" "clang-18 -O2" "clang-18 -Os" "clang-18 -O"
)

# Contender labels — must match the "keccak/<key>:" lines asm_op_keccak_vs.c
# prints in its "=== matrix-parse ===" block.
CONTENDERS=(
    "keccak/x86_64_asm"
    "keccak/x86_64_shld"
    "keccak/opt64lcu24"
    "keccak/opt64lcu24shld"
    "keccak/microcode"
)
# Short column headers for the markdown matrix, index-aligned with CONTENDERS.
SHORT=( "asm" "shld" "opt24" "opt24shld" "ucode" )

ACTIVE_CONFIGS=()
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

# run_matrix — for each active config: clean-rebuild asm_op_keccak_vs with that
# compiler/flags, run it, scrape each contender's min/median from the
# matrix-parse block, and record. Echoes the per-config human report live.
run_matrix() {
    local cfg cc opt cflags output line label mn md
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

        # Force clean rebuild — cached .o files hold the previous config's flags.
        rm -f keccak_x86_64_asm.o keccak_x86_64_shld.o \
              keccak_opt64lcu24.o keccak_opt64lcu24shld.o \
              asm_op_keccak_vs_static

        if ! make -s PROG=asm_op_keccak_vs CC="$cc" CFLAGS="$cflags" \
                  >/dev/null 2>build_err.log; then
            echo "[BUILD FAILED]"
            sed 's/^/    /' build_err.log
            continue
        fi
        rm -f build_err.log

        output=$(sudo taskset -c 0 ./asm_op_keccak_vs_static 2>&1)

        # Show the human report so the user sees per-config numbers in real time.
        echo "$output" | sed -n '/--- SUPERCOP scalar/,/ratios valid/p'

        ran_cfgs+=("$cfg")

        for label in "${CONTENDERS[@]}"; do
            line=$(echo "$output" | grep -E "^${label}:" || true)
            [ -z "$line" ] && continue
            mn=$(echo "$line" | grep -oE 'min[[:space:]]+[0-9]+'    | awk '{print $2}')
            md=$(echo "$line" | grep -oE 'median[[:space:]]+[0-9]+' | awk '{print $2}')
            record_result "$cfg" "$label" "$mn" "$md"
        done
    done
}

# headline: fastest SUPERCOP baseline (min over the 4 SUPERCOP variants) vs ucode.
# Echoes "<sc_best_label> <sc_best> <uc>" (or empty fields if unavailable).
compute_headline() {
    local label v sc_best="" sc_best_label="" uc
    for label in keccak/x86_64_asm keccak/x86_64_shld \
                 keccak/opt64lcu24 keccak/opt64lcu24shld; do
        v="${best_min[$label]:-}"
        [ -z "$v" ] && continue
        if [ -z "$sc_best" ] || [ "$v" -lt "$sc_best" ]; then
            sc_best="$v"; sc_best_label="$label"
        fi
    done
    uc="${best_min[keccak/microcode]:-}"
    echo "$sc_best_label|$sc_best|$uc"
}

report_terminal() {
    local label hl sc_best_label sc_best uc
    echo
    echo "═══════════════════════════════════════════════════════════════"
    echo "  KECCAK MATRIX SCOREBOARD  (best cyc/perm over ${#ran_cfgs[@]} configs)"
    echo "═══════════════════════════════════════════════════════════════"
    printf "  %-24s %8s %8s   %s\n" "contender" "min" "median" "best config (min)"
    printf "  %-24s %8s %8s   %s\n" "------------------------" "--------" "--------" "-----------------"
    for label in "${CONTENDERS[@]}"; do
        printf "  %-24s %8s %8s   %s\n" "$label" \
            "${best_min[$label]:-n/a}" "${best_med[$label]:-n/a}" "${best_cfg[$label]:-—}"
    done

    hl=$(compute_headline)
    sc_best_label="${hl%%|*}"; hl="${hl#*|}"
    sc_best="${hl%%|*}";       uc="${hl#*|}"
    echo
    if [ -n "$uc" ] && [ -n "$sc_best_label" ]; then
        echo "  fastest SUPERCOP baseline: $sc_best_label = $sc_best cyc/perm (${best_cfg[$sc_best_label]})"
        echo "  microcode (looped):        $uc cyc/perm (${best_cfg[keccak/microcode]})"
        awk -v u="$uc" -v s="$sc_best" 'BEGIN{
            printf "  ratio microcode / fastest: %.3fx  (%s)\n",
                   u/s, (u<s ? "*** microcode WINS ***" : "microcode loses")
        }'
    else
        echo "  (insufficient data for a headline — were any configs run?)"
    fi
    echo "═══════════════════════════════════════════════════════════════"
}

# Markdown: a full (config × contender) matrix of mins + a best-per-contender
# table + the headline. No git interaction (unlike the curve script).
emit_results_md() {
    local out="$1" cfg label i hl sc_best_label sc_best uc
    {
        echo "# Keccak SUPERCOP-matrix benchmark"
        echo
        echo "Same methodology as \`../bench_supercop_matrix.sh\`: for each (compiler, -O)"
        echo "config SUPERCOP tries, rebuild the SUPERCOP scalar Keccak baselines + the"
        echo "head-to-head harness, run back-to-back in one process, keep the best per"
        echo "contender. cyc/perm (RDTSC at TSC rate; pin to base so ticks ≈ true cycles)."
        echo
        echo "Configs that ran: ${#ran_cfgs[@]} / ${#CONFIGS[@]}"
        echo
        echo "## min cyc/perm per (config × contender)"
        echo
        printf "| config |"; for i in "${SHORT[@]}"; do printf " %s |" "$i"; done; echo
        printf "|---|";       for i in "${SHORT[@]}"; do printf -- "---|"; done; echo
        for cfg in "${ran_cfgs[@]}"; do
            printf "| %s |" "$cfg"
            for label in "${CONTENDERS[@]}"; do
                printf " %s |" "${cell[$cfg|$label]:-—}"
            done
            echo
        done
        echo
        echo "## best per contender (what SUPERCOP's autotuner would pick)"
        echo
        echo "| contender | best min | best median | winning config |"
        echo "|---|---|---|---|"
        for label in "${CONTENDERS[@]}"; do
            echo "| $label | ${best_min[$label]:-n/a} | ${best_med[$label]:-n/a} | ${best_cfg[$label]:-—} |"
        done
        echo
        hl=$(compute_headline)
        sc_best_label="${hl%%|*}"; hl="${hl#*|}"
        sc_best="${hl%%|*}";       uc="${hl#*|}"
        if [ -n "$uc" ] && [ -n "$sc_best_label" ]; then
            echo "## headline"
            echo
            echo "- fastest SUPERCOP baseline: **$sc_best_label = $sc_best cyc/perm** (${best_cfg[$sc_best_label]})"
            echo "- microcode (looped): **$uc cyc/perm** (${best_cfg[keccak/microcode]})"
            awk -v u="$uc" -v s="$sc_best" 'BEGIN{
                printf "- ratio microcode / fastest SUPERCOP: **%.3fx** (%s)\n",
                       u/s, (u<s ? "microcode WINS" : "microcode loses")
            }'
        fi
    } > "$out"
}

# ── Pipeline ──
check_cpu_frequency
select_active_configs

echo "Priming sudo (needed for taskset + microcode patch on each run)..."
sudo -v

run_matrix
report_terminal

RESULTS_FILE="${RESULTS_FILE:-KECCAK_RESULTS.md}"
emit_results_md "$RESULTS_FILE"
echo
echo "Markdown written to: $RESULTS_FILE"
echo "Done."
