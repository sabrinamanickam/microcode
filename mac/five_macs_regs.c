/*
 * mac5_regs.c — Batched 5×MAC128 in one vmwrite, register-only
 *
 * vmwrite rcx, rdx  (hooked at 0x0cd8 → patch 0x7c00)
 * acc += limb[0..4] × scalar
 *
 * Register convention (set by caller before vmwrite):
 *   RBX = scalar multiplier
 *   RSI = limb[0]
 *   RDI = limb[1]
 *   R9  = limb[2]
 *   R10 = limb[3]
 *   R11 = limb[4]
 *   RAX = acc_lo (in/out)
 *   R8  = acc_hi (in/out)
 *   RCX, RDX = don't care (vmwrite operands, clobbered by MUL)
 *
 * 23 triads. No memory loads. Overlapped carry-tail / setup pipeline.
 *
 * Build: make PROG=mac5_regs
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* ------------------------------------------------------------------ */
/* Phase 0: Smoke test — can ZEROEXT read RSI from microcode?         */
/* ------------------------------------------------------------------ */
static int smoke_test_regs(void) {
        ucode_t smoke[] = {
                {
                        ZEROEXT_DSZ64_DR(RAX, RSI),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, smoke, ARRAY_SZ(smoke));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);

        uint64_t rax_out;
        uint64_t canary = 0xCAFEBABE12345678ULL;

        asm volatile(
                "mov rsi, %[c]\n\t"
                "xor rax, rax\n\t"
                "xor rcx, rcx\n\t"
                "xor rdx, rdx\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[out], rax\n\t"
                : [out] "=r"(rax_out)
                : [c] "r"(canary)
                : "rax", "rcx", "rdx", "rsi", "r8"
        );

        printf("  Smoke (ZEROEXT RSI→RAX): 0x%016" PRIx64, rax_out);
        if (rax_out == canary) {
                printf("  PASS\n");
                return 1;
        } else {
                printf("  FAIL\n");
                return 0;
        }
}

