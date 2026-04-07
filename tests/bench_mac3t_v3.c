/*
 * test_setcc_after_mul.c — Diagnose SETCC_CONDB failure after MUL
 *
 * ═══════════════════════════════════════════════════════════════════
 *  Standalone ADD+SETCC works (6/6 pass). But inside the MAC hook
 *  where MUL precedes ADD+SETCC, carries are lost.
 *
 *  Hypothesis: MUL in T0 sets internal flags that persist into T1,
 *  and SETCC reads MUL's flags instead of ADD's flags.
 *
 *  Tests:
 *  1) SETCC alone (baseline — confirmed working)
 *  2) NOP in T0, then ADD+SETCC in T1 (cross-triad, no MUL)
 *  3) MUL in T0, then ADD+SETCC in T1 (the failing case)
 *  4) MUL in T0, GENARITHFLAGS+SETCC in T1 (explicit re-gen)
 *  5) MUL+GENARITHFLAGS in T0, SETCC in T1 (clear MUL flags?)
 *  6) MUL in T0, NOP in T1, ADD+SETCC in T2 (extra gap)
 *
 *  For each: RAX=0xFFFFFFFFFFFFFFFF, RCX=1 → ADD should overflow,
 *  CF=1, SETCC should return 1.
 *
 *  Build:  make PROG=test_setcc_after_mul
 *  Run:    sudo taskset -c 0 ./test_setcc_after_mul_static
 * ═══════════════════════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/*
 * All hooks:
 *   Input: RAX = operand a, RCX = operand b
 *          RDX = MUL multiplier (set to small value for predictable product)
 *   Output: RAX = SETCC result (expect 1 when a+b overflows)
 *
 * We use fixed values for MUL: RCX=multiplier, RDX=multiplicand
 * But since MUL overwrites RCX/RDX, we save the ADD operands first.
 */


/* ══════════════════════════════════════════════════════════════════
 *  Test 1: Baseline — ADD + SETCC same triad, no MUL (known good)
 *
 *  T0: ADD TMP0, RAX, RCX | SETCC_CONDB TMP1, TMP0 | NOP
 *  T1: ZEROEXT RAX, TMP1 | END
 * ══════════════════════════════════════════════════════════════════ */
