#!/usr/bin/env bash
#
# bench_supercop_matrix.sh — match SUPERCOP's measurement methodology:
#   for each (compiler, -O level) combination SUPERCOP tries, rebuild
#   full_curve25519_static and run the benchmark, then report the best
#   `min cycles` per contender along with which flag combo achieved it.
#
# SUPERCOP picks the fastest result per implementation across all of
# these flag sets — so should we, for a fair comparison.
#
# Run from simple/ directory:
#   ./bench_supercop_matrix.sh
#
# Requires sudo (for taskset's effective core pinning). Total run time
# is roughly N_configs * (build + bench) ≈ 24 * ~30s ≈ 12 minutes.

set -euo pipefail
cd "$(dirname "$0")"

# Flags every config gets. Note: SUPERCOP's gcc line adds -mtune=native too,
# but the clang line does NOT (since clang's -march=native implies -mtune=native).
# To match exactly, -mtune=native is conditionally appended only for gcc below.
SUPERCOP_BASE="-fwrapv -fPIC -fPIE -gdwarf-4 -Wall -march=native -I include/"

# (compiler, -O level) pairs — same -O levels SUPERCOP tries
# (okcompilers/c). We test multiple gcc versions side-by-side; any
# compiler whose binary isn't installed is silently skipped below.
CONFIGS=(
    "gcc-11 -O3"
    "gcc-11 -O2"
    "gcc-11 -Os"
    "gcc-11 -O"
    "gcc-12 -O3"
    "gcc-12 -O2"
    "gcc-12 -Os"
    "gcc-12 -O"
    "gcc-13 -O3"
    "gcc-13 -O2"
    "gcc-13 -Os"
    "gcc-13 -O"
    "clang-14 -O3"
    "clang-14 -O2"
    "clang-14 -Os"
    "clang-14 -O"
    "clang-17 -O3"
    "clang-17 -O2"
    "clang-17 -Os"
    "clang-17 -O"
    "clang-18 -O3"
    "clang-18 -O2"
    "clang-18 -Os"
    "clang-18 -O"
)

# Contender labels printed by the benchmark (must exactly match the
# beginning of each "label:" printf line in full_curve25519.c).
# "ours/ucode-inline" is special: it comes from full_curve25519_inline_static,
# which is a standalone binary with its own main() and a different output
# format (bare "min:"/"median:" lines). It's built+run separately per config.
CONTENDERS=(
    "ours/hand-C"
    "ours/fiat"
    "ours/cryptopt"
    "donna_c64"
    "amd64-51/asm"
    "amd64-51/ucode"
    "amd64-64/asm"
    "ours/ucode"
    "ours/ucode-inline"
)

# Skip configs whose compiler isn't installed.
declare -a ACTIVE_CONFIGS=()
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

# Warm sudo so taskset doesn't prompt mid-loop.
echo "Priming sudo (will be needed for taskset on each run)..."
sudo -v

declare -A cell        # "cfg|label" -> min cycles
declare -A cell_med    # "cfg|label" -> median cycles
declare -A best_min    # label -> best min cycles
declare -A best_med    # label -> best median cycles
declare -A best_cfg    # label -> winning config (for min)
declare -A best_med_cfg # label -> winning config (for median)
declare -a ran_cfgs    # configs that produced a benchmark (excludes build failures)

# Short column headers for the final matrix.
SHORT_LABEL_native_c="native"
SHORT_LABEL_fiat_crypto="fiat"
SHORT_LABEL_supercop_donna_c64="donna_c64"
SHORT_LABEL_supercop_amd64_51="amd64-51"
SHORT_LABEL_microcode="ucode"

