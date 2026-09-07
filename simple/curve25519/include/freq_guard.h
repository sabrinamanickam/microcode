/* include/freq_guard.h — self-certifying CPU frequency guard (header-only).
 *
 * rdtsc counts at the fixed TSC rate, not the core clock. On the N3350 the two
 * are equal ONLY when turbo is off and the core is pinned to base (1.10 GHz).
 * If the core bursts to 2.4 GHz every reported count is scaled by ~0.46 and
 * the absolute numbers are meaningless (lib/freq_guard.sh has the full note).
 *
 * frequency_guard() calibrates the TSC against CLOCK_MONOTONIC, reads the
 * turbo/governor/frequency state, PRINTS what it measured so the output
 * carries its own provenance, and returns non-zero if the numbers would not
 * be publishable. ALLOW_UNPINNED=1 downgrades the refusal to a warning.
 *
 * Header-only and self-contained: it reads the TSC itself rather than relying
 * on the including file's rdtsc helpers, so every benchmark binary gets an
 * identical guard.
 */
#ifndef FREQ_GUARD_H
#define FREQ_GUARD_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline uint64_t fg_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("cpuid\n\trdtsc" : "=a"(lo), "=d"(hi) :: "rbx", "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
}

static long fg_read_long(const char *path, long fallback)
{
    FILE *f = fopen(path, "r");
    if (!f) return fallback;
    long v;
    if (fscanf(f, "%ld", &v) != 1) v = fallback;
    fclose(f);
    return v;
}

static void fg_read_str(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    snprintf(buf, n, "unknown");
    if (!f) return;
    if (fgets(buf, (int)n, f)) buf[strcspn(buf, "\n")] = 0;
    fclose(f);
}

/* Calibrate the TSC against CLOCK_MONOTONIC over ~80 ms. */
static double fg_tsc_ghz(void)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    uint64_t t0 = fg_tsc();
    double ns;
    do {
        clock_gettime(CLOCK_MONOTONIC, &b);
        ns = (double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec);
    } while (ns < 8e7);
    uint64_t t1 = fg_tsc();
    return (double)(t1 - t0) / ns;      /* ticks per ns == GHz */
}

/* 0 = rdtsc ticks may be read as core cycles; non-zero = do not publish. */
static int frequency_guard(void)
{
    long no_turbo = fg_read_long("/sys/devices/system/cpu/intel_pstate/no_turbo", -1);
    long cur_khz  = fg_read_long("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", -1);
    long max_khz  = fg_read_long("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", -1);
    char gov[64];
    fg_read_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", gov, sizeof gov);

    double tsc_ghz  = fg_tsc_ghz();
    double core_ghz = cur_khz > 0 ? (double)cur_khz / 1e6 : 0.0;
    double ratio    = core_ghz > 0 ? tsc_ghz / core_ghz : 0.0;

    printf("--- measurement conditions ---\n");
    printf("  TSC rate            %.4f GHz  (calibrated vs CLOCK_MONOTONIC)\n", tsc_ghz);
    printf("  core clock          %.4f GHz  (scaling_cur_freq)\n", core_ghz);
    printf("  scaling_max_freq    %.4f GHz\n", max_khz > 0 ? (double)max_khz / 1e6 : 0.0);
    printf("  governor            %s\n", gov);
    printf("  turbo               %s\n",
           no_turbo == 1 ? "OFF (no_turbo=1)" :
           no_turbo == 0 ? "ON  (no_turbo=0)" : "unknown (no intel_pstate knob)");
    printf("  TSC / core          %.4f\n", ratio);
    printf("FGUARD tsc_ghz=%.4f core_ghz=%.4f ratio=%.4f no_turbo=%ld governor=%s\n",
           tsc_ghz, core_ghz, ratio, no_turbo, gov);

    int bad = 0;
    if (no_turbo != 1) {
        printf("  !! turbo is not disabled: the core can burst above base mid-run\n");
        bad = 1;
    }
    if (ratio < 0.98 || ratio > 1.02) {
        printf("  !! TSC and core clock disagree by >2%%: reported ticks are NOT cycles\n");
        bad = 1;
    }
    if (!bad) {
        printf("  => pinned: rdtsc ticks == core cycles, absolute counts are valid\n\n");
        return 0;
    }
    if (getenv("ALLOW_UNPINNED")) {
        printf("  ALLOW_UNPINNED set: continuing. Treat every number as a RATIO\n"
               "  only; the absolute cycle counts are scaled and not publishable.\n\n");
        return 0;
    }
    printf("\n  Refusing to produce publishable numbers unpinned. Fix with:\n"
           "    echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo\n"
           "    echo userspace | sudo tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor\n"
           "    echo 1100000  | sudo tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed\n"
           "  (or re-run with ALLOW_UNPINNED=1 for ratios only)\n\n");
    return 1;
}

#endif /* FREQ_GUARD_H */