static void install_test1(void) {
        ucode_t patch[] = {
                {
                        ADD_DSZ64_DRR(TMP0, RAX, RCX),
                        SETCC_CONDB_DR(TMP1, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ZEROEXT_DSZ64_DR(RAX, TMP1),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 2);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  Test 2: NOP triad before ADD+SETCC (cross-triad, no MUL)
 *
 *  T0: NOP | NOP | NOP
 *  T1: ADD TMP0, RAX, RCX | SETCC_CONDB TMP1, TMP0 | NOP
 *  T2: ZEROEXT RAX, TMP1 | END
 * ══════════════════════════════════════════════════════════════════ */
static void install_test2(void) {
        ucode_t patch[] = {
                {
                        NOP,
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(TMP0, RAX, RCX),
                        SETCC_CONDB_DR(TMP1, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ZEROEXT_DSZ64_DR(RAX, TMP1),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 3);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  Test 3: MUL in T0 slot 1, then ADD+SETCC in T1
 *  (This mimics the exact MAC structure that fails)
 *
 *  Setup: RAX = a, RCX = b
 *  T0: ZEROEXT TMP3, RAX   | MUL TMP4, RCX, RDX  | NOP
 *      (save a into TMP3)    (MUL clobbers RCX,RDX but we don't care)
 *      (use TMP4 as MUL dst so it doesn't touch our regs)
 *  T1: ADD TMP0, TMP3, RCX | SETCC_CONDB TMP1, TMP0 | NOP
 *
 *  Wait — MUL overwrites RCX (R64SRC) with product_lo. So we can't
 *  read RCX in T1 as the original b. This is a problem. The actual
 *  MAC uses the MUL product (RCX) as the ADD operand.
 *
 *  Let me restructure: save both operands before MUL, then use them.
 *
 *  T0: ZEROEXT TMP3, RAX | ZEROEXT TMP4, RCX | NOP
 *  T1: MUL TMP5, RCX, RDX | NOP | NOP
 *       (MUL clobbers RCX, RDX — we don't use them after)
 *  T2: ADD TMP0, TMP3, TMP4 | SETCC_CONDB TMP1, TMP0 | NOP
 *  T3: ZEROEXT RAX, TMP1 | END
 * ══════════════════════════════════════════════════════════════════ */
static void install_test3(void) {
        ucode_t patch[] = {
                /* T0: save operands */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        ZEROEXT_DSZ64_DR(TMP4, RCX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1: MUL (just to pollute flags) */
                {
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T2: ADD + SETCC */
                {
                        ADD_DSZ64_DRR(TMP0, TMP3, TMP4),
                        SETCC_CONDB_DR(TMP1, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                /* T3: return */
                {
                        ZEROEXT_DSZ64_DR(RAX, TMP1),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 4);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  Test 4: MUL + GENARITHFLAGS(ADD) + SETCC same triad
 *  If MUL pollutes flags, maybe explicit GENARITHFLAGS clears them.
 *
 *  T0: ZEROEXT TMP3, RAX | ZEROEXT TMP4, RCX | NOP
 *  T1: MUL TMP5, RCX, RDX | NOP | NOP
 *  T2: ADD TMP0, TMP3, TMP4 | NOP | NOP
 *  T3: GENARITHFLAGS_RR TMP0, TMP3 | SETCC_CONDB TMP1, TMP0 | NOP
 *  T4: ZEROEXT RAX, TMP1 | END
 * ══════════════════════════════════════════════════════════════════ */
static void install_test4(void) {
        ucode_t patch[] = {
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        ZEROEXT_DSZ64_DR(TMP4, RCX),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(TMP0, TMP3, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        GENARITHFLAGS_RR(TMP0, TMP3),
                        SETCC_CONDB_DR(TMP1, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ZEROEXT_DSZ64_DR(RAX, TMP1),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 5);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  Test 5: Exact MAC structure — ZEROEXT+MUL in T0, ADD+SETCC in T1
 *
 *  This is as close to the real MAC as possible, but we control
 *  the values. We ADD the saved acc (TMP3) + product_lo (RCX from MUL).
 *
 *  T0: ZEROEXT TMP3, RAX | MUL R64SRC, R64SRC, R64DST | NOP
 *  T1: ADD TMP0, TMP3, RCX | SETCC_CONDB TMP1, TMP0 | NOP
 *  T2: ZEROEXT RAX, TMP1 | END
 *
 *  Input: RAX = big (near max), RCX & RDX = values that produce
 *         a product_lo that when added to RAX causes overflow.
 * ══════════════════════════════════════════════════════════════════ */
static void install_test5(void) {
        ucode_t patch[] = {
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(TMP0, TMP3, RCX),
                        SETCC_CONDB_DR(TMP1, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ZEROEXT_DSZ64_DR(RAX, TMP1),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 3);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  Test 6: Same as 5 but with NOP gap between MUL triad and ADD triad
 *
 *  T0: ZEROEXT TMP3, RAX | MUL R64SRC, R64SRC, R64DST | NOP
 *  T1: NOP | NOP | NOP
 *  T2: ADD TMP0, TMP3, RCX | SETCC_CONDB TMP1, TMP0 | NOP
 *  T3: ZEROEXT RAX, TMP1 | END
 * ══════════════════════════════════════════════════════════════════ */
static void install_test6(void) {
        ucode_t patch[] = {
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        NOP,
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(TMP0, TMP3, RCX),
                        SETCC_CONDB_DR(TMP1, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ZEROEXT_DSZ64_DR(RAX, TMP1),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 4);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  Test 7: READAFLAGS after MUL — see what flags MUL sets
 *
 *  T0: MUL R64SRC, R64SRC, R64DST | NOP | NOP
 *  T1: READAFLAGS_DR TMP1, RAX | NOP | NOP
 *  T2: ZEROEXT RAX, TMP1 | END
 * ══════════════════════════════════════════════════════════════════ */
static void install_test7(void) {
        ucode_t patch[] = {
                {
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        READAFLAGS_DR(TMP1, RAX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ZEROEXT_DSZ64_DR(RAX, TMP1),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 3);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  HARNESS
 * ══════════════════════════════════════════════════════════════════ */
static uint64_t invoke(uint64_t rax_val, uint64_t rcx_val, uint64_t rdx_val) {
        uint64_t result;
        asm volatile(
                "mov rax, %[a]\n\t"
                "mov rcx, %[c]\n\t"
                "mov rdx, %[d]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[out], rax\n\t"
                : [out] "=r"(result)
                : [a] "r"(rax_val), [c] "r"(rcx_val), [d] "r"(rdx_val)
                : "rax", "rcx", "rdx", "r8"
        );
        return result;
}

typedef struct {
        const char *desc;
        uint64_t rax, rcx, rdx;
        uint64_t expect;  /* expected SETCC result, or 0xDEAD for "dump" */
} tcase_t;

/* For tests 1-6: ADD(RAX + RCX) → SETCC. RDX used as MUL operand. */
static tcase_t setcc_cases[] = {
        { "no overflow:  1 + 1",
          1, 1, 3, 0 },
        { "overflow:     0xFF..F + 1",
          0xFFFFFFFFFFFFFFFFULL, 1, 3, 1 },
        { "overflow:     0xFF..F + 2",
          0xFFFFFFFFFFFFFFFFULL, 2, 3, 1 },
        { "overflow:     0x80..0 + 0x80..0",
          0x8000000000000000ULL, 0x8000000000000000ULL, 3, 1 },
        { "no overflow:  0x7F..F + 1",
          0x7FFFFFFFFFFFFFFFULL, 1, 3, 0 },
        { "no overflow:  0 + 0",
          0, 0, 3, 0 },
};
static int nsetcc = sizeof(setcc_cases) / sizeof(setcc_cases[0]);

/* For test 5: ACC=RAX, MUL(RCX*RDX)→product_lo in RCX. ADD(ACC+product_lo). */
static tcase_t mac_cases[] = {
        { "acc=0xFF..F, mul=1*1=1 → overflow",
          0xFFFFFFFFFFFFFFFFULL, 1, 1, 1 },
        { "acc=0xFF..F, mul=2*3=6 → overflow",
          0xFFFFFFFFFFFFFFFFULL, 2, 3, 1 },
        { "acc=0, mul=2*3=6 → no overflow",
          0, 2, 3, 0 },
        { "acc=1, mul=0xFF..F*0xFF..F → product_lo=1, overflow?",
          0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 1 },
        { "acc=0, mul=0xFF..F*0xFF..F → product_lo=1, no overflow",
          0, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0 },
};
static int nmac = sizeof(mac_cases) / sizeof(mac_cases[0]);

static void run_setcc(const char *name, tcase_t *cases, int n) {
        int pass = 0;
        printf("--- %s ---\n", name);
        for (int i = 0; i < n; i++) {
                uint64_t got = invoke(cases[i].rax, cases[i].rcx, cases[i].rdx);
                int ok = (got == cases[i].expect);
                pass += ok;
                printf("  %-50s  expect:%" PRIu64 "  got:%" PRIu64 "  %s\n",
                       cases[i].desc, cases[i].expect, got,
                       ok ? "PASS" : "** FAIL **");
        }
        printf("  Result: %d / %d\n\n", pass, n);
}

static void run_dump(const char *name) {
        printf("--- %s ---\n", name);
        /* Test with different RCX*RDX products to see if MUL flags vary */
        struct { uint64_t rcx, rdx; const char *desc; } mulcases[] = {
                { 0, 0,    "0 * 0 = 0" },
                { 1, 1,    "1 * 1 = 1" },
                { 2, 3,    "2 * 3 = 6" },
                { 0xFFFFFFFFFFFFFFFFULL, 2, "0xFF..F * 2" },
                { 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, "0xFF..F * 0xFF..F" },
        };
        for (int i = 0; i < 5; i++) {
                uint64_t got = invoke(0, mulcases[i].rcx, mulcases[i].rdx);
                printf("  %-30s  flags: 0x%016" PRIx64 "\n", mulcases[i].desc, got);
                printf("  %32s  CF=%lu PF=%lu ZF=%lu SF=%lu OF=%lu\n",
                       "",
                       (got >> 0) & 1,
                       (got >> 2) & 1,
                       (got >> 6) & 1,
                       (got >> 7) & 1,
                       (got >> 11) & 1);
        }
        printf("\n");
}


/* ══════════════════════════════════════════════════════════════════ */
int main(void) {
        printf("==========================================================\n");
        printf("  SETCC_CONDB After MUL — Diagnostic\n");
        printf("==========================================================\n\n");

        printf("Test 1: Baseline — ADD+SETCC same triad, no MUL\n");
        install_test1();
        run_setcc("Test 1: baseline (no MUL)", setcc_cases, nsetcc);

        printf("Test 2: NOP triad then ADD+SETCC (no MUL, cross-triad)\n");
        install_test2();
        run_setcc("Test 2: NOP gap, no MUL", setcc_cases, nsetcc);

        printf("Test 3: MUL in T1, ADD+SETCC in T2 (separate triads)\n");
        install_test3();
        run_setcc("Test 3: MUL then ADD+SETCC", setcc_cases, nsetcc);

        printf("Test 4: MUL, ADD, GENARITHFLAGS+SETCC (explicit flag regen)\n");
        install_test4();
        run_setcc("Test 4: MUL + GENARITHFLAGS + SETCC", setcc_cases, nsetcc);

        printf("Test 5: Exact MAC structure — ZEROEXT+MUL T0, ADD+SETCC T1\n");
        printf("  (ADD uses MUL product_lo from RCX)\n");
        install_test5();
        run_setcc("Test 5: exact MAC layout", mac_cases, nmac);

        printf("Test 6: MAC structure with NOP gap between MUL and ADD\n");
        install_test6();
        run_setcc("Test 6: MAC with NOP gap", mac_cases, nmac);

        printf("Test 7: READAFLAGS after MUL — what flags does MUL set?\n");
        install_test7();
        run_dump("Test 7: MUL flags via READAFLAGS");

        printf("==========================================================\n");
        printf("  Interpretation\n");
        printf("==========================================================\n");
        printf("  Test 1-2 pass:    SETCC works without MUL\n");
        printf("  Test 3 fails:     MUL flag pollution (even with gap)\n");
        printf("  Test 3 passes:    MUL doesn't pollute, issue is elsewhere\n");
        printf("  Test 4 passes:    GENARITHFLAGS can clear MUL pollution\n");
        printf("  Test 5 fails:     Exact MAC structure confirms the bug\n");
        printf("  Test 6 passes:    NOP gap clears MUL flags\n");
        printf("  Test 7:           Raw MUL flag bits for analysis\n\n");

        return 0;
}
