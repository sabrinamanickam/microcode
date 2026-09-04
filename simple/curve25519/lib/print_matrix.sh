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
        "a51ops/C-ladder")   echo "a51op/Clad" ;;
        "amd64-51/asm-Clad")   echo "a51/asmCld" ;;
        "amd64-51/ucode-Clad") echo "a51/ucCld"  ;;
        "donna_c64")         echo "donna"       ;;
        "amd64-51/asm")      echo "a51/asm"     ;;
        "amd64-51/ucode")    echo "a51/ucode"   ;;
        "amd64-64/asm")      echo "a64/asm"     ;;
        "amd64-64/ucode")    echo "a64/ucode"   ;;
        "amd64-64/asm-Clad") echo "a64/asmCld"  ;;
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

# ═══════════════════════════════════════════════════════════════════════════
# CLAIM TABLES — one table per question, each ≤6 rows, each stating what is
# held constant. The 24-config sweep is methodology (SUPERCOP best-per-
# contender discipline), not a finding, so it lives in Appendix A.
# ═══════════════════════════════════════════════════════════════════════════

# commafy <int> — thousands separators, so 5-7 digit cycle counts stay readable.
commafy() { echo "$1" | sed -E ':a;s/([0-9]+)([0-9]{3})/\1,\2/;ta'; }

# backend_label <contender> — descriptive name for the claim tables. The short
# labels are for the wide appendix matrices; a headline table should not make
# the reader decode "uc/Clad".
backend_label() {
    case "$1" in
        "ucode/C-ladder")  echo "microcode 5×51 (this work)"        ;;
        "a51ops/C-ladder") echo "amd64-51 asm (Bernstein–Schwabe)"  ;;
        "ours/cryptopt")   echo "CryptOpt (superoptimized asm)"     ;;
        "ours/fiat")       echo "fiat-crypto (verified C)"          ;;
        "ours/hand-C")     echo "hand-written C (\`__uint128_t\`)"   ;;
        "ours/ucode")      echo "microcode 5×51 + inline-asm ladder (this work)" ;;
        "amd64-64/asm")    echo "amd64-64 asm (Bernstein–Schwabe, 4×64)" ;;
        "amd64-51/asm")    echo "amd64-51 asm (Bernstein–Schwabe, 5×51)" ;;
        "amd64-51/ucode")  echo "amd64-51 framework + microcode"    ;;
        "donna_c64")       echo "donna c64 (portable C)"            ;;
        *)                 short_label "$1"                         ;;
    esac
}

# geo_ratio <label> <ref> — geomean over configs of median(label)/median(ref).
geo_ratio() {
    local label="$1" ref="$2" cfg v fv
    for cfg in "${ran_cfgs[@]}"; do
        v="${cell_med["$cfg|$label"]:-}"; fv="${cell_med["$cfg|$ref"]:-}"
        [ -n "$v" ] && [ -n "$fv" ] && awk -v a="$v" -v b="$fv" 'BEGIN{printf "%.6f\n", a/b}'
    done | awk '{ s += log($1); n++ } END { if (n>0) printf "%.3f", exp(s/n); else printf "—" }'
}

# ratio2 <a> <b> — best-config median(a)/median(b), 3dp.
ratio2() {
    local a="${best_med[$1]:-}" b="${best_med[$2]:-}"
    [ -z "$a" ] || [ -z "$b" ] && { printf '—'; return; }
    awk -v x="$a" -v y="$b" 'BEGIN{printf "%.3f", x/y}'
}

# md_claim_table <ref> <label...> — Table 1. One row per field-op backend,
# ascending by best-config median, with the ratio against <ref>.
md_claim_table() {
    local ref="$1"; shift
    local cols=("$@") label md sorted
    sorted=$(for label in "${cols[@]}"; do
                 md="${best_med[$label]:-}"
                 [ -n "$md" ] && printf '%s\t%s\n' "$md" "$label"
             done | sort -n)

    echo "| field-op backend | cyc/X25519 | ÷ microcode | geomean | best config |"
    echo "|---|---:|---:|---:|---|"
    while IFS=$'\t' read -r md label; do
        [ -z "$label" ] && continue
        if [ "$label" = "$ref" ]; then
            echo "| **$(backend_label "$label")** | **$(commafy "$md")** | — | — | ${best_med_cfg[$label]:-—} |"
        else
            echo "| $(backend_label "$label") | $(commafy "$md") | $(ratio2 "$label" "$ref") | $(geo_ratio "$label" "$ref") | ${best_med_cfg[$label]:-—} |"
        fi
    done <<< "$sorted"
}

