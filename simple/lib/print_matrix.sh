# lib/print_matrix.sh — all result rendering (terminal + markdown).
#
# Sourced by bench_supercop_matrix.sh. Reads the global result tables
# (cell, cell_med, best_*) and the ran_cfgs list populated by run_matrix, plus
# the comparison-group arrays (SAME_LADDER_OURS, …) defined in the orchestrator.
#
# Four primitives, each with a terminal and a markdown twin:
#   print_matrix / md_matrix             — absolute cycle tables
#   print_ratio_matrix / md_ratio_matrix — per-config ratios vs a reference col,
#                                          plus a geomean summary
# Two drivers wire those into the full report:
#   report_terminal   — everything echoed to stdout
#   emit_results_md    — the same content as a markdown file (for RESULTS.md)

# short_label <contender> — compact column header for the matrices.
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
        "amd64-64/ucode")    echo "a64/ucode"   ;;
        *)                   echo "$1"          ;;
    esac
}

# ───────────────────────────── terminal tables ─────────────────────────────

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

    # Per-table summary: the geometric mean of each non-reference column's
    # ratio across configs. Useful when a paper quotes a single number per
    # comparison.
    echo
    printf "  %-12s" "geomean"
    for label in "${cols[@]}"; do
        if [ "$label" = "$ref_label" ]; then
            printf " | %9s" "1.000"
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
        printf " | %9s" "$gm"
    done
    echo
}

# emit_row <contender> — one line of the terminal "best per contender" table.
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

# report_terminal — the full stdout report: MIN cycle matrices, MIN ratio
# matrices, MEDIAN cycle matrices, MEDIAN ratio matrices, then the SUPERCOP-
# style "best per contender" table and the naming legend. Reads the
# comparison-group arrays (SAME_LADDER_*, END_TO_END) defined by the orchestrator.
report_terminal() {
    echo
    echo "###############################################################"
    echo "#  MIN cycle matrices                                           "
    echo "###############################################################"
    print_matrix "min" "Same-ladder field-op comparison — OUR ladder (clean: only field ops differ)" \
                 "${SAME_LADDER_OURS[@]}"
    print_matrix "min" "Same-ladder field-op comparison — amd64-51 ladder (asm field ops vs microcode field ops)" \
                 "${SAME_LADDER_A51[@]}"
    print_matrix "min" "Same-ladder field-op comparison — amd64-64 ladder (asm field ops vs 4×64 microcode field ops)" \
                 "${SAME_LADDER_A64[@]}"
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
    print_ratio_matrix "min" "amd64-64 ladder — ratios vs amd64-64/ucode" \
                       "amd64-64/ucode" "${SAME_LADDER_A64[@]}"
    print_ratio_matrix "min" "End-to-end — ratios vs ours/ucode-inline" \
                       "ours/ucode-inline" "${END_TO_END[@]}"

    echo
    echo "###############################################################"
    echo "#  MEDIAN cycle matrices                                        "
    echo "###############################################################"
    print_matrix "median" "Same-ladder field-op comparison — OUR ladder" \
                 "${SAME_LADDER_OURS[@]}"
    print_matrix "median" "Same-ladder field-op comparison — amd64-51 ladder" \
                 "${SAME_LADDER_A51[@]}"
    print_matrix "median" "Same-ladder field-op comparison — amd64-64 ladder" \
                 "${SAME_LADDER_A64[@]}"
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
    print_ratio_matrix "median" "amd64-64 ladder — ratios vs amd64-64/ucode" \
                       "amd64-64/ucode" "${SAME_LADDER_A64[@]}"
    print_ratio_matrix "median" "End-to-end — ratios vs ours/ucode-inline" \
                       "ours/ucode-inline" "${END_TO_END[@]}"

    echo
    echo "═══════════════════════════════════════════════════════════════"
    echo "  Best per contender (matches SUPERCOP's reporting style)"
    echo "═══════════════════════════════════════════════════════════════"
    printf "  %-22s %10s %-15s %10s %s\n" \
           "Contender" "min" "(config)" "median" "(config)"
    printf "  %-22s %10s %-15s %10s %s\n" \
           "─────────" "──────" "──────────" "──────" "──────────"

    echo "  -- Same-ladder, OUR ladder --"
    for label in "${SAME_LADDER_OURS[@]}"; do emit_row "$label"; done
    echo "  -- Same-ladder, amd64-51 ladder --"
    for label in "${SAME_LADDER_A51[@]}"; do emit_row "$label"; done
    echo "  -- Same-ladder, amd64-64 ladder (lib25519's Goldmont pick + 4×64 microcode) --"
    for label in "${SAME_LADDER_A64[@]}"; do emit_row "$label"; done
    echo "  -- End-to-end --"
    for label in "${END_TO_END[@]}"; do
        case "$label" in
            "ours/cryptopt"|"ours/ucode"|"amd64-51/asm"|"amd64-51/ucode"|"amd64-64/asm"|"amd64-64/ucode") continue ;;
        esac
        emit_row "$label"
    done
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
    echo "    amd64-64/ucode — amd64-64's ladder + 4×64 microcode field ops (hybrid)"
    echo
}

