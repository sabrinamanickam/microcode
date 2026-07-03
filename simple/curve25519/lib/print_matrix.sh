# lib/print_matrix.sh — result rendering (terminal + markdown).
#
# Sourced by bench_supercop_matrix.sh. Reads the global result tables
# (cell_med, best_med, best_med_cfg) and the ran_cfgs list populated by
# run_matrix, plus the TABLE array (the contenders to show) defined in the
# orchestrator.
#
# One table only: median cycles, raw per-(config, contender) counts.
#   print_matrix / md_matrix  — the table, terminal and markdown twins
#   report_terminal           — echoed to stdout
#   emit_results_md           — the same content as RESULTS.md

# short_label <contender> — compact column header for the matrix.
short_label() {
    case "$1" in
        "ours/hand-C")       echo "hand-C"      ;;
        "ours/fiat")         echo "fiat"        ;;
        "ours/cryptopt")     echo "cryptopt"    ;;
        "ours/ucode")        echo "ucode"       ;;
        "ucode/C-ladder")    echo "uc/Clad"     ;;
        "donna_c64")         echo "donna"       ;;
        "amd64-51/asm")      echo "a51/asm"     ;;
        "amd64-51/ucode")    echo "a51/ucode"   ;;
        "amd64-64/asm")      echo "a64/asm"     ;;
        "amd64-64/ucode")    echo "a64/ucode"   ;;
        *)                   echo "$1"          ;;
    esac
}

# ───────────────────────────── terminal table ──────────────────────────────

# print_matrix <title> <contender...> — median cycles, one column per
# contender, one row per config. '*' marks the best (lowest-median) config
# for that contender.
print_matrix() {
    local title="$1"; shift
    local cols=("$@")

    echo
    echo "═══════════════════════════════════════════════════════════════"
    echo "  $title  [median cycles]"
    echo "  * = best (lowest-median) config for that column"
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
            val="${cell_med["$cfg|$label"]:-}"
            if [ -z "$val" ]; then
                printf " | %9s" "—"
            elif [ "$cfg" = "${best_med_cfg[$label]}" ]; then
                printf " | %8s*" "$val"
            else
                printf " | %9s" "$val"
            fi
        done
        echo
    done
}

# print_ratio_matrix <focus> <contender...> — "does <focus> win?" table.
# Each competitor column = median(competitor) / median(focus), so:
#   ratio > 1  ⇒  focus is FASTER (fewer cycles) than that competitor — focus wins
#   ratio < 1  ⇒  focus is slower — focus loses
# The focus's own column is skipped. A geomean row collapses configs into one
# number per competitor.
print_ratio_matrix() {
    local focus="$1"; shift
    local cols=("$@")
    local comps=() c
    for c in "${cols[@]}"; do [ "$c" != "$focus" ] && comps+=("$c"); done

    echo
    echo "═══════════════════════════════════════════════════════════════"
    echo "  Does $(short_label "$focus") win?   [ratio = other ÷ $(short_label "$focus"), median cycles]"
    echo "  >1 ⇒ $(short_label "$focus") is FASTER than that column;  <1 ⇒ slower"
    echo "═══════════════════════════════════════════════════════════════"

    printf "  %-12s" "Config"
    for label in "${comps[@]}"; do printf " | %9s" "$(short_label "$label")"; done
    echo
    printf "  %-12s" "------------"
    for _ in "${comps[@]}"; do printf " + %9s" "---------"; done
    echo

    for cfg in "${ran_cfgs[@]}"; do
        printf "  %-12s" "$cfg"
        local fv="${cell_med["$cfg|$focus"]:-}"
        for label in "${comps[@]}"; do
            local v="${cell_med["$cfg|$label"]:-}"
            if [ -z "$v" ] || [ -z "$fv" ]; then
                printf " | %9s" "—"
            else
                local r; r=$(awk -v a="$v" -v b="$fv" 'BEGIN{printf "%.3f", a/b}')
                printf " | %9s" "$r"
            fi
        done
        echo
    done

    # geomean across configs of (competitor / focus).
    echo
    printf "  %-12s" "geomean"
    for label in "${comps[@]}"; do
        local gm
        gm=$(
            for cfg in "${ran_cfgs[@]}"; do
                local v="${cell_med["$cfg|$label"]:-}"
                local fv="${cell_med["$cfg|$focus"]:-}"
                [ -n "$v" ] && [ -n "$fv" ] && awk -v a="$v" -v b="$fv" 'BEGIN{printf "%.6f\n", a/b}'
            done | awk '{ s += log($1); n++ } END { if (n>0) printf "%.3f", exp(s/n); else printf "—" }'
        )
        printf " | %9s" "$gm"
    done
    echo
}

