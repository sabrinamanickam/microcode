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
}