/* ------------------------------------------------------------------ */
/* Phase 1: Install batched 5×MAC128 patch                            */
/* ------------------------------------------------------------------ */
static void install_mac5(void) {
        ucode_t mac5_patch[] = {
                /*
                 * 23 triads, overlapped pipeline.
                 *
                 * Per-MAC structure (non-overlapped, for reference):
                 *  A: ZEROEXT(RCX, limb), ZEROEXT(RDX, RBX)  [setup]
                 *  B: ZEROEXT(TMP3, RAX), MUL                 [save+mul]
                 *  C: ADD + AND + OR                           [acc + carry start]
                 *  D: NOTAND + ADD(R8+=prod_hi)                [carry cont + hi]
                 *  E: OR(carry merge)
                 *  F: SHR(carry extract)
                 *  G: ADD(R8+=carry)                           [carry fold]
                 *
                 * Overlapped: E+A, F+B merged into single triads.
                 * G+C merged (carry fold + next acc) — 3 ops, 0 conflicts.
                 * D gets extra AND (moved from C) — 3 ops.
                 *
                 * Overlap = 4 triads/transition vs 7 non-overlapped.
                 */

        /* ============================================================
         * MAC 0: limb[0] in RSI × scalar in RBX
         * ============================================================ */
                /* T0: setup */
                {
                        ZEROEXT_DSZ64_DR(RCX, RSI),
                        ZEROEXT_DSZ64_DR(RDX, RBX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1: save acc_lo + multiply */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T2: accumulate + carry chain start */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T3: carry cont + acc_hi += prod_hi */
                {
                        NOTAND_DSZ64_DRR(TMP0, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },

        /* ============================================================
         * Overlap MAC0→MAC1: limb[1] in RDI
         * E+A: carry merge + setup
         * F+B: carry extract + save + MUL
         * G+C: carry fold + accumulate + OR
         * D:   carry cont + hi acc + AND
         * ============================================================ */
                /* T4 (E+A) */
                {
                        OR_DSZ64_DRR(TMP1, TMP1, TMP0),
                        ZEROEXT_DSZ64_DR(RCX, RDI),
                        ZEROEXT_DSZ64_DR(RDX, RBX),
                        NOP_SEQWORD
                },
                /* T5 (F+B) */
                {
                        SHR_DSZ64_DRI(TMP1, TMP1, 63),
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP_SEQWORD
                },
                /* T6 (G+C) */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP1),
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T7 (D) */
                {
                        NOTAND_DSZ64_DRR(TMP0, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        NOP_SEQWORD
                },

        /* ============================================================
         * Overlap MAC1→MAC2: limb[2] in R9
         * ============================================================ */
                /* T8 (E+A) */
                {
                        OR_DSZ64_DRR(TMP1, TMP1, TMP0),
                        ZEROEXT_DSZ64_DR(RCX, R9),
                        ZEROEXT_DSZ64_DR(RDX, RBX),
                        NOP_SEQWORD
                },
                /* T9 (F+B) */
                {
                        SHR_DSZ64_DRI(TMP1, TMP1, 63),
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP_SEQWORD
                },
                /* T10 (G+C) */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP1),
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T11 (D) */
                {
                        NOTAND_DSZ64_DRR(TMP0, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        NOP_SEQWORD
                },

        /* ============================================================
         * Overlap MAC2→MAC3: limb[3] in R10
         * ============================================================ */
                /* T12 (E+A) */
                {
                        OR_DSZ64_DRR(TMP1, TMP1, TMP0),
                        ZEROEXT_DSZ64_DR(RCX, R10),
                        ZEROEXT_DSZ64_DR(RDX, RBX),
                        NOP_SEQWORD
                },
                /* T13 (F+B) */
                {
                        SHR_DSZ64_DRI(TMP1, TMP1, 63),
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP_SEQWORD
                },
                /* T14 (G+C) */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP1),
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T15 (D) */
                {
                        NOTAND_DSZ64_DRR(TMP0, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        NOP_SEQWORD
                },

        /* ============================================================
         * Overlap MAC3→MAC4: limb[4] in R11
         * ============================================================ */
                /* T16 (E+A) */
                {
                        OR_DSZ64_DRR(TMP1, TMP1, TMP0),
                        ZEROEXT_DSZ64_DR(RCX, R11),
                        ZEROEXT_DSZ64_DR(RDX, RBX),
                        NOP_SEQWORD
                },
                /* T17 (F+B) */
                {
                        SHR_DSZ64_DRI(TMP1, TMP1, 63),
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP_SEQWORD
                },
                /* T18 (G+C) */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP1),
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T19 (D) */
                {
                        NOTAND_DSZ64_DRR(TMP0, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        NOP_SEQWORD
                },

        /* ============================================================
         * MAC 4 tail — final carry (no next MAC to overlap with)
         * ============================================================ */
                /* T20 (E): merge carry */
                {
                        OR_DSZ64_DRR(TMP1, TMP1, TMP0),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T21 (F): extract bit 63 */
                {
                        SHR_DSZ64_DRI(TMP1, TMP1, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T22 (G): fold carry, done */
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
static __uint128_t ref_mac5(uint64_t scalar,
                            uint64_t l0, uint64_t l1, uint64_t l2,
                            uint64_t l3, uint64_t l4) {
        __uint128_t acc = 0;
        acc += (__uint128_t)l0 * scalar;
        acc += (__uint128_t)l1 * scalar;
        acc += (__uint128_t)l2 * scalar;
        acc += (__uint128_t)l3 * scalar;
        acc += (__uint128_t)l4 * scalar;
        return acc;
}

static void run_mac5(uint64_t scalar,
                     uint64_t l0, uint64_t l1, uint64_t l2,
                     uint64_t l3, uint64_t l4,
                     uint64_t *lo_out, uint64_t *hi_out) {
        uint64_t rax, r8;
        /* Pack into array so we only need one pointer input */
        uint64_t args[6] = { scalar, l0, l1, l2, l3, l4 };
        asm volatile(
                "mov rbx, [%[p]+0]\n\t"    /* scalar */
                "mov rsi, [%[p]+8]\n\t"    /* limb[0] */
                "mov rdi, [%[p]+16]\n\t"   /* limb[1] */
                "mov r9,  [%[p]+24]\n\t"   /* limb[2] */
                "mov r10, [%[p]+32]\n\t"   /* limb[3] */
                "mov r11, [%[p]+40]\n\t"   /* limb[4] */
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "xor rcx, rcx\n\t"
                "xor rdx, rdx\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(rax), [hi] "=r"(r8)
                : [p] "r"(args)
                : "rax", "rbx", "rcx", "rdx",
                  "rsi", "rdi", "r8", "r9", "r10", "r11", "memory"
        );
        *lo_out = rax;
        *hi_out = r8;
}

static int check(const char *label, uint64_t scalar,
                 uint64_t l0, uint64_t l1, uint64_t l2,
                 uint64_t l3, uint64_t l4) {
        __uint128_t expect = ref_mac5(scalar, l0, l1, l2, l3, l4);
        uint64_t exp_lo = (uint64_t)expect;
        uint64_t exp_hi = (uint64_t)(expect >> 64);

        uint64_t got_lo, got_hi;
        run_mac5(scalar, l0, l1, l2, l3, l4, &got_lo, &got_hi);

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

        printf("=== Phase 0: Register smoke test ===\n");
        if (!smoke_test_regs()) {
                printf("Register smoke FAILED — RSI not readable in ucode?\n");
                return 1;
        }

        printf("\n=== Phase 1: Install 5×MAC128 batch (23 triads) ===\n");
        install_mac5();

        printf("\n=== Phase 2: Correctness tests ===\n");

        /* Test 1: small, no carry */
        total++; pass += check("small    ",
                3, 7, 11, 13, 17, 19);
        /* 3×(7+11+13+17+19) = 3×67 = 201 */

        /* Test 2: one limb active */
        total++; pass += check("one-limb ",
                0xFFFFFFFFFFFFFFFFULL,
                0xFFFFFFFFFFFFFFFFULL,
                0, 0, 0, 0);

        /* Test 3: carry-heavy */
        total++; pass += check("carry-hvy",
                4,
                0x4000000000000000ULL,
                0x4000000000000000ULL,
                0x4000000000000000ULL,
                0x4000000000000000ULL,
                0x4000000000000000ULL);

        /* Test 4: mixed with carry */
        total++; pass += check("mixed    ",
                2,
                0xFFFFFFFFFFFFFFFFULL,
                0x8000000000000000ULL,
                1, 0, 0x7FFFFFFFFFFFFFFFULL);

        /* Test 5: all max — stress */
        {
                uint64_t m = 0xFFFFFFFFFFFFFFFFULL;
                total++; pass += check("5×max×max",
                        m, m, m, m, m, m);
        }

        printf("\n=== Results: %d/%d passed ===\n", pass, total);
        return (pass == total) ? 0 : 1;
}