# report_terminal — the stdout report: the median table, the three "does it
# win?" ratio tables (ours/ucode, amd64-51/ucode, amd64-64/ucode), legend.
report_terminal() {
    print_matrix "X25519 end-to-end (all contenders)" "${TABLE[@]}"

    echo
    echo "###############################################################"
    echo "#  Does microcode win?   ratio = other ÷ the ucode variant     #"
    echo "#  >1 ⇒ the ucode variant is FASTER (fewer cycles) than that col#"
    echo "###############################################################"
    print_ratio_matrix "ours/ucode"     "${TABLE[@]}"
    print_ratio_matrix "amd64-51/ucode" "${TABLE[@]}"
    print_ratio_matrix "amd64-64/ucode" "${TABLE[@]}"

    echo
    echo "###############################################################"
    echo "#  Same-ladder field-op isolation: IDENTICAL C ladder, only    #"
    echo "#  fe_mul/fe_sq differ. Attributes the microcode win end-to-end #"
    echo "#  with zero confound (driver/invert/cswap/pack held constant). #"
    echo "###############################################################"
    print_matrix "Same C ladder — only the field op differs" "${FIELDOP_ISO[@]}"
    print_ratio_matrix "ucode/C-ladder" "${FIELDOP_ISO[@]}"

    echo
    echo "Naming legend (label = ladder / field-op backend):"
    echo "    ours/ucode     — all-in-one inline-asm register-chained 5×51 ladder + microcode field ops (canonical)"
    echo "    ucode/C-ladder — microcode field ops on the SAME C ladder as hand-C/fiat/cryptopt (field-op isolation only)"
    echo "    amd64-64/asm   — Bernstein/Schwabe whole-stack x86-64 asm (4x64 saturated; lib25519's Goldmont pick)"
    echo "    amd64-64/ucode — amd64-64's framework + C ladder + 4×64 microcode field ops (hybrid)"
    echo "    amd64-51/asm   — Bernstein/Schwabe whole-stack x86-64 asm (5x51 unsaturated)"
    echo "    amd64-51/ucode — amd64-51's framework + inline-asm ladder + 5×51 microcode field ops (hybrid)"
    echo "    ours/cryptopt  — our C ladder + CryptOpt Goldmont-tuned asm field ops"
    echo "    ours/fiat      — our C ladder + fiat-crypto autogen C field ops"
    echo "    ours/hand-C    — our C ladder + hand-written C with __uint128_t"
    echo "    donna_c64      — donna's whole-stack portable C"
    echo
}

# ───────────────────────────── markdown table ──────────────────────────────

# md_matrix <title> <contender...> — markdown twin of print_matrix.
md_matrix() {
    local title="$1"; shift
    local cols=("$@")

    echo
    echo "### $title"
    echo
    echo "_median cycles. **bold** = best (lowest-median) config in that column._"
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
            local val="${cell_med["$cfg|$label"]:-}"
            if [ -z "$val" ]; then
                printf " — |"
            elif [ "$cfg" = "${best_med_cfg[$label]}" ]; then
                printf " **%s** |" "$val"
            else
                printf " %s |" "$val"
            fi
        done
        echo
    done
}

# md_ratio_matrix <focus> <contender...> — markdown twin of print_ratio_matrix.
# ratio = median(competitor) / median(focus); >1 ⇒ focus is faster (wins).
md_ratio_matrix() {
    local focus="$1"; shift
    local cols=("$@")
    local comps=() c
    for c in "${cols[@]}"; do [ "$c" != "$focus" ] && comps+=("$c"); done

    echo
    echo "### Does \`$(short_label "$focus")\` win?"
    echo
    echo "_ratio = other ÷ $(short_label "$focus") (median cycles). **>1 ⇒ $(short_label "$focus") is faster** (wins); <1 ⇒ slower. **bold** = geomean._"
    echo

    printf "| Config |"
    for label in "${comps[@]}"; do printf " %s |" "$(short_label "$label")"; done
    echo
    printf "|---|"
    for _ in "${comps[@]}"; do printf '%s' "---:|"; done
    echo

    for cfg in "${ran_cfgs[@]}"; do
        printf "| %s |" "$cfg"
        local fv="${cell_med["$cfg|$focus"]:-}"
        for label in "${comps[@]}"; do
            local v="${cell_med["$cfg|$label"]:-}"
            if [ -z "$v" ] || [ -z "$fv" ]; then
                printf " — |"
            else
                local r; r=$(awk -v a="$v" -v b="$fv" 'BEGIN{printf "%.3f", a/b}')
                printf " %s |" "$r"
            fi
        done
        echo
    done

    printf "| **geomean** |"
    for label in "${comps[@]}"; do
        local gm
        gm=$(
            for cfg in "${ran_cfgs[@]}"; do
                local v="${cell_med["$cfg|$label"]:-}"
                local fv="${cell_med["$cfg|$focus"]:-}"
                [ -n "$v" ] && [ -n "$fv" ] && awk -v a="$v" -v b="$fv" 'BEGIN{printf "%.6f\n", a/b}'
            done | awk '{ s += log($1); n++ } END { if (n>0) printf "%.3f", exp(s/n); else printf "—" }'
        )
        printf " **%s** |" "$gm"
    done
    echo
}