short_label() {
    case "$1" in
        "ours/hand-C")       echo "hand-C"      ;;
        "ours/fiat")         echo "fiat"        ;;
        "ours/cryptopt")     echo "cryptopt"    ;;
        "ours/ucode")        echo "ucode"       ;;
        "ours/ucode-inline") echo "ucode-inl"   ;;
        "donna_c64")         echo "donna"       ;;
        "amd64-51/asm")      echo "a51/asm"     ;;
        "amd64-51/ucode")    echo "a51/ucode"   ;;
        "amd64-64/asm")      echo "a64/asm"     ;;
        *)                   echo "$1"          ;;
    esac
}

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

    # Force clean rebuild — .o files cache previous flags.
    rm -f amd64-51_*.o amd64-64_*.o full_curve25519_static full_curve25519_inline_static

    if ! make -s PROG=full_curve25519 CC="$cc" CFLAGS="$cflags" >/dev/null 2>build_err.log ; then
        echo "[BUILD FAILED]"
        sed 's/^/    /' build_err.log
        continue
    fi
    rm -f build_err.log

    output=$(sudo taskset -c 0 ./full_curve25519_static 2>&1)

    # Echo the bench section so the user sees per-config numbers in real time.
    echo "$output" | sed -n '/X25519 Benchmark/,/^$/p' | head -20

    ran_cfgs+=("$cfg")

    for label in "${CONTENDERS[@]}"; do
        # "ours/ucode-inline" lives in its own binary — parsed below.
        [ "$label" = "ours/ucode-inline" ] && continue
        line=$(echo "$output" | grep -E "^${label}:" || true)
        [ -z "$line" ] && continue
        mn=$(echo  "$line" | grep -oE 'min[[:space:]]+[0-9]+'    | awk '{print $2}')
        md=$(echo  "$line" | grep -oE 'median[[:space:]]+[0-9]+' | awk '{print $2}')
        [ -z "$mn" ] && continue
        cell["$cfg|$label"]="$mn"
        [ -n "$md" ] && cell_med["$cfg|$label"]="$md"

        prev="${best_min[$label]:-}"
        if [ -z "$prev" ] || [ "$mn" -lt "$prev" ]; then
            best_min[$label]="$mn"
            best_cfg[$label]="$cfg"
        fi
        prev_med="${best_med[$label]:-}"
        if [ -n "$md" ] && { [ -z "$prev_med" ] || [ "$md" -lt "$prev_med" ]; }; then
            best_med[$label]="$md"
            best_med_cfg[$label]="$cfg"
        fi
    done

    # ─── ours/ucode-inline: standalone binary, bare "min:"/"median:" output ───
    # full_curve25519_inline.c is the all-in-one-asm-block 5×51 ladder + ucode
    # field ops experiment (see project_x25519_4x64_loses / inline_asm_no_help).
    # Different ladder framing than the other "ours/*" contenders, so it lives
    # in END_TO_END only, not SAME_LADDER_OURS.
    if make -s PROG=full_curve25519_inline CC="$cc" CFLAGS="$cflags" >/dev/null 2>inline_err.log ; then
        rm -f inline_err.log
        inline_out=$(sudo taskset -c 0 ./full_curve25519_inline_static 2>&1)
        echo "$inline_out" | sed -n '/--- Bench/,/p90:/p'

        mn=$(echo "$inline_out" | awk '/^min:/    {print $2; exit}')
        md=$(echo "$inline_out" | awk '/^median:/ {print $2; exit}')
        label="ours/ucode-inline"
        if [ -n "$mn" ]; then
            cell["$cfg|$label"]="$mn"
            [ -n "$md" ] && cell_med["$cfg|$label"]="$md"

            prev="${best_min[$label]:-}"
            if [ -z "$prev" ] || [ "$mn" -lt "$prev" ]; then
                best_min[$label]="$mn"
                best_cfg[$label]="$cfg"
            fi
            prev_med="${best_med[$label]:-}"
            if [ -n "$md" ] && { [ -z "$prev_med" ] || [ "$md" -lt "$prev_med" ]; }; then
                best_med[$label]="$md"
                best_med_cfg[$label]="$cfg"
            fi
        fi
    else
        echo "[INLINE BUILD FAILED]"
        sed 's/^/    /' inline_err.log
        rm -f inline_err.log
    fi
