/*
 * bench.c — MAC128 carry_square vs C carry_square benchmark
 *
 * BUILD:
 *   nasm -f elf64 -o carry_square_mac128.o carry_square_mac128.asm
 *   gcc -O3 -march=native -o bench bench.c carry_square_mac128.o \
 *       -I../../include
 *
 * RUN:
 *   sudo ./bench
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* ══════════════════════════════════════════════════════════════════
 *  C reference: fiat-crypto carry_square (compiled by gcc -O3)
 * ══════════════════════════════════════════════════════════════════ */

typedef unsigned __int128 uint128_t;

static void carry_square_c(uint64_t out1[5], const uint64_t arg1[5]) {
        uint64_t x1 = arg1[4] * (uint64_t)0x13;
        uint64_t x2 = x1 * 0x2;
        uint64_t x3 = arg1[4] * 0x2;
        uint64_t x4 = arg1[3] * (uint64_t)0x13;
        uint64_t x5 = x4 * 0x2;
        uint64_t x6 = arg1[3] * 0x2;
        uint64_t x7 = arg1[2] * 0x2;
        uint64_t x8 = arg1[1] * 0x2;

        uint128_t x23 = (uint128_t)arg1[0] * arg1[0];
        uint128_t x15 = (uint128_t)arg1[1] * x2;
        uint128_t x13 = (uint128_t)arg1[2] * x5;
        uint128_t x24 = x23 + x15 + x13;
        uint64_t  x25 = (uint64_t)(x24 >> 51);
        uint64_t  x26 = (uint64_t)(x24 & 0x7FFFFFFFFFFFFULL);

        uint128_t x22 = (uint128_t)arg1[0] * x8;
        uint128_t x12 = (uint128_t)arg1[2] * x2;
        uint128_t x11 = (uint128_t)arg1[3] * x4;
        uint128_t x30 = x22 + x12 + x11;
        uint128_t x31 = x25 + x30;
        uint64_t  x32 = (uint64_t)(x31 >> 51);
        uint64_t  x33 = (uint64_t)(x31 & 0x7FFFFFFFFFFFFULL);

        uint128_t x21 = (uint128_t)arg1[0] * x7;
        uint128_t x18 = (uint128_t)arg1[1] * arg1[1];
        uint128_t x10 = (uint128_t)arg1[3] * x2;
        uint128_t x29 = x21 + x18 + x10;
        uint128_t x34 = x32 + x29;
        uint64_t  x35 = (uint64_t)(x34 >> 51);
        uint64_t  x36 = (uint64_t)(x34 & 0x7FFFFFFFFFFFFULL);

        uint128_t x20 = (uint128_t)arg1[0] * x6;
        uint128_t x17 = (uint128_t)arg1[1] * x7;
        uint128_t x9  = (uint128_t)arg1[4] * x1;
        uint128_t x28 = x20 + x17 + x9;
        uint128_t x37 = x35 + x28;
        uint64_t  x38 = (uint64_t)(x37 >> 51);
        uint64_t  x39 = (uint64_t)(x37 & 0x7FFFFFFFFFFFFULL);

        uint128_t x19 = (uint128_t)arg1[0] * x3;
        uint128_t x16 = (uint128_t)arg1[1] * x6;
        uint128_t x14 = (uint128_t)arg1[2] * arg1[2];
        uint128_t x27 = x19 + x16 + x14;
        uint128_t x40 = x38 + x27;
        uint64_t  x41 = (uint64_t)(x40 >> 51);
        uint64_t  x42 = (uint64_t)(x40 & 0x7FFFFFFFFFFFFULL);

        uint64_t x43 = x41 * (uint64_t)0x13;
        uint64_t x44 = x26 + x43;
        uint64_t x45 = x44 >> 51;
        uint64_t x46 = x44 & 0x7FFFFFFFFFFFFULL;
        uint64_t x47 = x45 + x33;
        uint64_t x48 = x47 >> 51;
        uint64_t x49 = x47 & 0x7FFFFFFFFFFFFULL;
        uint64_t x50 = x48 + x36;

        out1[0] = x46;
        out1[1] = x49;
        out1[2] = x50;
        out1[3] = x39;
        out1[4] = x42;
}


/* ══════════════════════════════════════════════════════════════════
 *  MAC128 carry_square (linked from NASM)
 * ══════════════════════════════════════════════════════════════════ */

extern void carry_square_mac128(uint64_t out1[5], const uint64_t arg1[5]);


/* ══════════════════════════════════════════════════════════════════
 *  MAC128 Microcode Installation
 * ══════════════════════════════════════════════════════════════════ */