# md_dispersion <contender...> — per-contender best-config median alongside the
# min and the p10–p90 spread, so run-to-run dispersion is visible next to the
# headline median. Values are taken at each contender's best-median config (the
# same config the headline median comes from), over the benchmark's reps.
md_dispersion() {
    local cols=("$@") label cfg md mn a b spread
    echo
    echo "### Dispersion at each contender's best config"
    echo
    echo "_Median is the headline; min and the p10–p90 range show run-to-run spread at that config. A tight p90−p10 relative to the inter-contender gaps means the ranking is not noise._"
    echo
    echo "| contender | median | min | p10 | p90 | p90−p10 | best config |"
    echo "|---|---:|---:|---:|---:|---:|---|"
    for label in "${cols[@]}"; do
        cfg="${best_med_cfg[$label]:-}"
        md="${best_med[$label]:-}"; mn="${cell_min[$cfg|$label]:-}"
        a="${cell_p10[$cfg|$label]:-}"; b="${cell_p90[$cfg|$label]:-}"
        spread="—"
        [[ "$a" =~ ^[0-9]+$ && "$b" =~ ^[0-9]+$ ]] && spread=$((b - a))
        echo "| $(short_label "$label") | ${md:-—} | ${mn:-—} | ${a:-—} | ${b:-—} | $spread | ${cfg:-—} |"
    done
}

# emit_results_md <outfile> — write the markdown report (host info, legend,
# the median table, the dispersion table, and the three "does it win?" ratio
# tables) to <outfile>.
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
        echo "**Delivered core freq:** ${DELIVERED_FREQ_MHZ:-n/a} MHz · **TSC (RDTSC) rate:** ${TSC_FREQ_MHZ:-n/a} MHz · **correction f_core/f_TSC:** ${CYCLE_CORRECTION:-n/a} (aperf/mperf under load, verified before the sweep; comparative **ratios are invariant** to this factor, multiply **absolute** cycle counts by it for true core cycles)"
        echo "**Configs that ran:** ${#ran_cfgs[@]} / ${#ACTIVE_CONFIGS[@]}"
        echo "**Pipeline:** \`taskset -c 0 ./full_curve25519_inline2_static\` (+ amd64-64/ucode) for each (compiler, -O) combo"
        echo "**Metric:** median cycles per X25519 (headline; best config per contender in **bold**). Min and the p10–p90 spread are in the dispersion table below."
        echo
        echo "## Contender legend"
        echo
        echo "| label | backend |"
        echo "|---|---|"
        echo "| ours/ucode     | all-in-one inline-asm register-chained 5×51 ladder + microcode field ops (the canonical implementation) |"
        echo "| ucode/C-ladder | microcode field ops on the SAME C ladder as hand-C/fiat/cryptopt (field-op isolation only — not a headline contender) |"
        echo "| amd64-64/asm   | Bernstein–Schwabe whole-stack x86-64 asm (4x64 saturated; lib25519's Goldmont pick) |"
        echo "| amd64-64/ucode | amd64-64's framework + C ladder + 4×64 microcode field ops (hybrid) |"
        echo "| amd64-51/asm   | Bernstein–Schwabe whole-stack x86-64 asm (5x51 unsaturated) |"
        echo "| amd64-51/ucode | amd64-51's framework + inline-asm ladder + 5×51 microcode field ops (hybrid) |"
        echo "| ours/cryptopt  | our C ladder + CryptOpt Goldmont-tuned asm field ops |"
        echo "| ours/fiat      | our C ladder + fiat-crypto autogen C field ops |"
        echo "| ours/hand-C    | our C ladder + hand-written C with \`__uint128_t\` |"
        echo "| donna_c64      | donna whole-stack portable C |"
        echo
        echo "---"
        md_matrix "X25519 end-to-end" "${TABLE[@]}"
        md_dispersion "${TABLE[@]}"
        echo
        echo "---"
        echo
        echo "## Does microcode win?"
        echo
        echo "Ratio = other ÷ the ucode variant (median cycles). **>1 ⇒ the ucode variant is faster** than that competitor; the **geomean** row is the single number to quote."
        md_ratio_matrix "ours/ucode"     "${TABLE[@]}"
        md_ratio_matrix "amd64-51/ucode" "${TABLE[@]}"
        md_ratio_matrix "amd64-64/ucode" "${TABLE[@]}"
        echo
        echo "---"
        echo
        echo "## Same-ladder field-op isolation (attributes the win to microcode)"
        echo
        echo "All four use the **identical C Montgomery ladder** (driver, invert, cswap, pack held constant); only \`fe_mul\`/\`fe_sq\` differ. \`ucode/C-ladder\` is the microcode field ops on that same C ladder (NOT the inline ladder of the headline \`ours/ucode\`), so this isolates the field-op backend end-to-end with zero confounds. The ratio's **geomean** is the clean per-paper number."
        md_matrix "Same C ladder — only the field op differs" "${FIELDOP_ISO[@]}"
        md_ratio_matrix "ucode/C-ladder" "${FIELDOP_ISO[@]}"
    } > "$out"
}