done

# ─────────────────────────────────────────────────────────────────────
# Reviewer-2 fix #1: present results in two separate matrices.
#
# (a) Same-ladder field-op comparison: native / fiat / ucode all share
#     our hand-written Montgomery ladder + invert + cswap + mul121665.
#     Differences reflect FIELD-OP backend only.
#
# (b) End-to-end implementation comparison: donna_c64, amd64-51, and
#     ucode (for orientation). These differ at the WHOLE-FUNCTION level
#     (their own ladders, their own invert chains, their own cswap, …).
#     Differences reflect total implementation, not just field ops.
# ─────────────────────────────────────────────────────────────────────

SAME_LADDER_OURS=("ours/hand-C" "ours/fiat" "ours/cryptopt" "ours/ucode")
SAME_LADDER_A51=("amd64-51/asm" "amd64-51/ucode")
END_TO_END=("donna_c64" "amd64-51/asm" "amd64-51/ucode" "amd64-64/asm" "ours/cryptopt" "ours/ucode" "ours/ucode-inline")

print_matrix() {
    local metric="$1"; shift   # "min" or "median"
    local title="$1"; shift
    local cols=("$@")

    # Pick the right associative arrays for the chosen metric.
    local -n CELL_REF
    local -n BEST_CFG_REF
    if [ "$metric" = "median" ]; then
        CELL_REF=cell_med
        BEST_CFG_REF=best_med_cfg
    else
        CELL_REF=cell
        BEST_CFG_REF=best_cfg
    fi

    echo
    echo "═══════════════════════════════════════════════════════════════"
    echo "  $title  [metric: $metric cycles]"
    echo "  * = winner for that column"
    echo "═══════════════════════════════════════════════════════════════"

    printf "  %-12s" "Config"
    for label in "${cols[@]}"; do
        printf " | %9s" "$(short_label "$label")"
    done
    echo
    printf "  %-12s" "------------"
    for _ in "${cols[@]}"; do printf " + %9s" "---------"; done
    echo

    for cfg in "${ran_cfgs[@]}"; do
        printf "  %-12s" "$cfg"
        for label in "${cols[@]}"; do
            val="${CELL_REF["$cfg|$label"]:-}"
            if [ -z "$val" ]; then
                printf " | %9s" "—"
            elif [ "$cfg" = "${BEST_CFG_REF[$label]}" ]; then
                printf " | %8s*" "$val"
            else
                printf " | %9s" "$val"
            fi
        done
        echo
    done
}

# Speedup ratio = cycles[contender] / cycles[reference], computed per-row.
# Ratio > 1.000 → contender is slower than reference by (ratio-1)*100 %.
# Ratio < 1.000 → contender is faster than reference.
# Ratio = 1.000 → identical (always the case for the reference column).
# ─────────────────────────── markdown emit ─────────────────────────────
# md_matrix / md_ratio_matrix produce the same content as print_matrix /
# print_ratio_matrix but in markdown-table form (right-aligned numeric
# columns, **bold** for the winning cell). Goes to stdout so the caller
# can redirect to a file.

md_matrix() {
    local metric="$1"; shift
    local title="$1"; shift
    local cols=("$@")

    local -n CELL_REF
    local -n BEST_CFG_REF
    if [ "$metric" = "median" ]; then
        CELL_REF=cell_med
        BEST_CFG_REF=best_med_cfg
    else
        CELL_REF=cell
        BEST_CFG_REF=best_cfg
    fi

    echo
    echo "### $title"
    echo
    echo "_metric: $metric cycles. **bold** = winning config in that column._"
    echo

    printf "| Config |"
    for label in "${cols[@]}"; do printf " %s |" "$(short_label "$label")"; done
    echo
    printf "|---|"
    for _ in "${cols[@]}"; do printf "---:|"; done
    echo

    for cfg in "${ran_cfgs[@]}"; do
        printf "| %s |" "$cfg"
        for label in "${cols[@]}"; do
            local val="${CELL_REF["$cfg|$label"]:-}"
            if [ -z "$val" ]; then
                printf " — |"
            elif [ "$cfg" = "${BEST_CFG_REF[$label]}" ]; then
                printf " **%s** |" "$val"
            else
                printf " %s |" "$val"
            fi
        done
        echo
    done
}