# md_decomp <asm> <asmClad> <ucClad> <ucode|""> — Tables 2/3. The corners of a
# framework's ladder square. Each row changes ONE thing from the row above, and
# the per-step ratios are oriented as speedups (>1 = this row is faster), so
# they multiply out to the end-to-end ratio — printed as a consistency check.
md_decomp() {
    local A="$1" B="$2" C="$3" D="${4:-}"
    local a="${best_med[$A]:-}" b="${best_med[$B]:-}" c="${best_med[$C]:-}" d=""
    [ -n "$D" ] && d="${best_med[$D]:-}"

    echo "| variant | ladder | field ops | cyc/X25519 | × vs row above | what changed |"
    echo "|---|---|---|---:|---:|---|"
    echo "| \`$(short_label "$A")\` | qhasm, monolithic | qhasm asm | $(commafy "$a") | — | baseline |"
    echo "| \`$(short_label "$B")\` | C, per-op calls | qhasm asm | $(commafy "$b") | $(awk -v x="$a" -v y="$b" 'BEGIN{printf "%.3f", x/y}') | ladder: qhasm → C (fusion lost) |"
    echo "| \`$(short_label "$C")\` | C, per-op calls | **microcode** | $(commafy "$c") | **$(awk -v x="$b" -v y="$c" 'BEGIN{printf "%.3f", x/y}')** | **field ops: asm → microcode** |"
    if [ -n "$d" ]; then
        echo "| \`$(short_label "$D")\` | inline-asm, chained | microcode | $(commafy "$d") | $(awk -v x="$c" -v y="$d" 'BEGIN{printf "%.3f", x/y}') | ladder: C → register-chained asm |"
    fi
    echo
    echo "_\`× vs row above\` > 1 means that row is **faster** than the one above it._"
    echo
    if [ -n "$d" ]; then
        echo "**The field-op step is the paper's quantity:** $(awk -v x="$b" -v y="$c" 'BEGIN{printf "%.3f", x/y}')× faster (geomean $(geo_ratio "$B" "$C")×), with the ladder **and** the framework held constant."
        echo
        echo "**Consistency check.** The steps are multiplicative, so they must compose to the"
        echo "measured end-to-end ratio:"
        echo
        echo '```'
        awk -v a="$a" -v b="$b" -v c="$c" -v d="$d" 'BEGIN{
            printf "  %.3f (ladder) x %.3f (field ops) x %.3f (chaining)  =  %.5f\n", a/b, b/c, c/d, (a/b)*(b/c)*(c/d);
            printf "  measured  %d / %d                              =  %.5f\n", a, d, a/d }'
        echo '```'
    else
        echo "**The field-op step is the paper's quantity:** microcode is $(awk -v x="$c" -v y="$b" 'BEGIN{printf "%.3f", x/y}')× **slower** than the asm it replaces (geomean $(geo_ratio "$C" "$B")×), with the ladder held constant — against $(awk -v x="$c" -v y="$a" 'BEGIN{printf "%.3f", x/y}')× if the ladder rewrite is wrongly charged to the field ops."
    fi
}

