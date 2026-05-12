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
# is roughly N_configs * (build + bench) ≈ 8 * ~30s ≈ 4 minutes.

set -euo pipefail
cd "$(dirname "$0")"

# Flags every config gets. Note: SUPERCOP's gcc line adds -mtune=native too,
# but the clang line does NOT (since clang's -march=native implies -mtune=native).
# To match exactly, -mtune=native is conditionally appended only for gcc below.
SUPERCOP_BASE="-fwrapv -fPIC -fPIE -gdwarf-4 -Wall -march=native -I include/"

# (compiler, -O level) pairs — same ones in supercop-20260330/okcompilers/c.
CONFIGS=(
    "gcc -O3"
    "gcc -O2"
    "gcc -Os"
    "gcc -O"
    "clang -O3"
    "clang -O2"
    "clang -Os"
    "clang -O"
)

# Contender labels printed by the benchmark (must exactly match the
# beginning of each "label:" printf line in full_curve25519.c).
CONTENDERS=(
    "ours/hand-C"
    "ours/fiat"
    "donna_c64"
    "amd64-51/asm"
    "amd64-51/ucode"
    "ours/ucode"
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
        "ours/hand-C")    echo "hand-C"    ;;
        "ours/fiat")      echo "fiat"      ;;
        "ours/ucode")     echo "ucode"     ;;
        "donna_c64")      echo "donna"     ;;
        "amd64-51/asm")   echo "a51/asm"   ;;
        "amd64-51/ucode") echo "a51/ucode" ;;
        *)                echo "$1"        ;;
    esac
}

for cfg in "${ACTIVE_CONFIGS[@]}"; do
    cc="${cfg%% *}"
    opt="${cfg##* }"

    cflags="$opt $SUPERCOP_BASE -masm=intel"
    if [ "$cc" = "clang" ]; then
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
    rm -f amd64-51_*.o full_curve25519_static

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

SAME_LADDER_OURS=("ours/hand-C" "ours/fiat" "ours/ucode")
SAME_LADDER_A51=("amd64-51/asm" "amd64-51/ucode")
END_TO_END=("donna_c64" "amd64-51/asm" "amd64-51/ucode" "ours/ucode")

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
echo "#  MEDIAN cycle matrices                                        "
echo "###############################################################"
print_matrix "median" "Same-ladder field-op comparison — OUR ladder" \
             "${SAME_LADDER_OURS[@]}"
print_matrix "median" "Same-ladder field-op comparison — amd64-51 ladder" \
             "${SAME_LADDER_A51[@]}"
print_matrix "median" "End-to-end implementation comparison" \
             "${END_TO_END[@]}"

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
        "ours/ucode"|"amd64-51/asm"|"amd64-51/ucode") continue ;;
    esac
    emit_row "$label"
done
echo
echo "Naming legend (label = ladder / field-op-backend):"
echo "    ours/hand-C    — our ladder + hand-written C with __uint128_t"
echo "    ours/fiat      — our ladder + fiat-crypto autogen C"
echo "    ours/ucode     — our ladder + microcode field ops"
echo "    donna_c64      — donna's whole-stack portable C"
echo "    amd64-51/asm   — Bernstein/Schwabe whole-stack x86-64 asm"
echo "    amd64-51/ucode — amd64-51's ladder + microcode field ops (hybrid)"
echo
echo "Done."
