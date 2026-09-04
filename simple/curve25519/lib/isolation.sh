# lib/isolation.sh — keep the OS off the benchmark core for the duration of a sweep.
#
# Sourced by bench_supercop_matrix.sh. Defines setup_isolation() /
# restore_isolation() and exports BENCH_RUN, the command prefix every timed
# binary is launched with. No top-level work happens here.
#
# What this fixes
# ---------------
# The sweep pins to a core with taskset, but nothing stops the kernel from
# scheduling other work or delivering device interrupts there. The observed
# p10-p90 spread is a near-constant ~6,500-7,000 ticks for EVERY contender
# regardless of its cost — the signature of an external perturbation, not of
# contender behaviour. Medians sit at the interference-free floor, so the
# ranking was never at risk, but the tail is noise we can largely remove.
#
# Three levers, in decreasing order of what they buy:
#
#   1. nohz_full / isolcpus on the kernel command line. Removes the periodic
#      local-timer tick, which is the dominant perturbation. REQUIRES A REBOOT,
#      so this file only DETECTS it and reports whether it is active.
#   2. IRQ affinity steering. Moves every movable device interrupt off the
#      benchmark core onto the housekeeping core. Works at runtime, fully
#      reversible, done here. Per-CPU interrupts (the local APIC timer, IPIs,
#      rescheduling) cannot be steered and are unaffected — that is lever 1's job.
#   3. SCHED_FIFO. Stops other runnable tasks preempting the measurement.
#      Cheap; RT throttling (sched_rt_runtime_us) keeps a runaway from wedging
#      the box.
#
# BENCH_CORE is the core that is measured on; HOUSEKEEPING_CORE absorbs the
# steered interrupts. Core 0 stays the default benchmark core deliberately: the
# microcode patch is installed per-core by the binary itself after it pins, and
# core 0 is the configuration every previously published result used.

BENCH_CORE="${BENCH_CORE:-0}"
HOUSEKEEPING_CORE="${HOUSEKEEPING_CORE:-1}"

# Command prefix for every timed binary. Set by setup_isolation.
BENCH_RUN="sudo taskset -c $BENCH_CORE"

# Reported in the results header.
ISOLATION_STATUS="not configured"
NOHZ_FULL_STATUS="off"
IRQS_STEERED=0
IRQS_UNMOVABLE=0

declare -A _IRQ_SAVED=()      # irq number -> original smp_affinity_list
_ISOLATION_ACTIVE=0
_IRQBALANCE_STOPPED=0         # 1 if WE stopped irqbalance and must restart it

# setup_isolation — steer movable IRQs off BENCH_CORE and pick the run prefix.
# Never fatal: every step degrades to "not applied" and is disclosed.
setup_isolation() {
    local cmdline nohz iso

    cmdline=$(cat /proc/cmdline 2>/dev/null || echo "")
    nohz=$(grep -o 'nohz_full=[^ ]*' <<<"$cmdline" || true)
    iso=$(grep -o 'isolcpus=[^ ]*'  <<<"$cmdline" || true)
    if [[ -n "$nohz" || -n "$iso" ]]; then
        NOHZ_FULL_STATUS="active (${nohz:-}${nohz:+ }${iso:-})"
    else
        NOHZ_FULL_STATUS="off — periodic timer tick still hits core $BENCH_CORE"
        echo "NOTE: neither nohz_full= nor isolcpus= is on the kernel command line."
        echo "      The local-timer tick still interrupts the benchmark core. To remove it,"
        echo "      boot with:  isolcpus=$BENCH_CORE nohz_full=$BENCH_CORE rcu_nocbs=$BENCH_CORE"
        echo "      (reboot required; the sweep is still valid without it — medians sit at"
        echo "       the interference-free floor — but the p90 tail stays.)"
    fi

    # irqbalance re-spreads interrupts every few seconds and would silently undo
    # the steering below part-way through the sweep, so stop it for the duration
    # and restart it in restore_isolation if we were the one who stopped it.
    if pgrep -x irqbalance >/dev/null 2>&1; then
        if sudo systemctl stop irqbalance >/dev/null 2>&1; then
            _IRQBALANCE_STOPPED=1
            echo "Stopped irqbalance for the sweep (restarted afterwards)."
        else
            echo "WARNING: irqbalance is running and could not be stopped; it will undo"
            echo "         IRQ steering mid-sweep. Stop it manually and re-run:"
            echo "           sudo systemctl stop irqbalance"
        fi
    fi

    # Steer every movable device IRQ onto the housekeeping core.
    local irq n cur
    for irq in /proc/irq/[0-9]*; do
        [[ -d "$irq" ]] || continue
        n="${irq##*/}"
        cur=$(cat "$irq/smp_affinity_list" 2>/dev/null) || continue
        [[ -n "$cur" ]] || continue
        if echo "$HOUSEKEEPING_CORE" | sudo tee "$irq/smp_affinity_list" >/dev/null 2>&1; then
            _IRQ_SAVED["$n"]="$cur"
            IRQS_STEERED=$((IRQS_STEERED + 1))
        else
            IRQS_UNMOVABLE=$((IRQS_UNMOVABLE + 1))
        fi
    done
    _ISOLATION_ACTIVE=1

    # SCHED_FIFO for the measured process, if chrt is available.
    if command -v chrt >/dev/null 2>&1; then
        BENCH_RUN="sudo chrt -f 99 taskset -c $BENCH_CORE"
        ISOLATION_STATUS="core $BENCH_CORE; $IRQS_STEERED IRQs steered to core $HOUSEKEEPING_CORE ($IRQS_UNMOVABLE per-CPU/unmovable); SCHED_FIFO 99"
    else
        ISOLATION_STATUS="core $BENCH_CORE; $IRQS_STEERED IRQs steered to core $HOUSEKEEPING_CORE ($IRQS_UNMOVABLE per-CPU/unmovable); chrt absent, normal priority"
    fi

    echo "Isolation: $ISOLATION_STATUS"
    echo "           nohz_full/isolcpus: $NOHZ_FULL_STATUS"
}

# restore_isolation — put every steered IRQ back. Registered on EXIT by the
# orchestrator so an interrupted sweep does not leave the machine reconfigured.
restore_isolation() {
    (( _ISOLATION_ACTIVE )) || return 0
    _ISOLATION_ACTIVE=0
    local n restored=0
    for n in "${!_IRQ_SAVED[@]}"; do
        if echo "${_IRQ_SAVED[$n]}" | sudo tee "/proc/irq/$n/smp_affinity_list" >/dev/null 2>&1; then
            restored=$((restored + 1))
        fi
    done
    echo "Isolation restored: $restored/${#_IRQ_SAVED[@]} IRQ affinities put back."

    if (( _IRQBALANCE_STOPPED )); then
        _IRQBALANCE_STOPPED=0
        if sudo systemctl start irqbalance >/dev/null 2>&1; then
            echo "irqbalance restarted."
        else
            echo "WARNING: could not restart irqbalance — start it manually:"
            echo "           sudo systemctl start irqbalance"
        fi
    fi
}