md_ratio_matrix() {
    local metric="$1"; shift
    local title="$1"; shift
    local ref_label="$1"; shift
    local cols=("$@")

    local -n CELL_REF
    if [ "$metric" = "median" ]; then
        CELL_REF=cell_med
    else
        CELL_REF=cell
    fi

    echo
    echo "### $title"
    echo
    echo "_ratio = $metric cycles ÷ cycles($(short_label "$ref_label")). >1 = slower, <1 = faster, **bold** = geomean row._"
    echo

    printf "| Config |"
    for label in "${cols[@]}"; do printf " %s |" "$(short_label "$label")"; done
    echo
    printf "|---|"
    for _ in "${cols[@]}"; do printf "---:|"; done
    echo

    for cfg in "${ran_cfgs[@]}"; do
        printf "| %s |" "$cfg"
        local ref_val="${CELL_REF["$cfg|$ref_label"]:-}"
        for label in "${cols[@]}"; do
            local val="${CELL_REF["$cfg|$label"]:-}"
            if [ -z "$val" ] || [ -z "$ref_val" ]; then
                printf " — |"
            else
                local r
                r=$(awk -v a="$val" -v b="$ref_val" 'BEGIN{printf "%.3f", a/b}')
                printf " %s |" "$r"
            fi
        done
        echo
    done

    # Geomean row.
    printf "| **geomean** |"
    for label in "${cols[@]}"; do
        if [ "$label" = "$ref_label" ]; then
            printf " **1.000** |"
            continue
        fi
        local gm
        gm=$(
            for cfg in "${ran_cfgs[@]}"; do
                local v="${CELL_REF["$cfg|$label"]:-}"
                local rv="${CELL_REF["$cfg|$ref_label"]:-}"
                [ -n "$v" ] && [ -n "$rv" ] && awk -v a="$v" -v b="$rv" 'BEGIN{printf "%.6f\n", a/b}'
            done | awk '
                { sum += log($1); n++ }
                END { if (n > 0) printf "%.3f", exp(sum/n); else printf "—" }
            '
        )
        printf " **%s** |" "$gm"
    done
    echo
}

