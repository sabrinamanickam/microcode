# lib/freq_guard.sh — CPU frequency sanity check.
#
# Sourced by bench_supercop_matrix.sh. Defines check_cpu_frequency(); the
# orchestrator calls it. No top-level work happens here.
#
# Why this matters
# ----------------
# The benchmark times with RDTSC, which counts at the fixed TSC/nominal
# frequency (1.10 GHz on the N3350), NOT the actual core clock. RDTSC ticks
# only equal true core cycles when the core runs at that base frequency:
#
#     RDTSC_ticks = true_core_cycles × (F_tsc / F_core)
#
# If the governor lets the core turbo to ~2.4 GHz, every reported cycle count
# is scaled down by ~F_tsc/F_core ≈ 0.46 — e.g. a real 30k-cycle X25519 shows
# as ~14k. Absolute cycles/op are then wrong (and unpinned ondemand also ramps
# frequency mid-run, making numbers noisy). Pin to base before measuring.
#
# Override with ALLOW_UNPINNED=1 if you really intend an unpinned run.

check_cpu_frequency() {
    if [[ "${ALLOW_UNPINNED:-0}" == "1" ]]; then
        echo "WARNING: ALLOW_UNPINNED=1 set — skipping frequency guard."
        echo "         Absolute cycle counts will be scaled by F_tsc/F_core and are NOT true cycles/op."
        return 0
    fi

    # ── Turbo/boost must be OFF so the core cannot burst above base (P1). On this
    # part base == max non-turbo frequency == the TSC rate, so disabling turbo
    # makes RDTSC ticks equal true cycles even under sustained load. Support both
    # cpufreq drivers:
    #   intel_pstate:  /sys/.../intel_pstate/no_turbo   (1 = turbo OFF, good)
    #   acpi-cpufreq:  /sys/.../cpufreq/boost            (0 = boost OFF, good)
    local turbo_on=0 turbo_state="unknown" nt boost
    if [[ -r /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        nt=$(cat /sys/devices/system/cpu/intel_pstate/no_turbo)
        turbo_state="intel_pstate/no_turbo=$nt"
        [[ "$nt" == "1" ]] || turbo_on=1
    elif [[ -r /sys/devices/system/cpu/cpufreq/boost ]]; then
        boost=$(cat /sys/devices/system/cpu/cpufreq/boost)
        turbo_state="cpufreq/boost=$boost"
        [[ "$boost" == "0" ]] || turbo_on=1
    else
        turbo_state="no turbo/boost knob found (assuming fixed frequency)"
    fi

    if (( turbo_on )); then
        echo ""
        echo "ERROR: CPU turbo/boost is ENABLED ($turbo_state)."
        echo "       The core can burst above base while RDTSC keeps counting at the"
        echo "       TSC (base) rate, so reported cycle counts are scaled and invalid."
        echo ""
        echo "  Disable turbo, then re-run:"
        if [[ -e /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
            echo "    echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo"
        else
            echo "    echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost"
        fi
        echo ""
        echo "  (To bypass intentionally: ALLOW_UNPINNED=1 \$0)"
        exit 1
    fi
    echo "Turbo/boost OK: disabled ($turbo_state) — core cannot burst above base."

    # TSC/base frequency in kHz, parsed from the model name's "@ X.YZGHz" tag.
    local base_khz tsc_ghz
    tsc_ghz=$(grep -m1 'model name' /proc/cpuinfo | sed -n 's/.*@ \([0-9.]*\)GHz.*/\1/p')
    if [[ -z "$tsc_ghz" ]]; then
        echo "NOTE: could not parse base frequency from /proc/cpuinfo; assuming 1.10 GHz."
        tsc_ghz="1.10"
    fi
    base_khz=$(awk "BEGIN { printf \"%d\", $tsc_ghz * 1000000 }")

    # Tolerance: ±6% of base covers the 1.094 GHz pinned target vs 1.10 nominal.
    local lo hi
    lo=$(awk "BEGIN { printf \"%d\", $base_khz * 0.94 }")
    hi=$(awk "BEGIN { printf \"%d\", $base_khz * 1.06 }")

    local bad=0 c gov cur
    for c in /sys/devices/system/cpu/cpu[0-9]*/cpufreq; do
        [[ -d "$c" ]] || continue
        gov=$(cat "$c/scaling_governor" 2>/dev/null || echo "?")
        cur=$(cat "$c/scaling_cur_freq" 2>/dev/null || echo 0)
        if (( cur < lo || cur > hi )); then
            echo "  ${c%/cpufreq}: governor=$gov  cur=$((cur/1000)) MHz  (outside ${tsc_ghz} GHz ±6%)"
            bad=1
        fi
    done

    if (( bad )); then
        echo ""
        echo "ERROR: CPU is not pinned to its base/TSC frequency (~${tsc_ghz} GHz)."
        echo "       RDTSC counts at the TSC rate, so unpinned runs report wrong cycles/op"
        echo "       (a real 30k-cycle X25519 shows as ~14k at 2.4 GHz turbo)."
        echo ""
        echo "  Pin to base, then re-run:"
        echo "    sudo cpupower frequency-set -g userspace && sudo cpupower frequency-set -f ${tsc_ghz}GHz"
        echo "  or without cpupower:"
        echo "    echo userspace | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
        echo "    echo ${base_khz} | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_setspeed"
        echo ""
        echo "  (To bypass intentionally: ALLOW_UNPINNED=1 \$0)"
        exit 1
    fi

    echo "CPU frequency OK: all cores pinned near ${tsc_ghz} GHz base — RDTSC ticks ≈ true cycles."

    # Belt-and-suspenders: the check above reads scaling_cur_freq, which is the
    # frequency the governor *requests*. Now measure the frequency actually
    # DELIVERED under load, and the true TSC (RDTSC) rate, via turbostat (below).
    check_effective_freq "$tsc_ghz"
}

# ── globals set by check_effective_freq, read by the results-header writers ──
DELIVERED_FREQ_MHZ=""   # Bzy_MHz: core clock delivered under load
TSC_FREQ_MHZ=""         # TSC_MHz: the actual rate RDTSC ticks at
CYCLE_CORRECTION=""     # f_core / f_TSC : multiply RDTSC ticks by this for true cycles
PRE_DELIVERED_FREQ_MHZ=""   # the same three, as measured BEFORE the sweep
PRE_TSC_FREQ_MHZ=""
PRE_CYCLE_CORRECTION=""

# measure_freq — run a ~2 s busy load on the benchmark core and report the
# delivered core clock and the actual RDTSC rate over it, as "Bzy_MHz TSC_MHz"
# on stdout. Returns non-zero if turbostat is missing, cannot run, or its
# output cannot be parsed. Factored out so the SAME measurement can be taken
# before the sweep (check_effective_freq) and again after it
# (recheck_effective_freq) — a mid-sweep thermal throttle is invisible to a
# single up-front check.
measure_freq() {
    local out bzy tsc core="${BENCH_CORE:-0}"
    command -v turbostat >/dev/null 2>&1 || return 1
    out=$(sudo turbostat --quiet --show Core,CPU,Bzy_MHz,TSC_MHz \
            -- taskset -c "$core" bash -c \
               'e=$((SECONDS+2)); while [ $SECONDS -lt $e ]; do :; done' 2>&1) || return 1
    # Rows are: Core CPU Bzy_MHz TSC_MHz. Per-core rows carry a numeric CPU;
    # the package summary row carries '-'.
    read -r bzy tsc < <(awk -v c="$core" '$2==c{print $3, $4; exit}' <<<"$out")
    [[ -n "$bzy" && -n "$tsc" ]] && (( tsc > 0 )) || return 1
    echo "$bzy $tsc"
}

# check_effective_freq <tsc_ghz> — measure the DELIVERED core frequency and the
# actual TSC (RDTSC) rate under load with turbostat, which reads aperf/mperf and
# the TSC over a controlled interval far more reliably than hand-rolled rdmsr.
#
# On this part the base P-state (BCLK 100 MHz x 11 = 1100 MHz) does NOT equal the
# crystal-derived TSC nominal (19.2 MHz x 57 = 1094.4 MHz); the P-state grid is
# 100 MHz, so no P-state matches the TSC exactly and the closest (1100) sits
# ~0.5% above it. That offset is COMMON-MODE: it scales every contender's cycle
# count identically, so all reported RATIOS are invariant to it; only ABSOLUTE
# cycle counts need multiplying by f_core/f_TSC. We therefore RECORD the
# correction and abort only on a LARGE (>3%) deviation, which would signal turbo
# bursting (despite no_turbo) or thermal throttling, not the benign offset.
# Needs root + turbostat; if absent it discloses a skip (no_turbo already caps
# the core at base). Sets DELIVERED_FREQ_MHZ / TSC_FREQ_MHZ / CYCLE_CORRECTION.
check_effective_freq() {
    local tsc_ghz="${1:-1.10}"

    if ! command -v turbostat >/dev/null 2>&1; then
        echo "NOTE: turbostat absent — delivered-frequency (aperf/mperf) check SKIPPED"
        echo "      (no_turbo caps the core at base, so it cannot burst above it)."
        return 0
    fi

    local bzy tsc
    if ! read -r bzy tsc < <(measure_freq); then
        echo "NOTE: turbostat unavailable/unparsable — delivered-frequency check SKIPPED."
        return 0
    fi
    DELIVERED_FREQ_MHZ="$bzy"; TSC_FREQ_MHZ="$tsc"
    CYCLE_CORRECTION=$(awk -v b="$bzy" -v t="$tsc" 'BEGIN{printf "%.5f", b/t}')

    echo "Delivered core freq: ${bzy} MHz;  TSC (RDTSC) rate: ${tsc} MHz;  f_core/f_TSC = ${CYCLE_CORRECTION}"
    echo "  RDTSC undercounts true cycles by this factor: RATIOS are invariant to it;"
    echo "  multiply ABSOLUTE cycle counts by ${CYCLE_CORRECTION} for true core cycles."

    # Abort only on a LARGE deviation (turbo despite no_turbo, or thermal throttle),
    # never the sub-1% base-P-state/TSC quantization offset.
    if awk -v c="$CYCLE_CORRECTION" 'BEGIN{d=c-1; if(d<0)d=-d; exit !(d>0.03)}'; then
        echo ""
        echo "ERROR: core is ${CYCLE_CORRECTION}x the TSC rate (>3% off) — turbo bursting or"
        echo "       thermal throttling, not the benign P-state/TSC offset. Cool / re-pin, re-run."
        echo "       (To bypass intentionally: ALLOW_UNPINNED=1 \$0)"
        exit 1
    fi
    echo "Delivered-frequency OK: core at base P-state, stable (|f_core/f_TSC - 1| <= 3%)."

    # Remember the pre-sweep reading so recheck_effective_freq can compare.
    PRE_DELIVERED_FREQ_MHZ="$bzy"; PRE_TSC_FREQ_MHZ="$tsc"
    PRE_CYCLE_CORRECTION="$CYCLE_CORRECTION"
}

# ── globals set by recheck_effective_freq, read by the results-header writers ─
POST_DELIVERED_FREQ_MHZ=""   # Bzy_MHz measured AFTER the sweep
POST_TSC_FREQ_MHZ=""         # TSC_MHz measured AFTER the sweep
FREQ_DRIFT_PCT=""            # 100 * (post_corr/pre_corr - 1)
FREQ_DRIFT_STATUS="not checked"

# recheck_effective_freq — re-measure the delivered core frequency AFTER the
# sweep and compare with the pre-sweep reading.
#
# The up-front guard proves the machine was pinned when the sweep STARTED. It
# says nothing about a thermal throttle part-way through a ~20-minute run on a
# passively cooled part, which would silently inflate the tick counts of every
# configuration measured after it. This closes that gap by bracketing the
# sweep. It never aborts — the measurements are already taken by this point,
# and discarding them helps nobody — but a drift beyond 1% is recorded in
# FREQ_DRIFT_STATUS and propagated into RESULTS.md and PAPER_TABLES.md so the
# artifact itself carries the warning.
recheck_effective_freq() {
    local bzy tsc corr
    if ! read -r bzy tsc < <(measure_freq); then
        FREQ_DRIFT_STATUS="not checked (turbostat unavailable)"
        echo "NOTE: post-sweep delivered-frequency check SKIPPED (turbostat unavailable)."
        return 0
    fi
    POST_DELIVERED_FREQ_MHZ="$bzy"; POST_TSC_FREQ_MHZ="$tsc"
    corr=$(awk -v b="$bzy" -v t="$tsc" 'BEGIN{printf "%.5f", b/t}')

    if [[ -z "${PRE_CYCLE_CORRECTION:-}" ]]; then
        FREQ_DRIFT_STATUS="post-sweep only: f_core/f_TSC = $corr"
        echo "Post-sweep delivered freq: ${bzy} MHz; TSC ${tsc} MHz; f_core/f_TSC = $corr"
        return 0
    fi

    FREQ_DRIFT_PCT=$(awk -v a="$PRE_CYCLE_CORRECTION" -v b="$corr" \
                         'BEGIN{printf "%+.3f", 100*(b/a - 1)}')
    echo "Post-sweep delivered freq: ${bzy} MHz; TSC ${tsc} MHz; f_core/f_TSC = $corr"
    echo "  pre-sweep f_core/f_TSC = $PRE_CYCLE_CORRECTION -> drift ${FREQ_DRIFT_PCT}%"

    if awk -v d="$FREQ_DRIFT_PCT" 'BEGIN{if(d<0)d=-d; exit !(d>1.0)}'; then
        FREQ_DRIFT_STATUS="DRIFTED ${FREQ_DRIFT_PCT}% (pre ${PRE_CYCLE_CORRECTION} -> post ${corr})"
        echo ""
        echo "WARNING: the core clock drifted ${FREQ_DRIFT_PCT}% relative to the TSC during the"
        echo "         sweep — thermal throttling or a governor change. Configurations measured"
        echo "         late in the sweep are NOT comparable with early ones. Let the machine"
        echo "         cool, re-pin, and re-run before using these numbers in the paper."
        echo ""
    else
        FREQ_DRIFT_STATUS="stable (${FREQ_DRIFT_PCT}% over the sweep)"
        echo "Post-sweep check OK: core/TSC ratio stable to within 1% across the whole sweep."
    fi
}
