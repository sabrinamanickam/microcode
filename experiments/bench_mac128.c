/*
 * bench_mac128.c — Benchmark for MAC128 via VMWRITE hook
 *
 * Measures:
 *   1. Single MAC128 invocation latency (vmwrite rcx, rdx)
 *   2. 3-MAC chain latency (simulating limb accumulation)
 *   3. Native x86 baseline: MUL + ADD/ADC for comparison
 *   4. Single WMUL invocation latency
 *
 * Usage:
 *   ./bench_mac128          — MAC128 benchmarks (default)
 *   ./bench_mac128 w        — WMUL benchmarks
 *   ./bench_mac128 all      — Everything
 *
 * Compile same as mac128_vmw.c (needs patch.h, ucode_macro.h, etc.)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* ──────────────────────────────────────────────────────────────────
 *  Config
 * ────────────────────────────────────────────────────────────────── */
#define WARMUP_ITERS    1000
#define BENCH_ITERS     10000
#define CHAIN_LEN       3       /* MACs per chain (matches curve25519 limb) */

/* ──────────────────────────────────────────────────────────────────
 *  TSC helpers
 * ────────────────────────────────────────────────────────────────── */
static inline uint64_t rdtsc_start(void) {
        uint32_t lo, hi;
        asm volatile(
                "cpuid\n\t"
                "rdtsc\n\t"
                : "=a"(lo), "=d"(hi)
                :
                : "rbx", "rcx"
        );
        return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtsc_end(void) {
        uint32_t lo, hi;
        asm volatile(
                "rdtscp\n\t"
                "mov %%eax, %0\n\t"
                "mov %%edx, %1\n\t"
                "cpuid\n\t"
                : "=r"(lo), "=r"(hi)
                :
                : "rax", "rbx", "rcx", "rdx"
        );
        return ((uint64_t)hi << 32) | lo;
}

/* ──────────────────────────────────────────────────────────────────
 *  Patch installation (same as mac128_vmw.c)
 * ────────────────────────────────────────────────────────────────── */
static void do_patch(ucode_t *patch, int n_triads) {
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, n_triads);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_mac128(void) {
        ucode_t mac128_patch[] = {
                /* Triad 0: Save acc_lo, multiply */
                {
                        MOVE_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: sum + carry-detect operands */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* Triad 2: ~sum & (a|b), acc_hi += prod_hi */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 3: merge carry chain */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 4: extract carry bit 63 */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 5: fold carry, done */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(mac128_patch, 6);
}

static void install_wmul(void) {
        ucode_t wmul_patch[] = {
                {
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        MOVE_DSZ64_DR(RAX, RCX),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(wmul_patch, 2);
}

/* ──────────────────────────────────────────────────────────────────
 *  Sort helper for median
 * ────────────────────────────────────────────────────────────────── */
static int cmp_u64(const void *a, const void *b) {
        uint64_t va = *(const uint64_t *)a;
        uint64_t vb = *(const uint64_t *)b;
        return (va > vb) - (va < vb);
}

static void report(const char *label, uint64_t *samples, int n) {
        qsort(samples, n, sizeof(uint64_t), cmp_u64);

        uint64_t min = samples[0];
        uint64_t max = samples[n - 1];
        uint64_t med = samples[n / 2];

        /* p5 / p95 */
        uint64_t p5  = samples[(int)(n * 0.05)];
        uint64_t p95 = samples[(int)(n * 0.95)];

        /* mean (trim 5% tails) */
        int lo = (int)(n * 0.05);
        int hi = (int)(n * 0.95);
        uint64_t sum = 0;
        for (int i = lo; i < hi; i++)
                sum += samples[i];
        uint64_t tmean = sum / (hi - lo);

        printf("  %-38s  median %7" PRIu64 "  mean(trim) %7" PRIu64
               "  [p5 %7" PRIu64 "  p95 %7" PRIu64 "]"
               "  min %7" PRIu64 "  max %7" PRIu64 "\n",
               label, med, tmean, p5, p95, min, max);
}


/* ══════════════════════════════════════════════════════════════════
 *  Benchmark: single MAC128 invocation
 * ══════════════════════════════════════════════════════════════════ */
static void bench_mac128_single(void) {
        uint64_t samples[BENCH_ITERS];

        uint64_t a = 0x0007FFFFFFFFFFFFULL;   /* 51-bit limb */
        uint64_t b = 0x0006000000000000ULL;

        /* warmup */
        for (int i = 0; i < WARMUP_ITERS; i++) {
                asm volatile(
                        "xor rax, rax\n\t"
                        "xor r8, r8\n\t"
                        "mov rcx, %[a]\n\t"
                        "mov rdx, %[b]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        :
                        : [a] "r"(a), [b] "r"(b)
                        : "rax", "rcx", "rdx", "r8"
                );
        }

        /* measure */
        for (int i = 0; i < BENCH_ITERS; i++) {
                uint64_t t0 = rdtsc_start();
                asm volatile(
                        "xor rax, rax\n\t"
                        "xor r8, r8\n\t"
                        "mov rcx, %[a]\n\t"
                        "mov rdx, %[b]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        :
                        : [a] "r"(a), [b] "r"(b)
                        : "rax", "rcx", "rdx", "r8"
                );
                uint64_t t1 = rdtsc_end();
                samples[i] = t1 - t0;
        }

        report("MAC128 single (vmwrite)", samples, BENCH_ITERS);
}


/* ══════════════════════════════════════════════════════════════════
 *  Benchmark: 3-MAC chain (one limb accumulation)
 * ══════════════════════════════════════════════════════════════════ */
static void bench_mac128_chain3(void) {
        uint64_t samples[BENCH_ITERS];

        uint64_t a  = 0x0007FFFFFFFFFFFFULL;
        uint64_t b1 = 0x0006000000000000ULL;
        uint64_t b2 = 0x0005000000000000ULL;
        uint64_t b3 = 0x0004000000000000ULL;

        for (int i = 0; i < WARMUP_ITERS; i++) {
                asm volatile(
                        "xor rax, rax\n\t"
                        "xor r8, r8\n\t"
                        "mov rcx, %[a]\n\t"
                        "mov rdx, %[b1]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        "mov rcx, %[a]\n\t"
                        "mov rdx, %[b2]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        "mov rcx, %[a]\n\t"
                        "mov rdx, %[b3]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        :
                        : [a] "r"(a), [b1] "r"(b1), [b2] "r"(b2), [b3] "r"(b3)
                        : "rax", "rcx", "rdx", "r8"
                );
        }

        for (int i = 0; i < BENCH_ITERS; i++) {
                uint64_t t0 = rdtsc_start();
                asm volatile(
                        "xor rax, rax\n\t"
                        "xor r8, r8\n\t"
                        "mov rcx, %[a]\n\t"
                        "mov rdx, %[b1]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        "mov rcx, %[a]\n\t"
                        "mov rdx, %[b2]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        "mov rcx, %[a]\n\t"
                        "mov rdx, %[b3]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        :
                        : [a] "r"(a), [b1] "r"(b1), [b2] "r"(b2), [b3] "r"(b3)
                        : "rax", "rcx", "rdx", "r8"
                );
                uint64_t t1 = rdtsc_end();
                samples[i] = t1 - t0;
        }

        report("MAC128 chain×3 (3 vmwrites)", samples, BENCH_ITERS);
}


/* ══════════════════════════════════════════════════════════════════
 *  Benchmark: native x86 baseline — MUL + ADD/ADC
 *
 *  Single MAC: mul rdx → rdx:rax, then add/adc into accumulator.
 *  This is what the compiler would emit for __uint128_t math.
 * ══════════════════════════════════════════════════════════════════ */
static void bench_native_single(void) {
        uint64_t samples[BENCH_ITERS];

        uint64_t a = 0x0007FFFFFFFFFFFFULL;
        uint64_t b = 0x0006000000000000ULL;

        for (int i = 0; i < WARMUP_ITERS; i++) {
                asm volatile(
                        "xor r8, r8\n\t"           /* acc_hi = 0 */
                        "xor r9, r9\n\t"           /* acc_lo = 0 */
                        "mov rax, %[a]\n\t"
                        "mul %[b]\n\t"              /* rdx:rax = a*b */
                        "add r9, rax\n\t"           /* acc_lo += lo */
                        "adc r8, rdx\n\t"           /* acc_hi += hi + CF */
                        :
                        : [a] "r"(a), [b] "r"(b)
                        : "rax", "rdx", "r8", "r9"
                );
        }

        for (int i = 0; i < BENCH_ITERS; i++) {
                uint64_t t0 = rdtsc_start();
                asm volatile(
                        "xor r8, r8\n\t"
                        "xor r9, r9\n\t"
                        "mov rax, %[a]\n\t"
                        "mul %[b]\n\t"
                        "add r9, rax\n\t"
                        "adc r8, rdx\n\t"
                        :
                        : [a] "r"(a), [b] "r"(b)
                        : "rax", "rdx", "r8", "r9"
                );
                uint64_t t1 = rdtsc_end();
                samples[i] = t1 - t0;
        }

        report("Native MUL+ADD/ADC single", samples, BENCH_ITERS);
}

static void bench_native_chain3(void) {
        uint64_t samples[BENCH_ITERS];

        uint64_t a  = 0x0007FFFFFFFFFFFFULL;
        uint64_t b1 = 0x0006000000000000ULL;
        uint64_t b2 = 0x0005000000000000ULL;
        uint64_t b3 = 0x0004000000000000ULL;

        for (int i = 0; i < WARMUP_ITERS; i++) {
                asm volatile(
                        "xor r8, r8\n\t"
                        "xor r9, r9\n\t"
                        /* MAC 1 */
                        "mov rax, %[a]\n\t"
                        "mul %[b1]\n\t"
                        "add r9, rax\n\t"
                        "adc r8, rdx\n\t"
                        /* MAC 2 */
                        "mov rax, %[a]\n\t"
                        "mul %[b2]\n\t"
                        "add r9, rax\n\t"
                        "adc r8, rdx\n\t"
                        /* MAC 3 */
                        "mov rax, %[a]\n\t"
                        "mul %[b3]\n\t"
                        "add r9, rax\n\t"
                        "adc r8, rdx\n\t"
                        :
                        : [a] "r"(a), [b1] "r"(b1), [b2] "r"(b2), [b3] "r"(b3)
                        : "rax", "rdx", "r8", "r9"
                );
        }

        for (int i = 0; i < BENCH_ITERS; i++) {
                uint64_t t0 = rdtsc_start();
                asm volatile(
                        "xor r8, r8\n\t"
                        "xor r9, r9\n\t"
                        "mov rax, %[a]\n\t"
                        "mul %[b1]\n\t"
                        "add r9, rax\n\t"
                        "adc r8, rdx\n\t"
                        "mov rax, %[a]\n\t"
                        "mul %[b2]\n\t"
                        "add r9, rax\n\t"
                        "adc r8, rdx\n\t"
                        "mov rax, %[a]\n\t"
                        "mul %[b3]\n\t"
                        "add r9, rax\n\t"
                        "adc r8, rdx\n\t"
                        :
                        : [a] "r"(a), [b1] "r"(b1), [b2] "r"(b2), [b3] "r"(b3)
                        : "rax", "rdx", "r8", "r9"
                );
                uint64_t t1 = rdtsc_end();
                samples[i] = t1 - t0;
        }

        report("Native MUL+ADD/ADC chain×3", samples, BENCH_ITERS);
}


/* ══════════════════════════════════════════════════════════════════
 *  Benchmark: single WMUL invocation
 * ══════════════════════════════════════════════════════════════════ */
static void bench_wmul_single(void) {
        uint64_t samples[BENCH_ITERS];

        uint64_t a = 0x0007FFFFFFFFFFFFULL;
        uint64_t b = 0x0006000000000000ULL;

        for (int i = 0; i < WARMUP_ITERS; i++) {
                asm volatile(
                        "mov rcx, %[a]\n\t"
                        "mov rdx, %[b]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        :
                        : [a] "r"(a), [b] "r"(b)
                        : "rax", "rcx", "rdx"
                );
        }

        for (int i = 0; i < BENCH_ITERS; i++) {
                uint64_t t0 = rdtsc_start();
                asm volatile(
                        "mov rcx, %[a]\n\t"
                        "mov rdx, %[b]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        :
                        : [a] "r"(a), [b] "r"(b)
                        : "rax", "rcx", "rdx"
                );
                uint64_t t1 = rdtsc_end();
                samples[i] = t1 - t0;
        }

        report("WMUL single (vmwrite, 2 triads)", samples, BENCH_ITERS);
}


/* ══════════════════════════════════════════════════════════════════
 *  Benchmark: native MUL alone (no accumulate)
 * ══════════════════════════════════════════════════════════════════ */
static void bench_native_mul(void) {
        uint64_t samples[BENCH_ITERS];

        uint64_t a = 0x0007FFFFFFFFFFFFULL;
        uint64_t b = 0x0006000000000000ULL;

        for (int i = 0; i < WARMUP_ITERS; i++) {
                asm volatile(
                        "mov rax, %[a]\n\t"
                        "mul %[b]\n\t"
                        :
                        : [a] "r"(a), [b] "r"(b)
                        : "rax", "rdx"
                );
        }

        for (int i = 0; i < BENCH_ITERS; i++) {
                uint64_t t0 = rdtsc_start();
                asm volatile(
                        "mov rax, %[a]\n\t"
                        "mul %[b]\n\t"
                        :
                        : [a] "r"(a), [b] "r"(b)
                        : "rax", "rdx"
                );
                uint64_t t1 = rdtsc_end();
                samples[i] = t1 - t0;
        }

        report("Native MUL alone", samples, BENCH_ITERS);
}


/* ══════════════════════════════════════════════════════════════════
 *  Benchmark: VMWRITE baseline (no hook installed)
 *
 *  Measures the raw cost of vmwrite when it hits the
 *  hook_match_and_patch interception path.  Already installed
 *  by whichever patch runs first, so we just time the
 *  instruction itself with dummy args.
 * ══════════════════════════════════════════════════════════════════ */


/* ══════════════════════════════════════════════════════════════════
 *  Full limb0 of carry_square via MAC128 chain
 *
 *  acc = arg1[0]^2 + arg1[1]*x2 + arg1[2]*x5
 *  Compare against native __uint128_t reference.
 * ══════════════════════════════════════════════════════════════════ */
static void bench_limb0(void) {
        uint64_t samples_uc[BENCH_ITERS];
        uint64_t samples_nat[BENCH_ITERS];

        /* Realistic Curve25519 51-bit limb values */
        uint64_t arg1_0 = 0x0007FFFFFFFFFFFFULL;
        uint64_t arg1_1 = 0x0006123456789ABCULL;
        uint64_t arg1_2 = 0x0005FEDCBA987654ULL;
        uint64_t x2     = arg1_1 * 2;   /* pre-doubled, stays 52-bit */
        uint64_t x5     = arg1_2 * 38;  /* reduction constant */

        /* — microcode path — */
        for (int i = 0; i < WARMUP_ITERS; i++) {
                asm volatile(
                        "xor rax, rax\n\t"
                        "xor r8, r8\n\t"
                        "mov rcx, %[a2]\n\t"
                        "mov rdx, %[x5]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        "mov rcx, %[a1]\n\t"
                        "mov rdx, %[x2]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        "mov rcx, %[a0]\n\t"
                        "mov rdx, %[a0]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        :
                        : [a0] "r"(arg1_0), [a1] "r"(arg1_1),
                          [a2] "r"(arg1_2), [x2] "r"(x2), [x5] "r"(x5)
                        : "rax", "rcx", "rdx", "r8"
                );
        }

        for (int i = 0; i < BENCH_ITERS; i++) {
                uint64_t t0 = rdtsc_start();
                asm volatile(
                        "xor rax, rax\n\t"
                        "xor r8, r8\n\t"
                        "mov rcx, %[a2]\n\t"
                        "mov rdx, %[x5]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        "mov rcx, %[a1]\n\t"
                        "mov rdx, %[x2]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        "mov rcx, %[a0]\n\t"
                        "mov rdx, %[a0]\n\t"
                        "vmwrite rcx, rdx\n\t"
                        :
                        : [a0] "r"(arg1_0), [a1] "r"(arg1_1),
                          [a2] "r"(arg1_2), [x2] "r"(x2), [x5] "r"(x5)
                        : "rax", "rcx", "rdx", "r8"
                );
                uint64_t t1 = rdtsc_end();
                samples_uc[i] = t1 - t0;
        }

        report("Limb0 microcode (3× vmwrite)", samples_uc, BENCH_ITERS);

        /* — native path — */
        for (int i = 0; i < WARMUP_ITERS; i++) {
                asm volatile(
                        "xor r8, r8\n\t"
                        "xor r9, r9\n\t"
                        /* arg1[2] * x5 */
                        "mov rax, %[a2]\n\t"
                        "mul %[x5]\n\t"
                        "add r9, rax\n\t"
                        "adc r8, rdx\n\t"
                        /* arg1[1] * x2 */
                        "mov rax, %[a1]\n\t"
                        "mul %[x2]\n\t"
                        "add r9, rax\n\t"
                        "adc r8, rdx\n\t"
                        /* arg1[0]^2 */
                        "mov rax, %[a0]\n\t"
                        "mul %[a0]\n\t"
                        "add r9, rax\n\t"
                        "adc r8, rdx\n\t"
                        :
                        : [a0] "r"(arg1_0), [a1] "r"(arg1_1),
                          [a2] "r"(arg1_2), [x2] "r"(x2), [x5] "r"(x5)
                        : "rax", "rdx", "r8", "r9"
                );
        }

        for (int i = 0; i < BENCH_ITERS; i++) {
                uint64_t t0 = rdtsc_start();
                asm volatile(
                        "xor r8, r8\n\t"
                        "xor r9, r9\n\t"
                        "mov rax, %[a2]\n\t"
                        "mul %[x5]\n\t"
                        "add r9, rax\n\t"
                        "adc r8, rdx\n\t"
                        "mov rax, %[a1]\n\t"
                        "mul %[x2]\n\t"
                        "add r9, rax\n\t"
                        "adc r8, rdx\n\t"
                        "mov rax, %[a0]\n\t"
                        "mul %[a0]\n\t"
                        "add r9, rax\n\t"
                        "adc r8, rdx\n\t"
                        :
                        : [a0] "r"(arg1_0), [a1] "r"(arg1_1),
                          [a2] "r"(arg1_2), [x2] "r"(x2), [x5] "r"(x5)
                        : "rax", "rdx", "r8", "r9"
                );
                uint64_t t1 = rdtsc_end();
                samples_nat[i] = t1 - t0;
        }

        report("Limb0 native (3× MUL+ADC)", samples_nat, BENCH_ITERS);
}


/* ══════════════════════════════════════════════════════════════════
 *  Main
 * ══════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
        int do_wmul = 0, do_mac = 0;

        if (argc > 1 && argv[1][0] == 'w')
                do_wmul = 1;
        else if (argc > 1 && strcmp(argv[1], "all") == 0)
                do_wmul = do_mac = 1;
        else
                do_mac = 1;

        printf("╔══════════════════════════════════════════════════════════════╗\n");
        printf("║  bench_mac128 — VMWRITE hook performance                    ║\n");
        printf("║  %d warmup, %d samples, CPUID/RDTSC/RDTSCP fencing         ║\n",
               WARMUP_ITERS, BENCH_ITERS);
        printf("╚══════════════════════════════════════════════════════════════╝\n\n");

        printf("  All values in TSC cycles.\n\n");

        if (do_mac || do_wmul) {
                /* Native baselines (no hook needed) */
                printf("── Native x86 baselines ──────────────────────────────────────\n");
                bench_native_mul();
                bench_native_single();
                bench_native_chain3();
                printf("\n");
        }

        if (do_wmul) {
                printf("── WMUL (2-triad wide multiply) ──────────────────────────────\n");
                install_wmul();
                bench_wmul_single();
                printf("\n");
        }

        if (do_mac) {
                printf("── MAC128 (6-triad multiply-accumulate) ──────────────────────\n");
                install_mac128();
                bench_mac128_single();
                bench_mac128_chain3();
                printf("\n");

                printf("── Limb0 carry_square comparison ─────────────────────────────\n");
                bench_limb0();
                printf("\n");
        }

        if (do_mac) {
                /* Summary ratios */
                printf("── Notes ─────────────────────────────────────────────────────\n");
                printf("  • vmwrite cost = hook_match_and_patch overhead + triad execution\n");
                printf("  • ~950K cycles/invocation expected from match-and-patch scanner\n");
                printf("  • Native path: MUL is ~3-4 cycles, ADD/ADC ~1 cycle on Goldmont\n");
                printf("  • Break-even requires amortizing hook cost across many operations\n");
                printf("  • For curve25519: 5 limbs × ~3 MACs = ~15 hook invocations/square\n");
                printf("\n");
        }

        return 0;
}
