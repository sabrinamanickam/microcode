/*
 * test_setcc_carry.c — Test intra-triad SETCC_CONDB after ADD
 *
 * ═══════════════════════════════════════════════════════════════════
 *  Goal: Can we capture the carry flag from ADD within fewer triads
 *  than the current 6-triad bit-manipulation chain?
 *
 *  Known: flags do NOT cross triad boundaries on Goldmont.
 *  Question: within a single triad, does SETCC_CONDB see flags
 *  from ADD or GENARITHFLAGS in an earlier slot?
 *
 *  We test 5 strategies + a READAFLAGS diagnostic:
 *
 *  A) ADD + SETCC_CONDB in same triad (slots 0,1)
 *  B) GENARITHFLAGS + SETCC_CONDB in same triad
 *  C) ADD + GENARITHFLAGS same triad, SETCC next triad (expected fail)
 *  D) READAFLAGS diagnostic — what bits come back?
 *  E) ADD + READAFLAGS in same triad (no GENARITHFLAGS)
 *
 *  Test values:
 *    RAX = a, RCX = b.  Hook computes a+b, captures carry → RAX.
 *
 *  Build:  make PROG=test_setcc_carry
 *  Run:    sudo taskset -c 0 ./test_setcc_carry_static
 * ═══════════════════════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"


/* ══════════════════════════════════════════════════════════════════
 *  Strategy A: ADD + SETCC_CONDB in same triad
 *
 *  T0:  slot0: ADD TMP0, RAX, RCX       [does ADD set implicit flags?]
 *       slot1: SETCC_CONDB TMP1, TMP0   [capture CF → 0 or 1]
 *       slot2: NOP
 *
 *  T1:  slot0: ZEROEXT RAX, TMP1        [return carry in RAX]
 *       slot1: NOP
 *       slot2: END
 * ══════════════════════════════════════════════════════════════════ */