# md_standing <label...> — Table 4. Flat ascending list; orientation only.
md_standing() {
    local cols=("$@") label md
    echo "| implementation | cyc/X25519 | best config |"
    echo "|---|---:|---|"
    for label in "${cols[@]}"; do
        md="${best_med[$label]:-}"
        [ -n "$md" ] && printf '%s\t%s\n' "$md" "$label"
    done | sort -n | while IFS=$'\t' read -r md label; do
        [ -z "$label" ] && continue
        if [ "$label" = "ours/ucode" ]; then
            echo "| **$(backend_label "$label")** | **$(commafy "$md")** | ${best_med_cfg[$label]:-—} |"
        else
            echo "| $(backend_label "$label") | $(commafy "$md") | ${best_med_cfg[$label]:-—} |"
        fi
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
        echo "**Post-sweep frequency check:** ${FREQ_DRIFT_STATUS:-not checked}${POST_DELIVERED_FREQ_MHZ:+ · delivered ${POST_DELIVERED_FREQ_MHZ} MHz / TSC ${POST_TSC_FREQ_MHZ} MHz after the sweep} (the pre-sweep guard proves the machine was pinned when the sweep started; this proves it stayed pinned throughout)"
        echo "**Core isolation:** ${ISOLATION_STATUS:-not configured} · nohz_full/isolcpus: ${NOHZ_FULL_STATUS:-unknown}"
        echo "**Runs per config:** ${RUNS_PER_CONFIG:-1} (recorded median is the median of those runs; worst run-to-run spread ${REPRO_WORST_PCT:-n/a}% at ${REPRO_WORST_WHERE:-—})"
        echo "**Timing:** contenders measured INTERLEAVED (round-robin, one repetition of each per round) so measurement order cannot bias the ranking"
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
        echo "| a51ops/C-ladder | amd64-51 hand-asm field ops on that SAME C ladder — the CONTROL for the field-op claim (see CONTROLS.md) |"
        echo "| amd64-51/asm-Clad | amd64-51's framework + C ladderstep.c + amd64-51's own asm field ops — the ladder-tax control for 5×51 (see CONTROLS.md) |"
        echo "| amd64-51/ucode-Clad | amd64-51's framework + that SAME C ladderstep.c + 5×51 microcode field ops — pairs with asm-Clad to isolate the field ops inside one framework |"
        echo "| amd64-64/asm   | Bernstein–Schwabe whole-stack x86-64 asm (4x64 saturated; lib25519's Goldmont pick) |"
        echo "| amd64-64/ucode | amd64-64's framework + C ladder + 4×64 microcode field ops (hybrid) |"
        echo "| amd64-64/asm-Clad | amd64-64's framework + the SAME C ladder as amd64-64/ucode + amd64-64's own asm field ops — the CONTROL that separates the field-op backend from the ladder rewrite (see CONTROLS.md) |"
        echo "| amd64-51/asm   | Bernstein–Schwabe whole-stack x86-64 asm (5x51 unsaturated) |"
        echo "| amd64-51/ucode | amd64-51's framework + inline-asm ladder + 5×51 microcode field ops (hybrid) |"
        echo "| ours/cryptopt  | our C ladder + CryptOpt Goldmont-tuned asm field ops |"
        echo "| ours/fiat      | our C ladder + fiat-crypto autogen C field ops |"
        echo "| ours/hand-C    | our C ladder + hand-written C with \`__uint128_t\` |"
        echo "| donna_c64      | donna whole-stack portable C |"
        echo
        echo "---"
        echo
        echo "## How to read this"
        echo
        echo "Four tables, one question each. Every caption says what is held constant —"
        echo "that is the only thing that makes a ratio mean anything here."
        echo
        echo "| table | question |"
        echo "|---|---|"
        echo "| 1 | Is microcode field arithmetic faster than the best ISA-level code for the same representation? |"
        echo "| 2 | Where does the end-to-end 5×51 number come from? |"
        echo "| 3 | Does the benefit survive in a saturated representation? |"
        echo "| 4 | How does the whole implementation stand against shipped code? (orientation only) |"
        echo
        echo "Selection rule throughout: **best median per contender across all"
        echo "${#ran_cfgs[@]} (compiler, -O) configs** — SUPERCOP's own discipline. The full"
        echo "per-config sweep is Appendix A."
        echo
        echo "---"
        echo
        echo "## Table 1 — Field arithmetic: microcode vs the best ISA-level code"
        echo
        echo "**Held constant:** the entire implementation except \`fe_mul\`/\`fe_sq\` — identical C"
        echo "Montgomery ladder, driver, Fermat inversion, cswap and packing, all compiled into"
        echo "the same binary and timed in the same process. Only the field-op backend differs."
        echo
        echo "**This is the paper's claim.** It is the one comparison in which nothing but the"
        echo "field arithmetic changes."
        echo
        md_claim_table "ucode/C-ladder" "${FIELDOP_ISO[@]}"
        echo
        echo "_Note: fiat-crypto's C beating Bernstein–Schwabe's hand asm here is real, not"
        echo "an error. amd64-51's \`fe25519_mul.S\` is written to be inlined into its qhasm"
        echo "ladder, and pays a penalty when called per-op from C. Table 2 measures that"
        echo "penalty directly (row 2), which is why this table is not the whole story._"
        echo
        echo "---"
        echo
        echo "## Table 2 — Where the end-to-end 5×51 number comes from"
        echo
        echo "**Held constant:** amd64-51's framework (driver, inversion, pack, cswap) across all"
        echo "four rows. Each row changes exactly one thing from the row above."
        echo
        echo "The microcode field ops are worth more than the end-to-end figure shows, because"
        echo "part of the win is handed back: amd64-51's asm gains from being *fused into* its"
        echo "monolithic \`ladderstep.S\`, and the 128-triad patch RAM forbids microcode from"
        echo "holding a whole ladder step. Our register-chained inline-asm ladder recovers some"
        echo "of it."
        echo
        md_decomp "amd64-51/asm" "amd64-51/asm-Clad" "amd64-51/ucode-Clad" "amd64-51/ucode"
        echo
        echo "---"
        echo
        echo "## Table 3 — Does it survive in a saturated representation?"
        echo
        echo "**Held constant:** amd64-64's framework across all three rows; rows 2 and 3 share"
        echo "the same C \`ladderstep.c\` object source, so row 3 differs from row 2 only in the"
        echo "field ops."
        echo
        echo "No. The 4×64 saturated multiplier costs 75 triads, leaving no room for a dedicated"
        echo "squarer under the 128-triad cap, so squaring is \`mul(a,a)\`. Microcode wins inside a"
        echo "representation it can hold; it cannot adopt the better algorithm. This is the"
        echo "headroom result, and it is why the end-to-end table has us losing to amd64-64."
        echo
        md_decomp "amd64-64/asm" "amd64-64/asm-Clad" "amd64-64/ucode" ""
        echo
        echo "---"
        echo
        echo "## Table 4 — End-to-end standing (orientation, not the claim)"
        echo
        echo "**Held constant:** nothing — these are whole implementations differing in"
        echo "representation, ladder, inversion and field ops at once. Useful for placing the"
        echo "work against shipped code; useless for attributing the difference to microcode."
        echo "For that, see Table 1."
        echo
        md_standing "${STANDING[@]}"
        echo
        md_dispersion "${TABLE[@]}"
        echo
        echo "---"
        echo
        echo "# Appendix A — full per-config sweep"
        echo
        echo "The raw ${#ran_cfgs[@]}-config matrices behind the best-per-contender numbers above."
        echo "Present so the selection rule can be audited and so per-compiler behaviour is"
        echo "visible; not intended to be read row by row."
        md_matrix "A.1 — X25519 end-to-end, every contender" "${TABLE[@]}"
        md_matrix "A.2 — Same C ladder, only the field op differs" "${FIELDOP_ISO[@]}"
        echo
        echo "### A.3 — Per-config ratios"
        echo
        md_ratio_matrix "ucode/C-ladder" "${FIELDOP_ISO[@]}"
        md_ratio_matrix "ours/ucode"     "${STANDING[@]}"
        echo
        echo "### A.4 — Uncontrolled ratios (superseded)"
        echo
        echo "These compare a microcode hybrid against its asm baseline **without** holding the"
        echo "ladder constant, so they attribute the ladder rewrite to the field ops. Retained"
        echo "for auditability only — Tables 2 and 3 are the correct form of these comparisons."
        md_ratio_matrix "amd64-51/ucode" "${TABLE[@]}"
        md_ratio_matrix "amd64-64/ucode" "${TABLE[@]}"
    } > "$out"
}