emit_results_md() {
    local out="${1:-RESULTS.md}"
    local freq gov turbo cpu
    freq=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo "?")
    gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "?")
    turbo=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo "?")
    cpu=$(awk -F': ' '/model name/{print $2; exit}' /proc/cpuinfo)

    {
        echo "# X25519 Microcode Benchmark Results"
        echo
        echo "**Generated:** $(date)"
        echo "**Host:** $(hostname)"
        echo "**CPU:** $cpu"
        echo "**Pinned freq:** $freq kHz   (governor: \`$gov\`, no_turbo: \`$turbo\`)"
        echo "**Configs that ran:** ${#ran_cfgs[@]} / ${#ACTIVE_CONFIGS[@]}"
        echo "**Pipeline:** \`taskset -c 0 ./full_curve25519_static\` for each (compiler, -O) combo"
        echo
        echo "## Contender legend"
        echo
        echo "| label | backend |"
        echo "|---|---|"
        echo "| ours/hand-C    | our ladder + hand-written C with \`__uint128_t\` |"
        echo "| ours/fiat      | our ladder + fiat-crypto autogen C |"
        echo "| ours/cryptopt  | our ladder + CryptOpt Goldmont-tuned asm field ops |"
        echo "| ours/ucode     | our ladder + microcode field ops |"
        echo "| ours/ucode-inline | all-in-one inline-asm 5×51 ladder + microcode field ops |"
        echo "| donna_c64      | donna whole-stack portable C |"
        echo "| amd64-51/asm   | Bernstein–Schwabe whole-stack x86-64 asm (5x51 unsaturated) |"
        echo "| amd64-51/ucode | amd64-51's ladder + microcode field ops (hybrid) |"
        echo "| amd64-64/asm   | Bernstein–Schwabe whole-stack x86-64 asm (4x64 saturated; lib25519's Goldmont pick) |"
        echo
        echo "## How to read"
        echo
        echo "- **Same-ladder OUR / amd64-51** tables are apples-to-apples: only the field-op backend changes; ladder, invert, cswap, framework all held constant."
        echo "- **End-to-end** mixes implementations top to bottom; differences include integration choices (struct layout, cross-TU inlining, invert chaining) on top of the field-op delta."
        echo "- The **geomean** row in each ratio table collapses 24 configs into one number — that's the single ratio to quote in a paper."
        echo
        echo "---"
        echo
        echo "## MIN cycles"
        md_matrix "min" "Same-ladder OUR — only field-op differs" "${SAME_LADDER_OURS[@]}"
        md_matrix "min" "Same-ladder amd64-51 — only field-op differs" "${SAME_LADDER_A51[@]}"
        md_matrix "min" "End-to-end — mixed implementations" "${END_TO_END[@]}"
        echo
        echo "## MIN speedup ratios"
        md_ratio_matrix "min" "Same-ladder OUR — ratios vs ours/ucode" \
                        "ours/ucode" "${SAME_LADDER_OURS[@]}"
        md_ratio_matrix "min" "Same-ladder amd64-51 — ratios vs amd64-51/ucode" \
                        "amd64-51/ucode" "${SAME_LADDER_A51[@]}"
        md_ratio_matrix "min" "End-to-end — ratios vs ours/ucode" \
                        "ours/ucode" "${END_TO_END[@]}"
        echo
        echo "## MEDIAN cycles"
        md_matrix "median" "Same-ladder OUR" "${SAME_LADDER_OURS[@]}"
        md_matrix "median" "Same-ladder amd64-51" "${SAME_LADDER_A51[@]}"
        md_matrix "median" "End-to-end" "${END_TO_END[@]}"
        echo
        echo "## MEDIAN speedup ratios"
        md_ratio_matrix "median" "Same-ladder OUR — ratios vs ours/ucode" \
                        "ours/ucode" "${SAME_LADDER_OURS[@]}"
        md_ratio_matrix "median" "Same-ladder amd64-51 — ratios vs amd64-51/ucode" \
                        "amd64-51/ucode" "${SAME_LADDER_A51[@]}"
        md_ratio_matrix "median" "End-to-end — ratios vs ours/ucode" \
                        "ours/ucode" "${END_TO_END[@]}"
        echo
        echo "## Best per contender (SUPERCOP-style)"
        echo
        echo "| contender | best min | min config | best median | median config |"
        echo "|---|---:|---|---:|---|"
        for label in "${SAME_LADDER_OURS[@]}" "${SAME_LADDER_A51[@]}" "amd64-64/asm" "donna_c64" "ours/ucode-inline"; do
            local mn="${best_min[$label]:-—}"
            local md="${best_med[$label]:-—}"
            local mn_cfg="${best_cfg[$label]:-—}"
            local md_cfg="${best_med_cfg[$label]:-—}"
            echo "| $label | $mn | $mn_cfg | $md | $md_cfg |"
        done
    } > "$out"
}