static void install_hook_a(void) {
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
 *  Strategy B: GENARITHFLAGS + SETCC_CONDB in same triad
 *
 *  T0:  ADD TMP0, RAX, RCX
 *
 *  T1:  slot0: GENARITHFLAGS_RR TMP0, RAX   [generate flags from
 *              (result=TMP0, operand=RAX) — reconstructs CF]
 *       slot1: SETCC_CONDB TMP1, TMP0       [capture CF]
 *       slot2: NOP
 *
 *  T2:  ZEROEXT RAX, TMP1 | END
 * ══════════════════════════════════════════════════════════════════ */
static void install_hook_b(void) {
        ucode_t patch[] = {
                {
                        ADD_DSZ64_DRR(TMP0, RAX, RCX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        GENARITHFLAGS_RR(TMP0, RAX),
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
 *  Strategy C: ADD + GENARITHFLAGS same triad, SETCC next triad
 *  (cross-triad baseline — expected to FAIL)
 *
 *  T0:  slot0: ADD TMP0, RAX, RCX
 *       slot1: GENARITHFLAGS_RR TMP0, RAX
 *       slot2: NOP
 *
 *  T1:  slot0: SETCC_CONDB TMP1, TMP0
 *       NOP | NOP
 *
 *  T2:  ZEROEXT RAX, TMP1 | END
 * ══════════════════════════════════════════════════════════════════ */
static void install_hook_c(void) {
        ucode_t patch[] = {
                {
                        ADD_DSZ64_DRR(TMP0, RAX, RCX),
                        GENARITHFLAGS_RR(TMP0, RAX),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        SETCC_CONDB_DR(TMP1, TMP0),
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
 *  Strategy D: READAFLAGS diagnostic (with GENARITHFLAGS)
 *
 *  Same structure as B but uses READAFLAGS instead of SETCC
 *  to see the raw flags word.
 *
 *  T0:  ADD TMP0, RAX, RCX
 *  T1:  GENARITHFLAGS_RR TMP0, RAX | READAFLAGS_DR TMP1, TMP0
 *  T2:  ZEROEXT RAX, TMP1 | END
 * ══════════════════════════════════════════════════════════════════ */
static void install_hook_d(void) {
        ucode_t patch[] = {
                {
                        ADD_DSZ64_DRR(TMP0, RAX, RCX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        GENARITHFLAGS_RR(TMP0, RAX),
                        READAFLAGS_DR(TMP1, TMP0),
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
 *  Strategy E: ADD + READAFLAGS in same triad (no GENARITHFLAGS)
 *
 *  T0:  slot0: ADD TMP0, RAX, RCX
 *       slot1: READAFLAGS_DR TMP1, TMP0
 *       slot2: NOP
 *
 *  T1:  ZEROEXT RAX, TMP1 | END
 * ══════════════════════════════════════════════════════════════════ */
static void install_hook_e(void) {
        ucode_t patch[] = {
                {
                        ADD_DSZ64_DRR(TMP0, RAX, RCX),
                        READAFLAGS_DR(TMP1, TMP0),
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
 *  TEST INFRASTRUCTURE
 * ══════════════════════════════════════════════════════════════════ */
static uint64_t invoke_hook(uint64_t a, uint64_t c) {
        uint64_t result;
        asm volatile(
                "mov rax, %[a_val]\n\t"
                "mov rcx, %[c_val]\n\t"
                "xor rdx, rdx\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[out], rax\n\t"
                : [out] "=r"(result)
                : [a_val] "r"(a), [c_val] "r"(c)
                : "rax", "rcx", "rdx", "r8"
        );
        return result;
}

typedef struct {
        uint64_t a;
        uint64_t c;
        uint64_t expect_carry;
        const char *desc;
} test_case_t;

static test_case_t cases[] = {
        { 0,                         0, 0, "0 + 0 -> CF=0" },
        { 1,                         0, 0, "1 + 0 -> CF=0" },
        { 0x7FFFFFFFFFFFFFFFULL,     1, 0, "0x7F..F + 1 -> CF=0" },
        { 0xFFFFFFFFFFFFFFFFULL,     1, 1, "0xFF..F + 1 -> CF=1" },
        { 0xFFFFFFFFFFFFFFFFULL,     2, 1, "0xFF..F + 2 -> CF=1" },
        { 0x8000000000000000ULL,     0x8000000000000000ULL, 1,
                                        "0x80..0 + 0x80..0 -> CF=1" },
};
static int ncases = sizeof(cases) / sizeof(cases[0]);

/* For strategies A/B/C: result should be 0 or 1 */
static void run_setcc_tests(const char *name) {
        int pass = 0;
        printf("--- %s ---\n\n", name);

        for (int i = 0; i < ncases; i++) {
                uint64_t got = invoke_hook(cases[i].a, cases[i].c);
                int ok = (got == cases[i].expect_carry);
                pass += ok;
                printf("  %-35s  expect:%lu  got:%lu  %s\n",
                       cases[i].desc, cases[i].expect_carry, got,
                       ok ? "PASS" : "WRONG");
        }

        printf("\n  Result: %d / %d  ", pass, ncases);
        if (pass == ncases)
                printf(">>> FLAG FORWARDING WORKS <<<");
        else if (pass == 3)
                printf(">>> always returns 0 (flags not visible) <<<");
        else
                printf(">>> partial/unexpected — needs investigation <<<");
        printf("\n\n");
}

/* For strategies D/E: dump raw flag word */
static void run_readaflags_tests(const char *name) {
        printf("--- %s ---\n\n", name);
        printf("  %-35s  raw flags value\n", "test case");

        for (int i = 0; i < ncases; i++) {
                uint64_t got = invoke_hook(cases[i].a, cases[i].c);
                printf("  %-35s  0x%016" PRIx64 "\n", cases[i].desc, got);

                /* Decode assuming x86-style EFLAGS layout */
                printf("  %37s CF=%lu PF=%lu ZF=%lu SF=%lu OF=%lu\n",
                       "",
                       (got >> 0) & 1,   /* CF = bit 0 */
                       (got >> 2) & 1,   /* PF = bit 2 */
                       (got >> 6) & 1,   /* ZF = bit 6 */
                       (got >> 7) & 1,   /* SF = bit 7 */
                       (got >> 11) & 1); /* OF = bit 11 */
        }
        printf("\n");
}


/* ══════════════════════════════════════════════════════════════════ */
int main(void) {
        printf("==========================================================\n");
        printf("  Intra-Triad SETCC_CONDB / READAFLAGS Carry Flag Test\n");
        printf("==========================================================\n\n");

        /* -- A: ADD + SETCC same triad ----------------------------- */
        printf("Strategy A: ADD + SETCC_CONDB in same triad (slots 0,1)\n");
        printf("  Tests if ADD implicitly exposes CF to SETCC\n\n");
        install_hook_a();
        run_setcc_tests("A: ADD + SETCC_CONDB same triad");

        printf("==========================================================\n\n");

        /* -- B: GENARITHFLAGS + SETCC same triad ------------------- */
        printf("Strategy B: ADD then GENARITHFLAGS + SETCC_CONDB same triad\n");
        printf("  Explicit flag gen from (result, operand), SETCC reads\n\n");
        install_hook_b();
        run_setcc_tests("B: GENARITHFLAGS + SETCC_CONDB same triad");

        printf("==========================================================\n\n");

        /* -- C: cross-triad baseline ------------------------------- */
        printf("Strategy C: GENARITHFLAGS in T0, SETCC in T1 (cross-triad)\n");
        printf("  Expected to fail - confirms flag isolation\n\n");
        install_hook_c();
        run_setcc_tests("C: cross-triad (expected fail)");

        printf("==========================================================\n\n");

        /* -- D: READAFLAGS diagnostic (with GENARITHFLAGS) --------- */
        printf("Strategy D: GENARITHFLAGS + READAFLAGS same triad\n");
        printf("  Dumps raw flag bits to see what GENARITHFLAGS produces\n\n");
        install_hook_d();
        run_readaflags_tests("D: GENARITHFLAGS + READAFLAGS same triad");

        printf("==========================================================\n\n");

        /* -- E: ADD + READAFLAGS same triad (no GENARITHFLAGS) ----- */
        printf("Strategy E: ADD + READAFLAGS same triad (no GENARITHFLAGS)\n");
        printf("  Tests if ADD alone sets readable flags\n\n");
        install_hook_e();
        run_readaflags_tests("E: ADD + READAFLAGS same triad");

        printf("==========================================================\n");
        printf("  Interpretation Guide\n");
        printf("==========================================================\n");
        printf("  A works -> 3-triad MAC (ADD + SETCC + fold)\n");
        printf("  B works -> 4-triad MAC (ADD, GENFLAGS+SETCC, fold, done)\n");
        printf("  D shows bits -> decode to understand flag layout\n");
        printf("  E shows bits -> ADD alone sets flags without GENARITHFLAGS\n");
        printf("  Nothing works -> stuck at 6-triad carry chain\n\n");

        return 0;
}
