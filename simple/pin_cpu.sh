#!/usr/bin/env bash
# pin_cpu.sh — put the Goldmont core into a benchmarking state:
#   turbo OFF (no_turbo=1) + userspace governor pinned to base (~1.10 GHz),
#   so RDTSC ticks == true core cycles. Re-run after every reboot: no_turbo
#   and the governor both reset on boot. Needs root.
#
# Run:  sudo bash pin_cpu.sh
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "Run as root:  sudo bash $0" >&2
  exit 1
fi

# base/TSC frequency parsed from the model name's "@ X.YZGHz" tag.
tsc_ghz=$(grep -m1 'model name' /proc/cpuinfo | sed -n 's/.*@ \([0-9.]*\)GHz.*/\1/p')
tsc_ghz=${tsc_ghz:-1.10}

# 1) disable turbo / boost (whichever driver knob exists)
if [[ -e /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
  echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
elif [[ -e /sys/devices/system/cpu/cpufreq/boost ]]; then
  echo 0 > /sys/devices/system/cpu/cpufreq/boost
fi

# 2) pin every core to base via the userspace governor
if command -v cpupower >/dev/null 2>&1; then
  cpupower frequency-set -g userspace >/dev/null
  cpupower frequency-set -f "${tsc_ghz}GHz" >/dev/null
else
  base_khz=$(awk "BEGIN{printf \"%d\", $tsc_ghz*1000000}")
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo userspace > "$g"; done
  for s in /sys/devices/system/cpu/cpu*/cpufreq/scaling_setspeed; do echo "$base_khz" > "$s"; done
fi

# 3) report the resulting state
echo "no_turbo = $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo n/a)"
for c in /sys/devices/system/cpu/cpu[0-9]*/cpufreq; do
  printf "  %s  gov=%s  cur=%s kHz\n" "${c#/sys/devices/system/cpu/}" \
     "$(cat "$c/scaling_governor")" "$(cat "$c/scaling_cur_freq")"
done
echo "Done — core pinned to ${tsc_ghz} GHz base, turbo off. (Resets on reboot; re-run before benchmarking.)"