print_ratio_matrix() {
    local metric="$1"; shift
    local title="$1"; shift
    local ref_label="$1"; shift   # contender used as denominator
    local cols=("$@")

    local -n CELL_REF
    if [ "$metric" = "median" ]; then
        CELL_REF=cell_med
    else
        CELL_REF=cell
    fi

    echo
    echo "═══════════════════════════════════════════════════════════════"
    echo "  $title  [ratio = $metric cycles / cycles($(short_label "$ref_label"))]"
    echo "  >1.000 = slower than $(short_label "$ref_label");  <1.000 = faster"
    echo "═══════════════════════════════════════════════════════════════"

    printf "  %-12s" "Config"
    for label in "${cols[@]}"; do
        printf " | %9s" "$(short_label "$label")"
    done
    echo
    printf "  %-12s" "------------"
    for _ in "${cols[@]}"; do printf " + %9s" "---------"; done
    echo

    for cfg in "${ran_cfgs[@]}"; do
        printf "  %-12s" "$cfg"
        local ref_val="${CELL_REF["$cfg|$ref_label"]:-}"
        for label in "${cols[@]}"; do
            local val="${CELL_REF["$cfg|$label"]:-}"
            if [ -z "$val" ] || [ -z "$ref_val" ]; then
                printf " | %9s" "—"
            else
                local r
                r=$(awk -v a="$val" -v b="$ref_val" 'BEGIN{printf "%.3f", a/b}')
                printf " | %9s" "$r"
            fi
        done
        echo
    done

    # Per-table summary: best ratio (smallest, i.e. most-favoring-contender
    # config), and the geometric mean across configs for each non-reference
    # column. Useful when a paper quotes a single number per comparison.
    echo
    printf "  %-12s" "geomean"
    for label in "${cols[@]}"; do
        if [ "$label" = "$ref_label" ]; then
            printf " | %9s" "1.000"
            continue
        fi
        # Geometric mean of ratios across all configs with data.
        local gm
        gm=$(
            for cfg in "${ran_cfgs[@]}"; do
                local v="${CELL_REF["$cfg|$label"]:-}"
                local rv="${CELL_REF["$cfg|$ref_label"]:-}"
                [ -n "$v" ] && [ -n "$rv" ] && awk -v a="$v" -v b="$rv" 'BEGIN{printf "%.6f\n", a/b}'
            done | awk '
                { sum += log($1); n++ }
                END { if (n > 0) printf "%.3f", exp(sum/n); else printf "—" }
            '
        )
        printf " | %9s" "$gm"
    done
    echo
}

echo
echo "###############################################################"
echo "#  MIN cycle matrices                                           "
echo "###############################################################"
print_matrix "min" "Same-ladder field-op comparison — OUR ladder (clean: only field ops differ)" \
             "${SAME_LADDER_OURS[@]}"
print_matrix "min" "Same-ladder field-op comparison — amd64-51 ladder (asm field ops vs microcode field ops)" \
             "${SAME_LADDER_A51[@]}"
print_matrix "min" "End-to-end implementation comparison (mixed ladders, mixed everything)" \
             "${END_TO_END[@]}"

echo
echo "###############################################################"
echo "#  MIN speedup-ratio matrices (slower/faster vs the microcode  #"
echo "#  variant in each table; >1 = slower than microcode)          #"
echo "###############################################################"
print_ratio_matrix "min" "OUR ladder — ratios vs ours/ucode" \
                   "ours/ucode" "${SAME_LADDER_OURS[@]}"
print_ratio_matrix "min" "amd64-51 ladder — ratios vs amd64-51/ucode" \
                   "amd64-51/ucode" "${SAME_LADDER_A51[@]}"
print_ratio_matrix "min" "End-to-end — ratios vs ours/ucode" \
                   "ours/ucode" "${END_TO_END[@]}"

echo
echo "###############################################################"
echo "#  MEDIAN cycle matrices                                        "
echo "###############################################################"
print_matrix "median" "Same-ladder field-op comparison — OUR ladder" \
             "${SAME_LADDER_OURS[@]}"
print_matrix "median" "Same-ladder field-op comparison — amd64-51 ladder" \
             "${SAME_LADDER_A51[@]}"