static void install_mac128(void) {
        ucode_t mac128_patch[] = {
                /* Triad 0: Save acc_lo + multiply */
                {
                        MOVE_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: Accumulate low + carry detect operands */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* Triad 2: Propagated overflow + accumulate high */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 3: Merge carry chain */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 4: Extract carry bit */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 5: Fold carry, done */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac128_patch, ARRAY_SZ(mac128_patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  Cycle counting via RDTSC
 * ══════════════════════════════════════════════════════════════════ */

static inline uint64_t rdtsc_start(void) {
        uint32_t lo, hi;
        asm volatile(
                "cpuid\n\t"
                "rdtsc\n\t"
                : "=a"(lo), "=d"(hi)
                : "a"(0)
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

static int cmp_u64(const void *a, const void *b) {
        uint64_t va = *(const uint64_t *)a;
        uint64_t vb = *(const uint64_t *)b;
        return (va > vb) - (va < vb);
}


/* ══════════════════════════════════════════════════════════════════
 *  Validation
 * ══════════════════════════════════════════════════════════════════ */

static int validate(void) {
        uint64_t tests[][5] = {
                {0, 0, 0, 0, 0},
                {1, 0, 0, 0, 0},
                {123, 456, 789, 1011, 1213},
                {0x0003000000000000ULL, 0x0002000000000000ULL,
                 0x0001000000000000ULL, 0x0004000000000000ULL,
                 0x0005000000000000ULL},
                {0x0007FFFFFFFFFFFFULL, 0x0007FFFFFFFFFFFFULL,
                 0x0007FFFFFFFFFFFFULL, 0x0007FFFFFFFFFFFFULL,
                 0x0007FFFFFFFFFFFFULL},
                {0x0007FFFFFFFFFFFFULL, 1, 0x0004000000000000ULL, 2,
                 0x0006000000000000ULL},
                {0x00034A2F1B8C7D00ULL, 0x0005E91C3A287600ULL,
                 0x0001F4D8B2E63A00ULL, 0x00068C4F1D2A5B00ULL,
                 0x0002B7E0A9C15D00ULL},
                {42, 0, 0, 0, 0},
        };
        int n = sizeof(tests) / sizeof(tests[0]);
        int pass = 0;

        printf("=== VALIDATION ===\n\n");
        for (int i = 0; i < n; i++) {
                uint64_t c_out[5], mac_out[5];

                carry_square_c(c_out, tests[i]);
                carry_square_mac128(mac_out, tests[i]);

                int ok = (memcmp(c_out, mac_out, 40) == 0);
                printf("  Test %d: %s\n", i + 1, ok ? "✓" : "✗ MISMATCH");
                if (!ok) {
                        printf("    C:      [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ", "
                               "%016" PRIx64 ", %016" PRIx64 "]\n",
                               c_out[0], c_out[1], c_out[2], c_out[3], c_out[4]);
                        printf("    MAC128: [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ", "
                               "%016" PRIx64 ", %016" PRIx64 "]\n",
                               mac_out[0], mac_out[1], mac_out[2], mac_out[3], mac_out[4]);
                }
                pass += ok;
        }
        printf("\n  %d / %d passed\n\n", pass, n);
        return (pass == n);
}


/* ══════════════════════════════════════════════════════════════════
 *  Benchmark
 * ══════════════════════════════════════════════════════════════════ */

#define WARMUP   5000
#define SAMPLES  10001
#define INNER    100

static void benchmark(void) {
        uint64_t in_c[5]   = {0x34A2F1B8C7D00ULL, 0x5E91C3A287600ULL,
                               0x1F4D8B2E63A00ULL, 0x68C4F1D2A5B00ULL,
                               0x2B7E0A9C15D00ULL};
        uint64_t in_m[5];
        memcpy(in_m, in_c, 40);

        uint64_t out[5];
        uint64_t *c_cycles   = malloc(SAMPLES * sizeof(uint64_t));
        uint64_t *mac_cycles = malloc(SAMPLES * sizeof(uint64_t));

        printf("=== BENCHMARK ===\n");
        printf("  %d samples, %d iters each, %d warmup\n\n", SAMPLES, INNER, WARMUP);

        /* Warmup */
        for (int i = 0; i < WARMUP; i++) {
                carry_square_c(out, in_c);
                in_c[0] = out[0]; in_c[1] = out[1]; in_c[2] = out[2];
                in_c[3] = out[3]; in_c[4] = out[4];
        }
        memcpy(in_c, (uint64_t[]){0x34A2F1B8C7D00ULL, 0x5E91C3A287600ULL,
                0x1F4D8B2E63A00ULL, 0x68C4F1D2A5B00ULL, 0x2B7E0A9C15D00ULL}, 40);

        for (int i = 0; i < WARMUP; i++) {
                carry_square_mac128(out, in_m);
                in_m[0] = out[0]; in_m[1] = out[1]; in_m[2] = out[2];
                in_m[3] = out[3]; in_m[4] = out[4];
        }
        memcpy(in_m, (uint64_t[]){0x34A2F1B8C7D00ULL, 0x5E91C3A287600ULL,
                0x1F4D8B2E63A00ULL, 0x68C4F1D2A5B00ULL, 0x2B7E0A9C15D00ULL}, 40);

        /* Measure C version */
        for (int s = 0; s < SAMPLES; s++) {
                uint64_t t0 = rdtsc_start();
                for (int i = 0; i < INNER; i++) {
                        carry_square_c(out, in_c);
                        in_c[0] = out[0]; in_c[1] = out[1]; in_c[2] = out[2];
                        in_c[3] = out[3]; in_c[4] = out[4];
                }
                uint64_t t1 = rdtsc_end();
                c_cycles[s] = (t1 - t0) / INNER;
        }

        /* Reset */
        memcpy(in_c, (uint64_t[]){0x34A2F1B8C7D00ULL, 0x5E91C3A287600ULL,
                0x1F4D8B2E63A00ULL, 0x68C4F1D2A5B00ULL, 0x2B7E0A9C15D00ULL}, 40);

        /* Measure MAC128 version */
        for (int s = 0; s < SAMPLES; s++) {
                uint64_t t0 = rdtsc_start();
                for (int i = 0; i < INNER; i++) {
                        carry_square_mac128(out, in_m);
                        in_m[0] = out[0]; in_m[1] = out[1]; in_m[2] = out[2];
                        in_m[3] = out[3]; in_m[4] = out[4];
                }
                uint64_t t1 = rdtsc_end();
                mac_cycles[s] = (t1 - t0) / INNER;
        }

        /* Sort for percentiles */
        qsort(c_cycles, SAMPLES, sizeof(uint64_t), cmp_u64);
        qsort(mac_cycles, SAMPLES, sizeof(uint64_t), cmp_u64);

        uint64_t c_med   = c_cycles[SAMPLES / 2];
        uint64_t mac_med = mac_cycles[SAMPLES / 2];

        printf("  ┌──────────────────────┬────────┬────────┬────────┬────────┬────────┐\n");
        printf("  │ Version              │   p5   │  p25   │  p50   │  p75   │  p95   │\n");
        printf("  ├──────────────────────┼────────┼────────┼────────┼────────┼────────┤\n");
        printf("  │ C (gcc -O3)          │ %6" PRIu64 " │ %6" PRIu64 " │ %6" PRIu64 " │ %6" PRIu64 " │ %6" PRIu64 " │\n",
               c_cycles[SAMPLES*5/100], c_cycles[SAMPLES*25/100],
               c_med, c_cycles[SAMPLES*75/100], c_cycles[SAMPLES*95/100]);
        printf("  │ MAC128 (vmwrite)     │ %6" PRIu64 " │ %6" PRIu64 " │ %6" PRIu64 " │ %6" PRIu64 " │ %6" PRIu64 " │\n",
               mac_cycles[SAMPLES*5/100], mac_cycles[SAMPLES*25/100],
               mac_med, mac_cycles[SAMPLES*75/100], mac_cycles[SAMPLES*95/100]);
        printf("  └──────────────────────┴────────┴────────┴────────┴────────┴────────┘\n\n");

        if (mac_med < c_med) {
                printf("  MAC128 is %.2fx faster (%.1f%% fewer cycles)\n",
                       (double)c_med / mac_med,
                       (1.0 - (double)mac_med / c_med) * 100.0);
        } else if (mac_med > c_med) {
                printf("  MAC128 is %.2fx slower (%.1f%% more cycles)\n",
                       (double)mac_med / c_med,
                       ((double)mac_med / c_med - 1.0) * 100.0);
        } else {
                printf("  No measurable difference.\n");
        }

        free(c_cycles);
        free(mac_cycles);
}


/* ══════════════════════════════════════════════════════════════════ */

int main(void) {
        printf("╔══════════════════════════════════════════════╗\n");
        printf("║  Curve25519 carry_square: C vs MAC128         ║\n");
        printf("╚══════════════════════════════════════════════╝\n\n");

        printf("Installing MAC128 microcode...\n");
        install_mac128();
        printf("Done.\n\n");

        if (!validate()) {
                printf("VALIDATION FAILED — aborting.\n");
                return 1;
        }

        benchmark();

        return 0;
}
