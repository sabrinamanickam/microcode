/*
 * mac5_ldzx.c — Batched 5×MAC128 in one vmwrite via LDZX
 *
 * Amortizes CAM redirect cost (~5 cycles) across 5 multiply-accumulates
 * instead of paying it 5 times.
 *
 * vmwrite rcx, rdx  (hooked at 0x0cd8 → patch 0x7c00)
 *
 * INPUT:  RCX = pointer to uint64_t pairs[10] = {x0,y0,x1,y1,...,x4,y4}
 *         RAX = acc_lo (typically 0)
 *         R8  = acc_hi (typically 0)
 * OUTPUT: RAX = acc_lo, R8 = acc_hi   (acc += sum of x[i]*y[i])
 * CLOBBERS: RCX, RDX
 *
 * NOTE: Uses LDZX_DSZ64_ASZ32 — only ASZ32 opcodes exist in opcode.h.
 *       No _LDZX_DSZ64_ASZ64 opcode is defined.
 *       Compile with -no-pie to ensure data addresses fit in 32 bits.
 *
 * SEG: compile with -DSEG=N to set segment for LDZX.
 *      Try 0 (ES), 3 (DS), 6 (SS) — flat model should all be base=0.
 *
 * Build: make PROG=mac5_ldzx EXTRA_CFLAGS="-DSEG=3 -no-pie"
 *
 * 29 triads total (overlapped carry-tail / next-load pipeline).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#ifndef SEG
#define SEG 3
#endif

/* ------------------------------------------------------------------ */
/* Phase 0: LDZX smoke test — validates SEG before the big patch      */
/* ------------------------------------------------------------------ */
static int smoke_test_ldzx(void) {
        /* Tiny patch: load [RCX] → RAX, return */
        ucode_t smoke[] = {
                {
                        LDZX_DSZ64_ASZ32_SC1_DR(RAX, RCX, SEG),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, smoke, ARRAY_SZ(smoke));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);

        volatile uint64_t canary = 0xDEADBEEFCAFE1234ULL;
        uint64_t rax_out;

        asm volatile(
                "mov rax, 0\n\t"
                "lea rcx, [%[ptr]]\n\t"
                "xor rdx, rdx\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[out], rax\n\t"
                : [out] "=r"(rax_out)
                : [ptr] "m"(canary)
                : "rax", "rcx", "rdx", "r8", "memory"
        );

        printf("  LDZX smoke (SEG=%d): RAX = 0x%016" PRIx64, SEG, rax_out);
        if (rax_out == 0xDEADBEEFCAFE1234ULL) {
                printf("  PASS\n");
                return 1;
        } else {
                printf("  FAIL (expect 0xDEADBEEFCAFE1234)\n");
                return 0;
        }
}