print_matrix "median" "End-to-end implementation comparison" \
             "${END_TO_END[@]}"

echo
echo "###############################################################"
echo "#  MEDIAN speedup-ratio matrices                                "
echo "###############################################################"
print_ratio_matrix "median" "OUR ladder — ratios vs ours/ucode" \
                   "ours/ucode" "${SAME_LADDER_OURS[@]}"
print_ratio_matrix "median" "amd64-51 ladder — ratios vs amd64-51/ucode" \
                   "amd64-51/ucode" "${SAME_LADDER_A51[@]}"
print_ratio_matrix "median" "End-to-end — ratios vs ours/ucode" \
                   "ours/ucode" "${END_TO_END[@]}"

echo
echo "═══════════════════════════════════════════════════════════════"
echo "  Best per contender (matches SUPERCOP's reporting style)"
echo "═══════════════════════════════════════════════════════════════"
printf "  %-22s %10s %-15s %10s %s\n" \
       "Contender" "min" "(config)" "median" "(config)"
printf "  %-22s %10s %-15s %10s %s\n" \
       "─────────" "──────" "──────────" "──────" "──────────"

emit_row() {
    local label="$1"
    if [ -n "${best_min[$label]:-}" ]; then
        printf "  %-22s %10d %-15s %10s %s\n" \
            "$label" \
            "${best_min[$label]}" "(${best_cfg[$label]})" \
            "${best_med[$label]:-—}" \
            "${best_med_cfg[$label]:+(${best_med_cfg[$label]})}"
    else
        printf "  %-22s %10s\n" "$label" "—"
    fi
}

echo "  -- Same-ladder, OUR ladder --"
for label in "${SAME_LADDER_OURS[@]}"; do emit_row "$label"; done
echo "  -- Same-ladder, amd64-51 ladder --"
for label in "${SAME_LADDER_A51[@]}"; do emit_row "$label"; done
echo "  -- End-to-end --"
for label in "${END_TO_END[@]}"; do
    case "$label" in
        "ours/cryptopt"|"ours/ucode"|"amd64-51/asm"|"amd64-51/ucode"|"amd64-64/asm") continue ;;
    esac
    emit_row "$label"
done
echo "  -- amd64-64 (own ladder, lib25519's Goldmont pick) --"
emit_row "amd64-64/asm"
echo "  -- inline-asm ladder + microcode field ops --"
emit_row "ours/ucode-inline"
echo
echo "Naming legend (label = ladder / field-op-backend):"
echo "    ours/hand-C    — our ladder + hand-written C with __uint128_t"
echo "    ours/fiat      — our ladder + fiat-crypto autogen C"
echo "    ours/cryptopt  — our ladder + CryptOpt Goldmont-tuned asm field ops"
echo "    ours/ucode     — our ladder + microcode field ops"
echo "    ours/ucode-inline — all-in-one inline-asm 5×51 ladder + microcode field ops"
echo "    donna_c64      — donna's whole-stack portable C"
echo "    amd64-51/asm   — Bernstein/Schwabe whole-stack x86-64 asm (5x51 unsaturated)"
echo "    amd64-51/ucode — amd64-51's ladder + microcode field ops (hybrid)"
echo "    amd64-64/asm   — Bernstein/Schwabe whole-stack x86-64 asm (4x64 saturated; lib25519's Goldmont pick)"
echo

# ─────────────────────────── write RESULTS.md ─────────────────────────────
# Build a markdown-formatted version of all the tables and host info, so
# the run is reproducible / paste-able into a paper draft.
RESULTS_FILE="${RESULTS_FILE:-RESULTS.md}"
emit_results_md "$RESULTS_FILE"
echo "═══════════════════════════════════════════════════════════════"
echo "  Markdown tables written to:  $RESULTS_FILE"
echo "═══════════════════════════════════════════════════════════════"

echo
echo "Done."
