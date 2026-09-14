/*
 * freq_guard.h — refuse to produce timing numbers on an unpinned core.
 *
 * WHY THIS EXISTS. Every cycle count in this tree comes from RDTSC, which
 * ticks at the TSC's nominal rate (1.094 GHz on the N3350), NOT the core
 * clock. The two are only equal when the core is pinned to base. Let the
 * governor turbo to ~2.4 GHz and every number is scaled by f_TSC/f_core,
 * about 0.46 -- an unpinned run reports a real 278k-cycle X25519 as ~130k
 * and looks like a spectacular result rather than a broken measurement.
 *
 * lib/freq_guard.sh already enforces this, but only for the 24-config sweep.
 * The standalone binaries had no such check, and it has bitten twice in one
 * day: bench_kernel silently recorded a full unpinned arm set, and after a
 * reboot wiped the pinning the benchmark reported every contender at ~2.15x
 * too fast with the ranking intact, which is exactly the shape that gets
 * mistaken for a real speedup.
 *
 * A reboot ALWAYS clears no_turbo and returns the governor to ondemand, so
 * this is not a rare condition -- it is the default state of the machine.
 *
 * Escape hatch: ALLOW_UNPINNED=1 in the environment, matching the shell
 * guard, for the rare case where relative ratios are all that is wanted.
 */
#ifndef FREQ_GUARD_H
#define FREQ_GUARD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fg_read_long(const char *path, long *out) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fscanf(f, "%ld", out) == 1;
    fclose(f);
    return ok;
}

static int fg_read_str(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(buf, (int)n, f) != NULL;
    fclose(f);
    if (ok) buf[strcspn(buf, "\n")] = '\0';
    return ok;
}

/* Base/TSC frequency in kHz, from the model name's "@ X.YZGHz" tag. */
static long fg_base_khz(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return 1100000;
    char line[512];
    long khz = 1100000;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "model name", 10)) continue;
        char *at = strstr(line, "@ ");
        double ghz;
        if (at && sscanf(at + 2, "%lfGHz", &ghz) == 1) khz = (long)(ghz * 1000000.0);
        break;
    }
    fclose(f);
    return khz;
}

/* Returns 0 if the machine is fit to time on, 1 if the caller should abort. */
static int freq_guard(void) {
    const char *allow = getenv("ALLOW_UNPINNED");
    if (allow && *allow == '1') {
        printf("WARNING: ALLOW_UNPINNED=1 -- frequency guard skipped.\n"
               "         Absolute cycle counts are NOT true cycles/op; ratios only.\n\n");
        return 0;
    }

    long v, base = fg_base_khz(), lo = (long)(base * 0.94), hi = (long)(base * 1.06);
    int bad = 0;
    char gov[64];

    if (fg_read_long("/sys/devices/system/cpu/intel_pstate/no_turbo", &v)) {
        if (v != 1) { printf("  turbo is ENABLED (intel_pstate/no_turbo=%ld)\n", v); bad = 1; }
    } else if (fg_read_long("/sys/devices/system/cpu/cpufreq/boost", &v)) {
        if (v != 0) { printf("  boost is ENABLED (cpufreq/boost=%ld)\n", v); bad = 1; }
    }

    for (int c = 0; c < 64; c++) {
        char p[128];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
        if (!fg_read_long(p, &v)) continue;
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", c);
        if (!fg_read_str(p, gov, sizeof gov)) strcpy(gov, "?");
        if (v < lo || v > hi) {
            printf("  cpu%d: governor=%s cur=%ld kHz (outside %ld +/-6%%)\n", c, gov, v, base);
            bad = 1;
        }
    }

    if (!bad) return 0;

    printf("\nREFUSING TO MEASURE: the core is not pinned to its base/TSC frequency.\n"
           "RDTSC ticks at the TSC rate, so an unpinned run reports cycle counts\n"
           "scaled by f_TSC/f_core -- roughly 2.2x too fast on this part, with the\n"
           "ranking intact, which reads as a real speedup and is not one.\n"
           "\n  A reboot clears these; they are not sticky. Re-pin with:\n"
           "    echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo\n"
           "    echo userspace | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor\n"
           "    echo %ld | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_setspeed\n"
           "\n  (To bypass deliberately: set ALLOW_UNPINNED=1)\n\n",
           base);
    return 1;
}

#endif /* FREQ_GUARD_H */
