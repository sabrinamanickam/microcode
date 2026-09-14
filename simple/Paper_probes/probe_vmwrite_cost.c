/*
 * probe_vmwrite_cost.c — measure microcode dispatch overhead.
 *
 * Strategy:
 *   1. Install a 1-triad patch hooked to vmwrite. The triad is a single
 *      ZEROEXT_DSZ64_DR(RDI, RDI) — effectively a microcode-side "mov rdi, rdi",
 *      which is the smallest safe payload (a bare NOP triad has been reported
 *      to wedge the sequencer; a real op terminates cleanly).
 *   2. Time N back-to-back vmwrites in a tight loop, no other work inside.
 *   3. Divide by N for per-call cost. Subtract 1 cyc (the one triad runs at
 *      1 cyc/triad inside the patch) and that's the dispatch overhead.
 *   4. Cross-reference against a native `mov reg, reg` loop and a bare empty
 *      loop to confirm the loop framing isn't biasing the measurement.
 *
 * What we're answering:
 *   Per-X25519 we issue ~2,560 microcode calls. If dispatch overhead is X cyc
 *   per call, X25519 carries 2560*X cyc of pure microcode-invocation cost that
 *   amd64-64 doesn't pay. The current measured gap to amd64-64 is ~30k cyc, so
 *   X ≈ 12 is consistent with the gap being entirely dispatch-bound.
 *
 *   B2 (SQ+MUL fusion at step 13→14) would save 1 microcode call per ladder
 *   iter × 255 iters = 255 saved calls. The savings are 255 * X cyc — so:
 *     X ≈  5: B2 saves ~1.3k  → not worth ~5h patch work
 *     X ≈ 15: B2 saves ~3.8k  → marginally worth it
 *     X ≈ 30: B2 saves ~7.6k  → clearly worth it
 *
 * Build: make PROG=probe_vmwrite_cost
 * Run:   sudo taskset -c 0 ./probe_vmwrite_cost_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define N_CALLS    100000
#define BENCH_REPS 100

static inline uint64_t rdtsc_start(void) {
    uint32_t lo, hi;
    asm volatile("cpuid\n\trdtsc" : "=a"(lo), "=d"(hi) :: "rbx", "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t rdtsc_end(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx", "memory");
    asm volatile("cpuid" ::: "rax", "rbx", "rcx", "rdx", "memory");
    return ((uint64_t)hi << 32) | lo;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(void) {
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    /* Install 1-triad probe patch: ZEROEXT_DSZ64_DR(RDI, RDI). Slots 1-2 NOP.
     * END_SEQWORD terminates. */
    ucode_t patch[] = {
        { ZEROEXT_DSZ64_DR(RDI, RDI), NOP, NOP, END_SEQWORD }
    };
    patch_ucode(0x7c00, patch, 1);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("Installed 1-triad ZEROEXT probe patch at U7c00 (vmwrite hook).\n\n");

    /* Warmup so we don't measure cold-cache effects. */
    for (int i = 0; i < 1000; i++) {
        asm volatile("vmwrite rcx, rdx" ::: "rcx", "rdx", "memory", "cc");
    }

    uint64_t samples[BENCH_REPS];

    /* === Bench 1: vmwrite (1-triad patch) === */
    for (int r = 0; r < BENCH_REPS; r++) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < N_CALLS; i++) {
            asm volatile("vmwrite rcx, rdx" ::: "rcx", "rdx", "memory", "cc");
        }
        uint64_t t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    qsort(samples, BENCH_REPS, sizeof(samples[0]), cmp_u64);
    uint64_t vmw_min = samples[0];
    uint64_t vmw_med = samples[BENCH_REPS/2];
    printf("=== vmwrite + 1-triad patch  (%d calls × %d reps) ===\n", N_CALLS, BENCH_REPS);
    printf("  total: min %" PRIu64 ", median %" PRIu64 " cyc\n", vmw_min, vmw_med);
    printf("  per-call: min %.2f cyc, median %.2f cyc\n",
           vmw_min/(double)N_CALLS, vmw_med/(double)N_CALLS);

    /* === Bench 2: native INC reg (real serial-dep op, not eliminated) === */
    /* INC has a real dependency through dummy; can't be renamed away. */
    uint64_t dummy = 0;
    for (int r = 0; r < BENCH_REPS; r++) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < N_CALLS; i++) {
            asm volatile("inc %0" : "+r"(dummy));
        }
        uint64_t t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    qsort(samples, BENCH_REPS, sizeof(samples[0]), cmp_u64);
    uint64_t inc_min = samples[0];
    uint64_t inc_med = samples[BENCH_REPS/2];
    printf("\n=== native INC reg  (%d calls × %d reps) ===\n", N_CALLS, BENCH_REPS);
    printf("  total: min %" PRIu64 ", median %" PRIu64 " cyc  (dummy=%" PRIu64 ")\n",
           inc_min, inc_med, dummy);
    printf("  per-call: min %.2f cyc, median %.2f cyc\n",
           inc_min/(double)N_CALLS, inc_med/(double)N_CALLS);

    /* === Bench 2b: native MOV r32, r32 (32-bit, zero-extends to 64; NOT eliminated) === */
    uint64_t a = 0xDEADBEEFCAFEBABEULL, b = 0x1122334455667788ULL;
    for (int r = 0; r < BENCH_REPS; r++) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < N_CALLS; i++) {
            asm volatile("mov %k0, %k1" : "+r"(a) : "r"(b));
        }
        uint64_t t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    qsort(samples, BENCH_REPS, sizeof(samples[0]), cmp_u64);
    uint64_t mov32_min = samples[0];
    uint64_t mov32_med = samples[BENCH_REPS/2];
    printf("\n=== native MOV r32,r32 (zero-extend)  (%d calls × %d reps) ===\n", N_CALLS, BENCH_REPS);
    printf("  total: min %" PRIu64 ", median %" PRIu64 " cyc  (a=%" PRIu64 ")\n",
           mov32_min, mov32_med, a);
    printf("  per-call: min %.2f cyc, median %.2f cyc\n",
           mov32_min/(double)N_CALLS, mov32_med/(double)N_CALLS);

    /* === Bench 3: empty volatile loop (frames the loop overhead floor) === */
    for (int r = 0; r < BENCH_REPS; r++) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < N_CALLS; i++) {
            asm volatile("" ::: "memory");
        }
        uint64_t t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    qsort(samples, BENCH_REPS, sizeof(samples[0]), cmp_u64);
    uint64_t empty_min = samples[0];
    uint64_t empty_med = samples[BENCH_REPS/2];
    printf("\n=== empty loop  (%d iters × %d reps) ===\n", N_CALLS, BENCH_REPS);
    printf("  total: min %" PRIu64 ", median %" PRIu64 " cyc\n", empty_min, empty_med);
    printf("  per-iter: min %.2f cyc, median %.2f cyc\n",
           empty_min/(double)N_CALLS, empty_med/(double)N_CALLS);

    /* === Analysis === */
    /* Signed cast in case any "minus empty" measurement is < 0 due to noise. */
    double vmw_per   = ((int64_t)vmw_min   - (int64_t)empty_min) / (double)N_CALLS;
    double inc_per   = ((int64_t)inc_min   - (int64_t)empty_min) / (double)N_CALLS;
    double mov32_per = ((int64_t)mov32_min - (int64_t)empty_min) / (double)N_CALLS;
    printf("\n=== Analysis ===\n");
    printf("  vmwrite + 1-triad patch (minus loop floor):  %.2f cyc/call\n", vmw_per);
    printf("  native INC reg          (minus loop floor):  %.2f cyc/call\n", inc_per);
    printf("  native MOV r32,r32      (minus loop floor):  %.2f cyc/call\n", mov32_per);
    printf("\n  Patch contributes 1 cyc for the 1 triad. Dispatch overhead estimate: %.2f cyc/call\n",
           vmw_per - 1.0);
    printf("  vmwrite vs native INC ratio: %.1fx\n", vmw_per / inc_per);
    printf("\n  Implication for B2 (SQ+MUL fusion, saves 1 vmread/iter × 255 = 255 calls/X25519):\n");
    printf("    expected savings = %.2f × 255 = %.0f cyc/X25519\n",
           vmw_per - 1.0, (vmw_per - 1.0) * 255.0);

    /* Clean up the hook so future runs start fresh. */
    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