/* ------------------------------------------------------------------ */
/* Phase 1: Install the batched 5×MAC128 patch                        */
/* ------------------------------------------------------------------ */
static void install_mac5(void) {
        ucode_t mac5_patch[] = {
                /*
                 * Register usage:
                 *   TMP4 = advancing pointer (starts at RCX input)
                 *   TMP3 = saved acc_lo before each MAC
                 *   TMP0, TMP1, TMP2 = carry chain temporaries
                 *   RAX  = acc_lo (persistent across MACs)
                 *   R8   = acc_hi (persistent across MACs)
                 *   RCX  = loaded multiplicand / prod_lo after MUL
                 *   RDX  = loaded multiplier   / prod_hi after MUL
                 */

                /* === T0: Setup — save pointer === */
                {
                        ZEROEXT_DSZ64_DR(TMP4, RCX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },

        /* ============================================================
         * MAC 0
         * ============================================================ */
                /* T1: load x0 from [TMP4], advance ptr */
                {
                        LDZX_DSZ64_ASZ32_SC1_DR(RCX, TMP4, SEG),
                        ADD_DSZ64_DRI(TMP4, TMP4, 8),
                        NOP,
                        NOP_SEQWORD
                },
                /* T2: load y0, advance ptr */
                {
                        LDZX_DSZ64_ASZ32_SC1_DR(RDX, TMP4, SEG),
                        ADD_DSZ64_DRI(TMP4, TMP4, 8),
                        NOP,
                        NOP_SEQWORD
                },
                /* T3: save acc_lo, multiply */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T4: acc_lo += prod_lo, carry chain start */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T5: carry cont + acc_hi += prod_hi */
                {
                        NOTAND_DSZ64_DRR(TMP0, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },

        /* ============================================================
         * Overlap MAC0→MAC1: carry tail + next loads + next MUL
         * ============================================================ */
                /* T6: merge carry, load x1, advance */
                {
                        OR_DSZ64_DRR(TMP1, TMP1, TMP0),
                        LDZX_DSZ64_ASZ32_SC1_DR(RCX, TMP4, SEG),
                        ADD_DSZ64_DRI(TMP4, TMP4, 8),
                        NOP_SEQWORD
                },
                /* T7: extract carry, load y1, advance */
                {
                        SHR_DSZ64_DRI(TMP1, TMP1, 63),
                        LDZX_DSZ64_ASZ32_SC1_DR(RDX, TMP4, SEG),
                        ADD_DSZ64_DRI(TMP4, TMP4, 8),
                        NOP_SEQWORD
                },
                /* T8: fold carry + save acc_lo + MUL */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP1),
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP_SEQWORD
                },

        /* ============================================================
         * MAC 1 core
         * ============================================================ */
                /* T9 */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T10 */
                {
                        NOTAND_DSZ64_DRR(TMP0, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },

        /* ============================================================
         * Overlap MAC1→MAC2
         * ============================================================ */
                /* T11 */
                {
                        OR_DSZ64_DRR(TMP1, TMP1, TMP0),
                        LDZX_DSZ64_ASZ32_SC1_DR(RCX, TMP4, SEG),
                        ADD_DSZ64_DRI(TMP4, TMP4, 8),
                        NOP_SEQWORD
                },
                /* T12 */
                {
                        SHR_DSZ64_DRI(TMP1, TMP1, 63),
                        LDZX_DSZ64_ASZ32_SC1_DR(RDX, TMP4, SEG),
                        ADD_DSZ64_DRI(TMP4, TMP4, 8),
                        NOP_SEQWORD
                },
                /* T13 */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP1),
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP_SEQWORD
                },

        /* ============================================================
         * MAC 2 core
         * ============================================================ */
                /* T14 */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T15 */
                {
                        NOTAND_DSZ64_DRR(TMP0, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },

        /* ============================================================
         * Overlap MAC2→MAC3
         * ============================================================ */
                /* T16 */
                {
                        OR_DSZ64_DRR(TMP1, TMP1, TMP0),
                        LDZX_DSZ64_ASZ32_SC1_DR(RCX, TMP4, SEG),
                        ADD_DSZ64_DRI(TMP4, TMP4, 8),
                        NOP_SEQWORD
                },
                /* T17 */
                {
                        SHR_DSZ64_DRI(TMP1, TMP1, 63),
                        LDZX_DSZ64_ASZ32_SC1_DR(RDX, TMP4, SEG),
                        ADD_DSZ64_DRI(TMP4, TMP4, 8),
                        NOP_SEQWORD
                },
                /* T18 */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP1),
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP_SEQWORD
                },

        /* ============================================================
         * MAC 3 core
         * ============================================================ */
                /* T19 */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T20 */
                {
                        NOTAND_DSZ64_DRR(TMP0, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },

        /* ============================================================
         * Overlap MAC3→MAC4
         * ============================================================ */
                /* T21 */
                {
                        OR_DSZ64_DRR(TMP1, TMP1, TMP0),
                        LDZX_DSZ64_ASZ32_SC1_DR(RCX, TMP4, SEG),
                        ADD_DSZ64_DRI(TMP4, TMP4, 8),
                        NOP_SEQWORD
                },
                /* T22 */
                {
                        SHR_DSZ64_DRI(TMP1, TMP1, 63),
                        LDZX_DSZ64_ASZ32_SC1_DR(RDX, TMP4, SEG),
                        ADD_DSZ64_DRI(TMP4, TMP4, 8),
                        NOP_SEQWORD
                },
                /* T23 */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP1),
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP_SEQWORD
                },

        /* ============================================================
         * MAC 4 core
         * ============================================================ */
                /* T24 */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T25 */
                {
                        NOTAND_DSZ64_DRR(TMP0, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },

        /* ============================================================
         * MAC 4 tail (no overlap — final carry)
         * ============================================================ */
                /* T26: merge carry */
                {
                        OR_DSZ64_DRR(TMP1, TMP1, TMP0),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T27: extract bit 63 */
                {
                        SHR_DSZ64_DRI(TMP1, TMP1, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T28: fold carry, done */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP1),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        patch_ucode(0x7c00, mac5_patch, ARRAY_SZ(mac5_patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
        printf("  Installed MAC5 patch: %zu triads\n", ARRAY_SZ(mac5_patch));
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static __uint128_t ref_mac5(const uint64_t pairs[10]) {
        __uint128_t acc = 0;
        for (int i = 0; i < 5; i++)
                acc += (__uint128_t)pairs[2*i] * pairs[2*i+1];
        return acc;
}

static void run_mac5(const uint64_t pairs[10],
                     uint64_t *lo_out, uint64_t *hi_out)
{
        uint64_t rax, r8;
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "lea rcx, [%[p]]\n\t"
                "xor rdx, rdx\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(rax), [hi] "=r"(r8)
                : [p] "m"(pairs[0])
                : "rax", "rcx", "rdx", "r8", "memory"
        );
        *lo_out = rax;
        *hi_out = r8;
}

static int check(const char *label, const uint64_t pairs[10]) {
        __uint128_t expect = ref_mac5(pairs);
        uint64_t exp_lo = (uint64_t)expect;
        uint64_t exp_hi = (uint64_t)(expect >> 64);

        uint64_t got_lo, got_hi;
        run_mac5(pairs, &got_lo, &got_hi);

        int ok = (got_lo == exp_lo && got_hi == exp_hi);
        printf("  %s: lo=0x%016" PRIx64 " hi=0x%016" PRIx64 "  %s\n",
               label, got_lo, got_hi, ok ? "PASS" : "FAIL");
        if (!ok) {
                printf("    expect lo=0x%016" PRIx64 " hi=0x%016" PRIx64 "\n",
                       exp_lo, exp_hi);
        }
        return ok;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */
int main(void) {
        int pass = 0, total = 0;

        printf("=== Phase 0: LDZX smoke test (SEG=%d) ===\n", SEG);
        if (!smoke_test_ldzx()) {
                printf("LDZX smoke FAILED — aborting. Try different -DSEG=N\n");
                return 1;
        }

        printf("\n=== Phase 1: Install 5×MAC128 batch patch ===\n");
        install_mac5();

        printf("\n=== Phase 2: Correctness tests ===\n");

        /* Test 1: small values, no carry */
        {
                uint64_t p[] = {7,13, 11,17, 3,5, 2,9, 4,6};
                /* 91 + 187 + 15 + 18 + 24 = 335 */
                total++; pass += check("small-nocry", p);
        }

        /* Test 2: single pair active, rest zero */
        {
                uint64_t p[] = {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
                                0,0, 0,0, 0,0, 0,0};
                total++; pass += check("max*max+0s ", p);
        }

        /* Test 3: force carry from low to hi */
        {
                uint64_t big = 0x4000000000000000ULL;
                uint64_t p[] = {big,4, big,4, big,4, big,4, big,4};
                total++; pass += check("carry-heavy", p);
        }

        /* Test 4: mixed sizes with carry */
        {
                uint64_t p[] = {
                        0xFFFFFFFFFFFFFFFFULL, 2,
                        0xFFFFFFFFFFFFFFFFULL, 3,
                        1, 1,
                        0, 99,
                        0x8000000000000000ULL, 2
                };
                total++; pass += check("mixed-carry", p);
        }

        /* Test 5: all ones — maximum stress */
        {
                uint64_t ones = 0xFFFFFFFFFFFFFFFFULL;
                uint64_t p[] = {ones,ones, ones,ones, ones,ones, ones,ones, ones,ones};
                total++; pass += check("5×max*max  ", p);
        }

        printf("\n=== Results: %d/%d passed ===\n", pass, total);
        return (pass == total) ? 0 : 1;
}