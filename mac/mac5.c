/*
 * mac5.c — 5×MAC128 in one vmwrite, SETCC_CONDB carry (12 triads)
 *
 * vmwrite rcx, rdx  (hooked at 0x0cd8 → patch 0x7c00)
 * RAX:R8 += RCX×RDX + R9×R10 + R11×R14 + RSI×RDI + RBX×R12
 *
 * Register convention:
 *   Pair 1: RCX × RDX    (clobbered by MUL pipeline)
 *   Pair 2: R9  × R10
 *   Pair 3: R11 × R14
 *   Pair 4: RSI × RDI
 *   Pair 5: RBX × R12
 *   RAX = acc_lo (in/out)
 *   R8  = acc_hi (in/out)
 *
 * 12 triads.  SETCC_CONDB carry detection.  No memory loads.
 * Carry-fold for hi terms overlapped with MUL pipeline.
 *
 * MUL convention: MUL(dst, src0, src1) → hi→dst, lo→src1
 *
 * Build: make PROG=mac5
 * Run:   sudo taskset -c 0 ./mac5_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* ── Install 12-triad MAC5 patch ─────────────────────────────── */
static void install_mac5(void) {
        ucode_t mac5_patch[] = {
                /*
                 * 12-triad 5×MAC128, SETCC_CONDB carry detection.
                 *
                 * Pipeline: for each MAC pair (a,b):
                 *   Even triad: fold prev carry+hi, issue MUL(a,b)
                 *   Odd triad:  ADD acc_lo += prod_lo, SETCC carry
                 *
                 * Hi terms folded into R8 in slot 2 of MUL triads
                 * (starting from T4, once first hi term is ready).
                 *
                 * TMP usage: TMP0,TMP2 (acc ping-pong), TMP3 (carry),
                 *            TMP4,TMP5 (hi staging, reused after fold)
                 */

                /* T0: save acc_lo, multiply pair 1 → hi=RCX, lo=RDX */
                {
                        ZEROEXT_DSZ64_DR(TMP0, RAX),
                        MUL_DSZ64_DRR(RCX, RCX, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1: acc += p1_lo, carry1 */
                {
                        ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                        SETCC_CONDB_DR(TMP3, TMP2),
                        NOP,
                        NOP_SEQWORD
                },
                /* T2: TMP4 = p1_hi+carry1, multiply pair 2 → hi=RCX, lo=R10 */
                {
                        ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                        MUL_DSZ64_DRR(RCX, R9, R10),
                        NOP,
                        NOP_SEQWORD
                },
                /* T3: acc += p2_lo, carry2 */
                {
                        ADD_DSZ64_DRR(TMP0, TMP2, R10),
                        SETCC_CONDB_DR(TMP3, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                /* T4: TMP5 = p2_hi+carry2, multiply pair 3 → hi=RCX, lo=R14.
                 *     Fold: R8 += TMP4 (p1_hi+c1) */
                {
                        ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                        MUL_DSZ64_DRR(RCX, R11, R14),
                        ADD_DSZ64_DRR(R8, R8, TMP4),
                        NOP_SEQWORD
                },
                /* T5: acc += p3_lo, carry3 */
                {
                        ADD_DSZ64_DRR(TMP2, TMP0, R14),
                        SETCC_CONDB_DR(TMP3, TMP2),
                        NOP,
                        NOP_SEQWORD
                },
                /* T6: TMP4 = p3_hi+carry3, multiply pair 4 → hi=RCX, lo=RDI.
                 *     Fold: R8 += TMP5 (p2_hi+c2) */
                {
                        ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                        MUL_DSZ64_DRR(RCX, RSI, RDI),
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP_SEQWORD
                },
                /* T7: acc += p4_lo, carry4 */
                {
                        ADD_DSZ64_DRR(TMP0, TMP2, RDI),
                        SETCC_CONDB_DR(TMP3, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                /* T8: TMP5 = p4_hi+carry4, multiply pair 5 → hi=RCX, lo=R12.
                 *     Fold: R8 += TMP4 (p3_hi+c3) */
                {
                        ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                        MUL_DSZ64_DRR(RCX, RBX, R12),
                        ADD_DSZ64_DRR(R8, R8, TMP4),
                        NOP_SEQWORD
                },
                /* T9: acc += p5_lo, carry5 */
                {
                        ADD_DSZ64_DRR(TMP2, TMP0, R12),
                        SETCC_CONDB_DR(TMP3, TMP2),
                        NOP,
                        NOP_SEQWORD
                },
                /* T10: finalize acc_lo, TMP4 = p5_hi+carry5, fold p4_hi */
                {
                        ZEROEXT_DSZ64_DR(RAX, TMP2),
                        ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP_SEQWORD
                },
                /* T11: fold last hi term, done */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP4),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7d7c, mac5_patch, ARRAY_SZ(mac5_patch));
        hook_match_and_patch(18, 0x0cd8, 0x7d7c);
        printf("Installed MAC5 patch: %zu triads (SETCC_CONDB carry)\n",
               ARRAY_SZ(mac5_patch));
}

/* ── Reference: software 5-MAC ───────────────────────────────── */
static __uint128_t ref_mac5(uint64_t a1, uint64_t b1,
                            uint64_t a2, uint64_t b2,
                            uint64_t a3, uint64_t b3,
                            uint64_t a4, uint64_t b4,
                            uint64_t a5, uint64_t b5) {
        __uint128_t acc = 0;
        acc += (__uint128_t)a1 * b1;
        acc += (__uint128_t)a2 * b2;
        acc += (__uint128_t)a3 * b3;
        acc += (__uint128_t)a4 * b4;
        acc += (__uint128_t)a5 * b5;
        return acc;
}

/* ── Execute vmwrite MAC5 ────────────────────────────────────── */
static void run_mac5(uint64_t a1, uint64_t b1,
                     uint64_t a2, uint64_t b2,
                     uint64_t a3, uint64_t b3,
                     uint64_t a4, uint64_t b4,
                     uint64_t a5, uint64_t b5,
                     uint64_t *lo_out, uint64_t *hi_out) {
        uint64_t rax, r8;
        asm volatile(
                "mov rcx, %[a1]\n\t"
                "mov rdx, %[b1]\n\t"
                "mov r9,  %[a2]\n\t"
                "mov r10, %[b2]\n\t"
                "mov r11, %[a3]\n\t"
                "mov r14, %[b3]\n\t"
                "mov rsi, %[a4]\n\t"
                "mov rdi, %[b4]\n\t"
                "mov rbx, %[a5]\n\t"
                "mov r12, %[b5]\n\t"
                "xor eax, eax\n\t"
                "xor r8d, r8d\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(rax), [hi] "=r"(r8)
                : [a1] "m"(a1), [b1] "m"(b1),
                  [a2] "m"(a2), [b2] "m"(b2),
                  [a3] "m"(a3), [b3] "m"(b3),
                  [a4] "m"(a4), [b4] "m"(b4),
                  [a5] "m"(a5), [b5] "m"(b5)
                : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
                  "r8", "r9", "r10", "r11", "r12", "r14", "memory"
        );
        *lo_out = rax;
        *hi_out = r8;
}

/* ── Correctness check ───────────────────────────────────────── */
static int check(const char *label,
                 uint64_t a1, uint64_t b1,
                 uint64_t a2, uint64_t b2,
                 uint64_t a3, uint64_t b3,
                 uint64_t a4, uint64_t b4,
                 uint64_t a5, uint64_t b5) {
        __uint128_t expect = ref_mac5(a1,b1, a2,b2, a3,b3, a4,b4, a5,b5);
        uint64_t exp_lo = (uint64_t)expect;
        uint64_t exp_hi = (uint64_t)(expect >> 64);

        uint64_t got_lo, got_hi;
        run_mac5(a1,b1, a2,b2, a3,b3, a4,b4, a5,b5, &got_lo, &got_hi);

        int ok = (got_lo == exp_lo && got_hi == exp_hi);
        printf("  %-14s lo=%016" PRIx64 " hi=%016" PRIx64 "  %s\n",
               label, got_lo, got_hi, ok ? "PASS" : "FAIL");
        if (!ok)
                printf("    expect   lo=%016" PRIx64 " hi=%016" PRIx64 "\n",
                       exp_lo, exp_hi);
        return ok;
}

/* ── Benchmark helpers ───────────────────────────────────────── */
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
        asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
        asm volatile("cpuid" ::: "rax", "rbx", "rcx", "rdx");
        return ((uint64_t)hi << 32) | lo;
}

#define BATCH 1000
#define REPS  100

int main(void) {
        uint64_t t0, t1, min, sum;
        int pass = 0, total = 0;

        printf("=== MAC5: 12-triad SETCC_CONDB (5 MACs / vmwrite) ===\n\n");
        install_mac5();

        /* ── Correctness tests ───────────────────────────────── */
        printf("\n--- Correctness ---\n");

        /* Small, no carry */
        total++; pass += check("small",
                3, 7,  5, 11,  2, 13,  1, 17,  4, 19);
        /* 21+55+26+17+76 = 195 */

        /* One pair active */
        total++; pass += check("one-pair",
                0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
                0, 0,  0, 0,  0, 0,  0, 0);

        /* Carry-heavy: 5 × (2^63 × 2) = 5 × 2^64 = {5, 0} */
        total++; pass += check("carry-hvy",
                0x8000000000000000ULL, 2,
                0x8000000000000000ULL, 2,
                0x8000000000000000ULL, 2,
                0x8000000000000000ULL, 2,
                0x8000000000000000ULL, 2);

        /* Mixed with carry */
        total++; pass += check("mixed",
                0xFFFFFFFFFFFFFFFFULL, 2,
                0x8000000000000000ULL, 3,
                1, 1,
                0, 0x7FFFFFFFFFFFFFFFULL,
                0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL);

        /* All max — stress test */
        {
                uint64_t m = 0xFFFFFFFFFFFFFFFFULL;
                total++; pass += check("5×max×max",
                        m, m,  m, m,  m, m,  m, m,  m, m);
        }

        /* Only pair 4 (RSI×RDI) — test these regs work in MUL */
        total++; pass += check("pair4-only",
                0, 0,  0, 0,  0, 0,
                0xDEADBEEFCAFEBABEULL, 0x1234567890ABCDEFULL,
                0, 0);

        /* Only pair 5 (RBX×R12) */
        total++; pass += check("pair5-only",
                0, 0,  0, 0,  0, 0,  0, 0,
                0xDEADBEEFCAFEBABEULL, 0x1234567890ABCDEFULL);

        printf("\n  Results: %d/%d passed\n", pass, total);
        if (pass != total) {
                fprintf(stderr, "FAILED — aborting benchmark.\n");
                return 1;
        }

        /* ── Benchmark ───────────────────────────────────────── */
        printf("\n--- Benchmark: %d ops/batch, %d batches ---\n\n", BATCH, REPS);

        uint64_t x0 = 0x0007FFFFFFFFFFFFULL, y0 = 0x0006000000000000ULL;
        uint64_t x1 = 0x0005FFFFFFFFFFFFULL, y1 = 0x0004000000000000ULL;
        uint64_t x2 = 0x0003FFFFFFFFFFFFULL, y2 = 0x0002000000000000ULL;
        uint64_t x3 = 0x0001FFFFFFFFFFFFULL, y3 = 0x0001000000000000ULL;
        uint64_t x4 = 0x0000FFFFFFFFFFFFULL, y4 = 0x0000800000000000ULL;

        /* Pack pairs into array so asm only needs one pointer */
        uint64_t pairs[10] = { x0,y0, x1,y1, x2,y2, x3,y3, x4,y4 };

        /* ── MAC5 via vmwrite ────────────────────────────────── */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "mov rcx, [%[p]+0]\n\t"
                                "mov rdx, [%[p]+8]\n\t"
                                "mov r9,  [%[p]+16]\n\t"
                                "mov r10, [%[p]+24]\n\t"
                                "mov r11, [%[p]+32]\n\t"
                                "mov r14, [%[p]+40]\n\t"
                                "mov rsi, [%[p]+48]\n\t"
                                "mov rdi, [%[p]+56]\n\t"
                                "mov rbx, [%[p]+64]\n\t"
                                "mov r12, [%[p]+72]\n\t"
                                "xor eax, eax\n\t"
                                "xor r8d, r8d\n\t"
                                "vmwrite rcx, rdx\n\t"
                                :
                                : [p] "r"(pairs)
                                : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
                                  "r8", "r9", "r10", "r11", "r12", "r14",
                                  "memory"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt; if (dt < min) min = dt;
        }
        printf("  MAC5 vmwrite:     min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles"
               "  (1 vmwrite, 5 MACs, 12 triads)\n",
               min / BATCH, sum / REPS / BATCH);

        /* ── Native 5× MUL+ADD+ADC (registers) ──────────────── */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "xor r8, r8\n\t"
                                "xor r9, r9\n\t"

                                "mov rax, %[x0]\n\t" "mul %[y0]\n\t"
                                "add r9, rax\n\t"     "adc r8, rdx\n\t"

                                "mov rax, %[x1]\n\t" "mul %[y1]\n\t"
                                "add r9, rax\n\t"     "adc r8, rdx\n\t"

                                "mov rax, %[x2]\n\t" "mul %[y2]\n\t"
                                "add r9, rax\n\t"     "adc r8, rdx\n\t"

                                "mov rax, %[x3]\n\t" "mul %[y3]\n\t"
                                "add r9, rax\n\t"     "adc r8, rdx\n\t"

                                "mov rax, %[x4]\n\t" "mul %[y4]\n\t"
                                "add r9, rax\n\t"     "adc r8, rdx\n\t"
                                :
                                : [x0] "r"(x0), [y0] "r"(y0),
                                  [x1] "r"(x1), [y1] "r"(y1),
                                  [x2] "r"(x2), [y2] "r"(y2),
                                  [x3] "r"(x3), [y3] "r"(y3),
                                  [x4] "r"(x4), [y4] "r"(y4)
                                : "rax", "rdx", "r8", "r9"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt; if (dt < min) min = dt;
        }
        printf("  Native 5×MUL:     min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles"
               "  (5 MUL+ADC, register ops)\n",
               min / BATCH, sum / REPS / BATCH);

        /* ── What GCC emits for __uint128_t accumulation ─────── */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                __uint128_t acc;
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        acc = 0;
                        acc += (__uint128_t)x0 * y0;
                        acc += (__uint128_t)x1 * y1;
                        acc += (__uint128_t)x2 * y2;
                        acc += (__uint128_t)x3 * y3;
                        acc += (__uint128_t)x4 * y4;
                        /* prevent optimization */
                        asm volatile("" :: "r"((uint64_t)acc),
                                          "r"((uint64_t)(acc>>64)));
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt; if (dt < min) min = dt;
        }
        printf("  GCC __uint128_t:  min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles"
               "  (compiler-generated 5×MUL)\n",
               min / BATCH, sum / REPS / BATCH);

        /* ── Analysis ────────────────────────────────────────── */
        printf("\n--- Analysis for Curve25519 fe_sq ---\n\n");
        printf("  fe_sq = 5 limbs × 3 MACs/limb = 15 MACs total\n");
        printf("  Carry must propagate between limbs (sequential)\n\n");
        printf("  mac3 (9 triads, SETCC):  5 vmwrites × ~14 cy = ~70 cy MAC\n");
        printf("  mac5 (12 triads, SETCC): 5 vmwrites × ~17 cy = ~85 cy MAC\n");
        printf("    (still 5 calls — 3 MACs/limb can't fill 5 slots)\n\n");
        printf("  mac5 is WORSE for fe_sq.  mac3 is optimal for 3-MACs/limb.\n\n");
        printf("  To beat GCC: monolithic fe_sq patch (1 vmwrite, all 15 MACs\n");
        printf("  + carry propagation in microcode, ~35-40 triads, ~1 redirect)\n");

        return 0;
}