# ───────────────────────────── markdown tables ─────────────────────────────
# md_matrix / md_ratio_matrix produce the same content as their print_*
# twins but in markdown-table form (right-aligned numeric columns, **bold**
# for the winning cell). Written to stdout so emit_results_md can redirect them.

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
    for _ in "${cols[@]}"; do printf '%s' "---:|"; done
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
    for _ in "${cols[@]}"; do printf '%s' "---:|"; done
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

# emit_results_md <outfile> — write the whole markdown report (host info,
# legend, every cycle/ratio matrix, best-per-contender table) to <outfile>.
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
        echo "| amd64-64/ucode | amd64-64's ladder + 4×64 microcode field ops (hybrid) |"
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
        md_matrix "min" "Same-ladder amd64-64 — only field-op differs (4×64)" "${SAME_LADDER_A64[@]}"
        md_matrix "min" "End-to-end — mixed implementations" "${END_TO_END[@]}"
        echo
        echo "## MIN speedup ratios"
        md_ratio_matrix "min" "Same-ladder OUR — ratios vs ours/ucode" \
                        "ours/ucode" "${SAME_LADDER_OURS[@]}"
        md_ratio_matrix "min" "Same-ladder amd64-51 — ratios vs amd64-51/ucode" \
                        "amd64-51/ucode" "${SAME_LADDER_A51[@]}"
        md_ratio_matrix "min" "Same-ladder amd64-64 — ratios vs amd64-64/ucode" \
                        "amd64-64/ucode" "${SAME_LADDER_A64[@]}"
        md_ratio_matrix "min" "End-to-end — ratios vs ours/ucode-inline" \
                        "ours/ucode-inline" "${END_TO_END[@]}"
        echo
        echo "## MEDIAN cycles"
        md_matrix "median" "Same-ladder OUR" "${SAME_LADDER_OURS[@]}"
        md_matrix "median" "Same-ladder amd64-51" "${SAME_LADDER_A51[@]}"
        md_matrix "median" "Same-ladder amd64-64 (4×64)" "${SAME_LADDER_A64[@]}"
        md_matrix "median" "End-to-end" "${END_TO_END[@]}"
        echo
        echo "## MEDIAN speedup ratios"
        md_ratio_matrix "median" "Same-ladder OUR — ratios vs ours/ucode" \
                        "ours/ucode" "${SAME_LADDER_OURS[@]}"
        md_ratio_matrix "median" "Same-ladder amd64-51 — ratios vs amd64-51/ucode" \
                        "amd64-51/ucode" "${SAME_LADDER_A51[@]}"
        md_ratio_matrix "median" "Same-ladder amd64-64 — ratios vs amd64-64/ucode" \
                        "amd64-64/ucode" "${SAME_LADDER_A64[@]}"
        md_ratio_matrix "median" "End-to-end — ratios vs ours/ucode-inline" \
                        "ours/ucode-inline" "${END_TO_END[@]}"
        echo
        echo "## Best per contender (SUPERCOP-style)"
        echo
        echo "| contender | best min | min config | best median | median config |"
        echo "|---|---:|---|---:|---|"
        for label in "${SAME_LADDER_OURS[@]}" "${SAME_LADDER_A51[@]}" "amd64-64/asm" "amd64-64/ucode" "donna_c64" "ours/ucode-inline"; do
            local mn="${best_min[$label]:-—}"
            local md="${best_med[$label]:-—}"
            local mn_cfg="${best_cfg[$label]:-—}"
            local md_cfg="${best_med_cfg[$label]:-—}"
            echo "| $label | $mn | $mn_cfg | $md | $md_cfg |"
        done
    } > "$out"
}
