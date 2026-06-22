/*
 * ujmp_test.c — Verify microcode UJMP_I (unconditional) and UJMPCC
 *               (conditional) branch micro-ops actually work.
 *
 * Encoding (from include/inst.h + tools/main/main.c usage):
 *   UJMP_I(target_uaddr)                          - unconditional jump
 *   UJMPCC_DIRECT_NOTTAKEN_COND<cc>_RI(reg, addr) - branch to addr if
 *                                                   reg satisfies condition
 *
 * Ordered safest → most ambitious. Each test installs its own patch.
 * Test B (forward conditional branch) is run BEFORE Test D (loop) so
 * we know whether the branch direction works before risking an infinite
 * loop in the loop test.
 *
 *   A. UJMP_I forward jump:            unconditional, simple sanity.
 *   B. UJMPCC CONDNZ taken forward:    TMP0=1, branch should fire.
 *   C. UJMPCC CONDNZ NOT taken:        TMP0=0, branch should fall through.
 *   D. UJMPCC backward count-down loop: SUB + UJMPCC in SEPARATE triads
 *                                       (was originally combined; that
 *                                        hung the NUC, presumably an
 *                                        intra-triad slot0→slot1 RAW
 *                                        hazard).
 *
 * Build:  make PROG=ujmp_test
 * Run:    sudo taskset -c 0 ./ujmp_test_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define REGION  0x7c00
#define T(n) (REGION + (n) * 4)

static uint64_t fire_patch(void) {
    uint64_t res;
    asm volatile(
        "xor eax, eax\n\t"
        "vmwrite rcx, rdx\n\t"
        : "=a"(res)
        :
        : "rcx", "rdx", "memory", "cc"
    );
    return res;
}

static void reinstall(ucode_t *p, int n) {
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(REGION, p, n);
    hook_match_and_patch(0, 0x0cd8, REGION);
}

/* ── A. UJMP_I forward jump ──────────────────────────────────────
 *   T0: TMP0 = 0xAAAA
 *   T1: UJMP_I(T3)               -- skip T2
 *   T2: TMP0 = 0xBBBB (POISON)
 *   T3: RAX = TMP0; END          -- expect 0xAAAA
 */
static int test_a(void) {
    printf("--- A: UJMP_I forward jump ---\n");
    ucode_t p[] = {
        { MOVE_DSZ64_DI(TMP0, 0xAAAA), NOP, NOP, NOP_SEQWORD },
        { UJMP_I(T(3)), NOP, NOP, NOP_SEQWORD },
        { MOVE_DSZ64_DI(TMP0, 0xBBBB), NOP, NOP, NOP_SEQWORD },          /* poison */
        { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0xAAAA);
    printf("  RAX = 0x%" PRIx64 "  expect 0xAAAA  %s\n", r, ok ? "PASS" : "FAIL");
    return ok;
}

/* ── B. UJMPCC CONDNZ taken forward (safe, no loop) ─────────────
 *   T0: TMP0 = 1
 *   T1: UJMPCC CONDNZ(TMP0, T3) — should branch (TMP0 != 0)
 *   T2: RAX = 0xDEAD; END        (poison — should NOT run)
 *   T3: RAX = 0xBEEF; END        (expected path)
 *
 * Note: UJMPCC alone in its own triad here, no intra-triad write of
 * TMP0 to worry about. Cleanest possible test of the conditional.
 */
static int test_b(void) {
    printf("--- B: UJMPCC CONDNZ branch-taken (forward) ---\n");
    ucode_t p[] = {
        { MOVE_DSZ64_DI(TMP0, 1), NOP, NOP, NOP_SEQWORD },
        { UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP0, T(3)),
          NOP, NOP, NOP_SEQWORD },
        { MOVE_DSZ64_DI(RAX, 0xDEAD), NOP, NOP, END_SEQWORD },           /* poison */
        { MOVE_DSZ64_DI(RAX, 0xBEEF), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0xBEEF);
    printf("  RAX = 0x%" PRIx64 "  expect 0xBEEF  %s%s\n",
           r, ok ? "PASS" : "FAIL",
           (!ok && r == 0xDEAD) ? "  (branch NOT taken when it should have been)" : "");
    return ok;
}

/* ── C. UJMPCC CONDNZ NOT taken (fall through) ──────────────────
 *   T0: TMP0 = 0
 *   T1: UJMPCC CONDNZ(TMP0, T3) — should NOT branch (TMP0 == 0)
 *   T2: RAX = 0xABCD; END        (expected path)
 *   T3: RAX = 0xDEAD; END        (poison)
 */
static int test_c(void) {
    printf("--- C: UJMPCC CONDNZ fall-through ---\n");
    ucode_t p[] = {
        { MOVE_DSZ64_DI(TMP0, 0), NOP, NOP, NOP_SEQWORD },
        { UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP0, T(3)),
          NOP, NOP, NOP_SEQWORD },
        { MOVE_DSZ64_DI(RAX, 0xABCD), NOP, NOP, END_SEQWORD },
        { MOVE_DSZ64_DI(RAX, 0xDEAD), NOP, NOP, END_SEQWORD },           /* poison */
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0xABCD);
    printf("  RAX = 0x%" PRIx64 "  expect 0xABCD  %s%s\n",
           r, ok ? "PASS" : "FAIL",
           (!ok && r == 0xDEAD) ? "  (branch WAS taken when CONDNZ was false)" : "");
    return ok;
}

/* ── D. UJMPCC chain of forward branches (UNROLLED loop, no backward jumps)
 *
 * Replaces the backward-loop test, which crashed the NUC. Same primitive
 * (UJMPCC CONDNZ with SUB-then-test pattern), but every branch goes
 * FORWARD only — execution length is bounded by patch length, so an
 * infinite loop is structurally impossible.
 *
 * Three "iterations" of decrement + branch-forward. After SUB:
 *   iter 1: TMP0 = 2 → CONDNZ true  → branch forward past poison 1
 *   iter 2: TMP0 = 1 → CONDNZ true  → branch forward past poison 2
 *   iter 3: TMP0 = 0 → CONDNZ false → FALL THROUGH to exit
 *
 * Triad layout (11 triads):
 *   T0:                TMP0 = 3
 *   T1:                TMP0 -= 1                              (TMP0=2)
 *   T2:                UJMPCC CONDNZ_RI(TMP0, T4)
 *   T3 (poison 1):     RAX = 0xBAD1; END
 *   T4:                TMP0 -= 1                              (TMP0=1)
 *   T5:                UJMPCC CONDNZ_RI(TMP0, T7)
 *   T6 (poison 2):     RAX = 0xBAD2; END
 *   T7:                TMP0 -= 1                              (TMP0=0)
 *   T8:                UJMPCC CONDNZ_RI(TMP0, T10)            (should NOT branch)
 *   T9 (expected exit): RAX = TMP0; END                       (RAX=0)
 *   T10 (poison 3):    RAX = 0xBAD3; END
 *
 * Expected: RAX = 0 (PASS).
 * Failure decode:
 *   0xBAD1 → iter 1's CONDNZ false-when-should-be-true (didn't branch)
 *   0xBAD2 → iter 2's CONDNZ same
 *   0xBAD3 → iter 3 wrongly branched even though TMP0 was 0
 *   nonzero/other → SUB didn't decrement correctly
 *
 * Patch length: 11 triads, all forward. Worst case: every UJMPCC fails,
 * we fall through linearly, hit some END, and return. Bounded.
 */
static int test_d(void) {
    printf("--- D: UJMPCC unrolled forward-branch chain (3 iters, safe) ---\n");
    ucode_t p[] = {
        /* T0  */ { MOVE_DSZ64_DI(TMP0, 3),       NOP, NOP, NOP_SEQWORD },
        /* T1  */ { SUB_DSZ64_DRI(TMP0, TMP0, 1), NOP, NOP, NOP_SEQWORD },
        /* T2  */ { UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP0, T(4)),
                                                  NOP, NOP, NOP_SEQWORD },
        /* T3  */ { MOVE_DSZ64_DI(RAX, 0xBAD1),   NOP, NOP, END_SEQWORD }, /* poison 1 */
        /* T4  */ { SUB_DSZ64_DRI(TMP0, TMP0, 1), NOP, NOP, NOP_SEQWORD },
        /* T5  */ { UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP0, T(7)),
                                                  NOP, NOP, NOP_SEQWORD },
        /* T6  */ { MOVE_DSZ64_DI(RAX, 0xBAD2),   NOP, NOP, END_SEQWORD }, /* poison 2 */
        /* T7  */ { SUB_DSZ64_DRI(TMP0, TMP0, 1), NOP, NOP, NOP_SEQWORD },
        /* T8  */ { UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP0, T(10)),
                                                  NOP, NOP, NOP_SEQWORD },
        /* T9  */ { ZEROEXT_DSZ64_DR(RAX, TMP0),  NOP, NOP, END_SEQWORD }, /* expected */
        /* T10 */ { MOVE_DSZ64_DI(RAX, 0xBAD3),   NOP, NOP, END_SEQWORD }, /* poison 3 */
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0);
    printf("  RAX = 0x%" PRIx64 "  expect 0x0  %s\n", r, ok ? "PASS" : "FAIL");
    if (!ok) {
        const char *reason = "";
        if      (r == 0xBAD1) reason = "  iter 1 fall-through (CONDNZ didn't branch at TMP0=2)";
        else if (r == 0xBAD2) reason = "  iter 2 fall-through (CONDNZ didn't branch at TMP0=1)";
        else if (r == 0xBAD3) reason = "  iter 3 branched (CONDNZ branched at TMP0=0 — should fall through)";
        else                  reason = "  unexpected — SUB or register flow may be wrong";
        printf("  →%s\n", reason);
    }
    return ok;
}

/* ── E. UJMPCC CONDZ branch-taken (TMP0=0) ──────────────────────
 *
 * Diagnostic: tools/main/main.c uses CONDZ_RI in production. If CONDNZ
 * is broken but CONDZ works, we have an actionable answer.
 *
 *   T0: TMP0 = 0
 *   T1: UJMPCC CONDZ_RI(TMP0, T3) — should branch (TMP0 == 0)
 *   T2: RAX = 0xDEAD; END (poison)
 *   T3: RAX = 0xBEEF; END (expected)
 */
static int test_e(void) {
    printf("--- E: UJMPCC CONDZ branch-taken (TMP0=0) ---\n");
    ucode_t p[] = {
        { MOVE_DSZ64_DI(TMP0, 0), NOP, NOP, NOP_SEQWORD },
        { UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(TMP0, T(3)),
          NOP, NOP, NOP_SEQWORD },
        { MOVE_DSZ64_DI(RAX, 0xDEAD), NOP, NOP, END_SEQWORD },           /* poison */
        { MOVE_DSZ64_DI(RAX, 0xBEEF), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0xBEEF);
    printf("  RAX = 0x%" PRIx64 "  expect 0xBEEF  %s%s\n",
           r, ok ? "PASS" : "FAIL",
           (!ok && r == 0xDEAD) ? "  (branch NOT taken even though TMP0 == 0)" : "");
    return ok;
}

/* ── F. UJMPCC CONDZ fall-through (TMP0=5) ──────────────────────
 *
 * Pair with test E: if E branches (good) and F also branches (bad),
 * then CONDZ is also unconditional. If E branches and F falls through,
 * CONDZ works correctly and we can use it (inverting condition logic
 * vs CONDNZ).
 *
 *   T0: TMP0 = 5
 *   T1: UJMPCC CONDZ_RI(TMP0, T3) — should NOT branch (TMP0 != 0)
 *   T2: RAX = 0xABCD; END (expected)
 *   T3: RAX = 0xDEAD; END (poison)
 */
static int test_f(void) {
    printf("--- F: UJMPCC CONDZ fall-through (TMP0=5) ---\n");
    ucode_t p[] = {
        { MOVE_DSZ64_DI(TMP0, 5), NOP, NOP, NOP_SEQWORD },
        { UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(TMP0, T(3)),
          NOP, NOP, NOP_SEQWORD },
        { MOVE_DSZ64_DI(RAX, 0xABCD), NOP, NOP, END_SEQWORD },
        { MOVE_DSZ64_DI(RAX, 0xDEAD), NOP, NOP, END_SEQWORD },           /* poison */
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0xABCD);
    printf("  RAX = 0x%" PRIx64 "  expect 0xABCD  %s%s\n",
           r, ok ? "PASS" : "FAIL",
           (!ok && r == 0xDEAD) ? "  (branch TAKEN when TMP0 != 0 — CONDZ also broken)" : "");
    return ok;
}

/* ── G. main.c-style: XOR (result=0) → CONDZ should branch ──────
 *
 * Replicates the pattern from tools/main/main.c:
 *   T_n: { XOR_DSZ64_DRR(reg, a, b),         // sets RFLAGS if it works
 *          ...,
 *          UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(reg, target),  // reads RFLAGS
 *          ... }
 *
 * If XOR-of-TMP-regs updates arch RFLAGS visible to UJMPCC in the same
 * triad, then CONDZ with XOR-result-zero should branch (ZF=1).
 *
 * Setup so TMP0 ^ TMP1 = 0 (i.e., TMP0 == TMP1).
 *
 *   T0: TMP0 = 5
 *   T1: TMP1 = 5
 *   T2: XOR TMP0 = TMP0 ^ TMP1   (slot 0, result=0, should set ZF=1)
 *       NOP                       (slot 1)
 *       UJMPCC CONDZ_RI(TMP0, T4) (slot 2, should branch on ZF=1)
 *   T3: RAX = 0xDEAD; END (poison)
 *   T4: RAX = 0xBEEF; END (expected)
 */
static int test_g(void) {
    printf("--- G: intra-triad XOR(0) → CONDZ branch-taken ---\n");
    ucode_t p[] = {
        { MOVE_DSZ64_DI(TMP0, 5),   NOP, NOP, NOP_SEQWORD },
        { MOVE_DSZ64_DI(TMP1, 5),   NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(TMP0, TMP0, TMP1),
          NOP,
          UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(TMP0, T(4)),
          NOP_SEQWORD },
        { MOVE_DSZ64_DI(RAX, 0xDEAD), NOP, NOP, END_SEQWORD },           /* poison */
        { MOVE_DSZ64_DI(RAX, 0xBEEF), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0xBEEF);
    printf("  RAX = 0x%" PRIx64 "  expect 0xBEEF  %s%s\n",
           r, ok ? "PASS" : "FAIL",
           (!ok && r == 0xDEAD) ? "  (XOR didn't bridge ZF to RFLAGS — or intra-triad flag flow broken)" : "");
    return ok;
}

/* ── H. main.c-style: XOR (result!=0) → CONDZ should NOT branch ──
 *
 * Pair with G. TMP0 ^ TMP1 = 6 (nonzero, ZF should be 0).
 *
 *   T0: TMP0 = 5
 *   T1: TMP1 = 3
 *   T2: XOR TMP0 = TMP0 ^ TMP1   (slot 0, result=6, should set ZF=0)
 *       NOP                       (slot 1)
 *       UJMPCC CONDZ_RI(TMP0, T4) (slot 2, should NOT branch on ZF=0)
 *   T3: RAX = 0xABCD; END (expected — fall through)
 *   T4: RAX = 0xDEAD; END (poison — only if wrongly branched)
 */
static int test_h(void) {
    printf("--- H: intra-triad XOR(non-0) → CONDZ fall-through ---\n");
    ucode_t p[] = {
        { MOVE_DSZ64_DI(TMP0, 5),   NOP, NOP, NOP_SEQWORD },
        { MOVE_DSZ64_DI(TMP1, 3),   NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(TMP0, TMP0, TMP1),
          NOP,
          UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(TMP0, T(4)),
          NOP_SEQWORD },
        { MOVE_DSZ64_DI(RAX, 0xABCD), NOP, NOP, END_SEQWORD },
        { MOVE_DSZ64_DI(RAX, 0xDEAD), NOP, NOP, END_SEQWORD },           /* poison */
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0xABCD);
    printf("  RAX = 0x%" PRIx64 "  expect 0xABCD  %s%s\n",
           r, ok ? "PASS" : "FAIL",
           (!ok && r == 0xDEAD) ? "  (UJMPCC branched on nonzero XOR result — likely still RFLAGS-frozen)" : "");
    return ok;
}

/* ── I. SUB → XOR → UJMPCC unrolled chain (the real loop pattern, safe)
 *
 * Combines the lessons from D, G, H. SUB decrements (TMP-flag-domain only),
 * XOR-with-zero bridges to arch RFLAGS, UJMPCC reads it. Each "loop
 * iteration" is one triad:
 *
 *   { SUB counter -= 1,                  // slot 0
 *     XOR scratch = counter ^ zero_reg,  // slot 1: sets RFLAGS
 *     UJMPCC CONDNZ_RI(scratch, target), // slot 2: branch if counter != 0
 *     NOP_SEQWORD }
 *
 * Unrolled 3 times here, all branches forward. If it works, the same
 * triad with target=self (backward branch) would be a real loop.
 *
 *   T0:                TMP0 = 3 (counter)
 *   T1:                TMP1 = 0 (constant zero for XOR test)
 *   T2 (iter 1):       SUB | XOR | UJMPCC CONDNZ → T4   (TMP0=2 → branch)
 *   T3 (poison 1):     RAX = 0xBAD1; END
 *   T4 (iter 2):       SUB | XOR | UJMPCC CONDNZ → T6   (TMP0=1 → branch)
 *   T5 (poison 2):     RAX = 0xBAD2; END
 *   T6 (iter 3):       SUB | XOR | UJMPCC CONDNZ → T8   (TMP0=0 → fall through)
 *   T7 (expected):     RAX = TMP0; END   (RAX = 0)
 *   T8 (poison 3):     RAX = 0xBAD3; END
 *
 * Pass: RAX = 0.
 * Failure decode mirrors test D's.
 */
static int test_i(void) {
    printf("--- I: SUB+XOR+UJMPCC unrolled chain (the loop primitive, safe) ---\n");
    ucode_t p[] = {
        /* T0 */ { MOVE_DSZ64_DI(TMP0, 3),     NOP, NOP, NOP_SEQWORD },
        /* T1 */ { MOVE_DSZ64_DI(TMP1, 0),     NOP, NOP, NOP_SEQWORD },
        /* T2 */ { SUB_DSZ64_DRI(TMP0, TMP0, 1),
                   XOR_DSZ64_DRR(TMP2, TMP0, TMP1),
                   UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP2, T(4)),
                   NOP_SEQWORD },
        /* T3 */ { MOVE_DSZ64_DI(RAX, 0xBAD1), NOP, NOP, END_SEQWORD },
        /* T4 */ { SUB_DSZ64_DRI(TMP0, TMP0, 1),
                   XOR_DSZ64_DRR(TMP2, TMP0, TMP1),
                   UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP2, T(6)),
                   NOP_SEQWORD },
        /* T5 */ { MOVE_DSZ64_DI(RAX, 0xBAD2), NOP, NOP, END_SEQWORD },
        /* T6 */ { SUB_DSZ64_DRI(TMP0, TMP0, 1),
                   XOR_DSZ64_DRR(TMP2, TMP0, TMP1),
                   UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP2, T(8)),
                   NOP_SEQWORD },
        /* T7 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD }, /* expected */
        /* T8 */ { MOVE_DSZ64_DI(RAX, 0xBAD3), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0);
    printf("  RAX = 0x%" PRIx64 "  expect 0x0  %s\n", r, ok ? "PASS" : "FAIL");
    if (!ok) {
        const char *reason = "";
        if      (r == 0xBAD1) reason = "  iter 1 fall-through (CONDNZ didn't branch at TMP0=2)";
        else if (r == 0xBAD2) reason = "  iter 2 fall-through (CONDNZ didn't branch at TMP0=1)";
        else if (r == 0xBAD3) reason = "  iter 3 branched (CONDNZ branched at TMP0=0)";
        else                  reason = "  unexpected — SUB or XOR may not be doing what we think";
        printf("  →%s\n", reason);
    }
    return ok;
}

/* ── J. XOR in slot 1, UJMPCC in slot 2 — slot-1→slot-2 flag flow
 *
 * G/H proved slot 0→2 flag flow works (XOR in slot 0, UJMPCC in slot 2).
 * Test I needed XOR in slot 1 (after SUB in slot 0). If slot 1→slot 2
 * flag flow ALSO works, then I's failure is purely the stale-SUB issue.
 * If slot 1→slot 2 doesn't work, that's a separate constraint.
 *
 *   T0: TMP0 = 5, TMP1 = 5            (so XOR result will be 0)
 *   T2: NOP / XOR / UJMPCC CONDZ → T4
 *
 * Expected if slot 1→2 flag flow works: branch taken, RAX = 0xBEEF.
 * Expected if RFLAGS stays at patch-entry value: no branch, RAX = 0xDEAD.
 */
static int test_j(void) {
    printf("--- J: XOR in slot 1 → UJMPCC slot 2 — slot1→2 flag flow ---\n");
    ucode_t p[] = {
        { MOVE_DSZ64_DI(TMP0, 5), NOP, NOP, NOP_SEQWORD },
        { MOVE_DSZ64_DI(TMP1, 5), NOP, NOP, NOP_SEQWORD },
        { NOP,
          XOR_DSZ64_DRR(TMP2, TMP0, TMP1),      /* slot 1, result = 0, ZF=1 */
          UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(TMP2, T(4)),
          NOP_SEQWORD },
        { MOVE_DSZ64_DI(RAX, 0xDEAD), NOP, NOP, END_SEQWORD },         /* poison */
        { MOVE_DSZ64_DI(RAX, 0xBEEF), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0xBEEF);
    printf("  RAX = 0x%" PRIx64 "  expect 0xBEEF  %s%s\n",
           r, ok ? "PASS" : "FAIL",
           (!ok && r == 0xDEAD) ? "  (slot 1→2 flag flow doesn't work — XOR must live in slot 0)" : "");
    return ok;
}

/* ── K. Two-triad iteration: SUB in T_n, XOR+UJMPCC in T_n+1
 *
 * Fixes Test I. SUB and XOR are in DIFFERENT triads, so cross-triad
 * RAW guarantees the SUB result is visible to the XOR.
 *
 * 3 unrolled iterations, all forward branches (safe).
 *
 *   T0:                TMP0 = 3
 *   T1:                TMP1 = 0
 *   T2 (iter 1 dec):   TMP0 -= 1                   (→ 2)
 *   T3 (iter 1 test):  XOR TMP2 = TMP0 ^ TMP1; UJMPCC CONDNZ → T5
 *   T4 (poison 1)
 *   T5 (iter 2 dec):   TMP0 -= 1                   (→ 1)
 *   T6 (iter 2 test):  XOR + UJMPCC → T8
 *   T7 (poison 2)
 *   T8 (iter 3 dec):   TMP0 -= 1                   (→ 0)
 *   T9 (iter 3 test):  XOR (result 0, ZF=1) + UJMPCC CONDNZ → T11
 *                      (should NOT branch — CONDNZ sees ZF=1)
 *   T10 (expected):    RAX = TMP0; END
 *   T11 (poison 3)
 *
 * If K passes but I failed, intra-triad SUB→XOR has the stale-RAW issue.
 * If K also fails, something more fundamental is wrong.
 */
static int test_k(void) {
    printf("--- K: Two-triad iteration (SUB ⊕ XOR+UJMPCC) ---\n");
    ucode_t p[] = {
        /* T0  */ { MOVE_DSZ64_DI(TMP0, 3),                    NOP, NOP, NOP_SEQWORD },
        /* T1  */ { MOVE_DSZ64_DI(TMP1, 0),                    NOP, NOP, NOP_SEQWORD },
        /* T2  */ { SUB_DSZ64_DRI(TMP0, TMP0, 1),              NOP, NOP, NOP_SEQWORD },
        /* T3  */ { XOR_DSZ64_DRR(TMP2, TMP0, TMP1),
                    NOP,
                    UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP2, T(5)),
                    NOP_SEQWORD },
        /* T4  */ { MOVE_DSZ64_DI(RAX, 0xBAD1), NOP, NOP, END_SEQWORD },
        /* T5  */ { SUB_DSZ64_DRI(TMP0, TMP0, 1),              NOP, NOP, NOP_SEQWORD },
        /* T6  */ { XOR_DSZ64_DRR(TMP2, TMP0, TMP1),
                    NOP,
                    UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP2, T(8)),
                    NOP_SEQWORD },
        /* T7  */ { MOVE_DSZ64_DI(RAX, 0xBAD2), NOP, NOP, END_SEQWORD },
        /* T8  */ { SUB_DSZ64_DRI(TMP0, TMP0, 1),              NOP, NOP, NOP_SEQWORD },
        /* T9  */ { XOR_DSZ64_DRR(TMP2, TMP0, TMP1),
                    NOP,
                    UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP2, T(11)),
                    NOP_SEQWORD },
        /* T10 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD }, /* expected */
        /* T11 */ { MOVE_DSZ64_DI(RAX, 0xBAD3), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0);
    printf("  RAX = 0x%" PRIx64 "  expect 0x0  %s\n", r, ok ? "PASS" : "FAIL");
    if (!ok) {
        const char *reason = "";
        if      (r == 0xBAD1) reason = "  iter 1 fell through (CONDNZ didn't branch at TMP0=2)";
        else if (r == 0xBAD2) reason = "  iter 2 fell through (CONDNZ didn't branch at TMP0=1)";
        else if (r == 0xBAD3) reason = "  iter 3 branched (CONDNZ branched at TMP0=0)";
        else                  reason = "  unexpected — TMP1 may have been clobbered between iters";
        printf("  →%s\n", reason);
    }
    return ok;
}

/* ── L. Direct SUB_DSZ64_DRI sanity check (no UJMPCC, no XOR)
 *
 *   T0: TMP0 = 5
 *   T1: SUB TMP0 -= 1
 *   T2: RAX = TMP0; END
 *
 * Expected: RAX = 4. If RAX = 5, SUB is a no-op on TMP regs.
 */
static int test_l(void) {
    printf("--- L: SUB_DSZ64_DRI(TMP, TMP, 1) sanity check ---\n");
    ucode_t p[] = {
        { MOVE_DSZ64_DI(TMP0, 5),               NOP, NOP, NOP_SEQWORD },
        { SUB_DSZ64_DRI(TMP0, TMP0, 1),         NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP0),          NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 4);
    printf("  RAX = 0x%" PRIx64 "  expect 0x4  %s\n", r, ok ? "PASS" : "FAIL");
    if (!ok) {
        if      (r == 5) printf("  → SUB_DSZ64_DRI is a NO-OP on TMP registers.\n");
        else if (r == 6) printf("  → SUB acted like ADD (encoding may be reversed).\n");
        else             printf("  → unexpected — SUB has nonstandard semantics here.\n");
    }
    return ok;
}

/* ── M. Direct ADD_DSZ64_DRI sanity check */
static int test_m(void) {
    printf("--- M: ADD_DSZ64_DRI(TMP, TMP, 1) sanity check ---\n");
    ucode_t p[] = {
        { MOVE_DSZ64_DI(TMP0, 5),               NOP, NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRI(TMP0, TMP0, 1),         NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP0),          NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 6);
    printf("  RAX = 0x%" PRIx64 "  expect 0x6  %s\n", r, ok ? "PASS" : "FAIL");
    if (!ok) {
        if      (r == 5) printf("  → ADD_DSZ64_DRI is a NO-OP on TMP registers.\n");
        else             printf("  → unexpected ADD semantics.\n");
    }
    return ok;
}

/* ── N. ADD_DSZ64_DRI with -1 — workaround for the broken SUB
 *
 *   T0: TMP0 = 5
 *   T1: ADD TMP0 += -1
 *   T2: RAX = TMP0; END
 *
 * Expected: RAX = 4. If yes, this is the canonical decrement primitive.
 */
static int test_n(void) {
    printf("--- N: ADD_DSZ64_DRI(TMP, TMP, -1) decrement workaround ---\n");
    ucode_t p[] = {
        { MOVE_DSZ64_DI(TMP0, 5),               NOP, NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRI(TMP0, TMP0, -1),        NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP0),          NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 4);
    printf("  RAX = 0x%" PRIx64 "  expect 0x4  %s\n", r, ok ? "PASS" : "FAIL");
    if (!ok) {
        printf("  → ADD with -1 doesn't decrement here. May need ADD_DSZ64_DRR with a precomputed -1 register.\n");
    }
    return ok;
}

/* ── O. The K loop primitive, but using ADD-1 instead of SUB
 *
 * Identical structure to Test K, but with ADD_DSZ64_DRI(TMP, TMP, -1)
 * replacing SUB. If N passes (decrement works) and the loop framework
 * is otherwise sound, this should produce RAX = 0.
 *
 *   T0:                TMP0 = 3
 *   T1:                TMP1 = 0
 *   T2 (iter 1 dec):   TMP0 += -1              (→ 2)
 *   T3 (iter 1 test):  XOR + UJMPCC CONDNZ → T5
 *   T4 (poison 1)
 *   T5 (iter 2 dec):   TMP0 += -1              (→ 1)
 *   T6 (iter 2 test):  XOR + UJMPCC CONDNZ → T8
 *   T7 (poison 2)
 *   T8 (iter 3 dec):   TMP0 += -1              (→ 0)
 *   T9 (iter 3 test):  XOR + UJMPCC CONDNZ → T11   (should fall through)
 *   T10 (expected):    RAX = TMP0; END
 *   T11 (poison 3)
 */
static int test_o(void) {
    printf("--- O: K-loop pattern using ADD(reg, reg, -1) ---\n");
    ucode_t p[] = {
        /* T0  */ { MOVE_DSZ64_DI(TMP0, 3),                    NOP, NOP, NOP_SEQWORD },
        /* T1  */ { MOVE_DSZ64_DI(TMP1, 0),                    NOP, NOP, NOP_SEQWORD },
        /* T2  */ { ADD_DSZ64_DRI(TMP0, TMP0, -1),             NOP, NOP, NOP_SEQWORD },
        /* T3  */ { XOR_DSZ64_DRR(TMP2, TMP0, TMP1),
                    NOP,
                    UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP2, T(5)),
                    NOP_SEQWORD },
        /* T4  */ { MOVE_DSZ64_DI(RAX, 0xBAD1), NOP, NOP, END_SEQWORD },
        /* T5  */ { ADD_DSZ64_DRI(TMP0, TMP0, -1),             NOP, NOP, NOP_SEQWORD },
        /* T6  */ { XOR_DSZ64_DRR(TMP2, TMP0, TMP1),
                    NOP,
                    UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP2, T(8)),
                    NOP_SEQWORD },
        /* T7  */ { MOVE_DSZ64_DI(RAX, 0xBAD2), NOP, NOP, END_SEQWORD },
        /* T8  */ { ADD_DSZ64_DRI(TMP0, TMP0, -1),             NOP, NOP, NOP_SEQWORD },
        /* T9  */ { XOR_DSZ64_DRR(TMP2, TMP0, TMP1),
                    NOP,
                    UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP2, T(11)),
                    NOP_SEQWORD },
        /* T10 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
        /* T11 */ { MOVE_DSZ64_DI(RAX, 0xBAD3), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 0);
    printf("  RAX = 0x%" PRIx64 "  expect 0x0  %s\n", r, ok ? "PASS" : "FAIL");
    if (!ok) {
        const char *reason = "";
        if      (r == 0xBAD1) reason = "  iter 1 fell through unexpectedly";
        else if (r == 0xBAD2) reason = "  iter 2 fell through unexpectedly";
        else if (r == 0xBAD3) reason = "  iter 3 still branched — ADD-(-1) decrement didn't reach zero";
        else                  reason = "  unexpected result";
        printf("  →%s\n", reason);
    }
    return ok;
}

/* ── P. Count-UP loop primitive (the actual workable pattern)
 *
 * Findings so far:
 *   - SUB_DSZ64_DRI(dst, src, imm) computes imm − src (reversed args)
 *   - ADD_DSZ64_DRI's immediate is 16-bit UNSIGNED (no sign extension)
 *   → can't directly decrement via an immediate
 *
 * Workaround: count UP from 0 to a threshold, exit on equality.
 *
 *   T0:                TMP0 = 0      (counter)
 *   T1:                TMP1 = 3      (threshold)
 *   T2 (iter 1 incr):  TMP0 += 1                       (→ 1)
 *   T3 (iter 1 test):  XOR TMP2 = TMP0 ^ TMP1; UJMPCC CONDNZ → T5
 *   T4 (poison 1)
 *   T5 (iter 2 incr):  TMP0 += 1                       (→ 2)
 *   T6 (iter 2 test):  XOR + UJMPCC CONDNZ → T8
 *   T7 (poison 2)
 *   T8 (iter 3 incr):  TMP0 += 1                       (→ 3)
 *   T9 (iter 3 test):  XOR (result 0, ZF=1) + UJMPCC CONDNZ → T11
 *                      (should NOT branch — counter == threshold)
 *   T10 (expected):    RAX = TMP0; END                 (RAX = 3)
 *   T11 (poison 3)
 *
 * Pass: RAX = 3. The loop counted up correctly and exited at the threshold.
 */
static int test_p(void) {
    printf("--- P: Count-UP loop primitive (ADD+XOR+UJMPCC) ---\n");
    ucode_t p[] = {
        /* T0  */ { MOVE_DSZ64_DI(TMP0, 0),                    NOP, NOP, NOP_SEQWORD },
        /* T1  */ { MOVE_DSZ64_DI(TMP1, 3),                    NOP, NOP, NOP_SEQWORD },
        /* T2  */ { ADD_DSZ64_DRI(TMP0, TMP0, 1),              NOP, NOP, NOP_SEQWORD },
        /* T3  */ { XOR_DSZ64_DRR(TMP2, TMP0, TMP1),
                    NOP,
                    UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP2, T(5)),
                    NOP_SEQWORD },
        /* T4  */ { MOVE_DSZ64_DI(RAX, 0xBAD1), NOP, NOP, END_SEQWORD },
        /* T5  */ { ADD_DSZ64_DRI(TMP0, TMP0, 1),              NOP, NOP, NOP_SEQWORD },
        /* T6  */ { XOR_DSZ64_DRR(TMP2, TMP0, TMP1),
                    NOP,
                    UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP2, T(8)),
                    NOP_SEQWORD },
        /* T7  */ { MOVE_DSZ64_DI(RAX, 0xBAD2), NOP, NOP, END_SEQWORD },
        /* T8  */ { ADD_DSZ64_DRI(TMP0, TMP0, 1),              NOP, NOP, NOP_SEQWORD },
        /* T9  */ { XOR_DSZ64_DRR(TMP2, TMP0, TMP1),
                    NOP,
                    UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP2, T(11)),
                    NOP_SEQWORD },
        /* T10 */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
        /* T11 */ { MOVE_DSZ64_DI(RAX, 0xBAD3), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 3);
    printf("  RAX = 0x%" PRIx64 "  expect 0x3  %s\n", r, ok ? "PASS" : "FAIL");
    if (!ok) {
        const char *reason = "";
        if      (r == 0xBAD1) reason = "  iter 1 fall-through unexpectedly";
        else if (r == 0xBAD2) reason = "  iter 2 fall-through unexpectedly";
        else if (r == 0xBAD3) reason = "  iter 3 branched — counter didn't reach threshold";
        else                  reason = "  unexpected — count-up may also have an issue";
        printf("  →%s\n", reason);
    }
    return ok;
}

/* ── Q. THE REAL THING — backward-branching loop
 *
 * Final test: a true backward branch (target points to an earlier triad).
 * If this works, the canonical microcode loop pattern is verified end-to-end.
 *
 * Iteration count kept very small (2 iterations) to minimize damage if
 * the branch somehow misbehaves. Every primitive used has been verified
 * by an earlier test:
 *   - MOVE_DSZ64_DI: tests A, M, et al.
 *   - ADD_DSZ64_DRI w/ positive imm: M, P
 *   - XOR_DSZ64_DRR: G, H, P
 *   - UJMPCC_CONDNZ_RI reading RFLAGS via intra-triad XOR: G, H, P
 *
 * Only the BACKWARD-target encoding is new. If P passed and Q hangs,
 * backward branches specifically have an issue.
 *
 *   T0:                counter = 0
 *   T1:                threshold = 2
 *   T2 (LOOP_TOP):     counter += 1
 *   T3:                XOR scratch = counter ^ threshold
 *                      UJMPCC CONDNZ_RI(scratch, T2)   ← BACKWARD branch
 *   T4 (exit):         RAX = counter; END
 *
 * Expected:
 *   iter 1: counter=1, scratch=1^2=3, CONDNZ branches back to T2
 *   iter 2: counter=2, scratch=2^2=0, CONDNZ falls through to T4
 *   exit:   RAX = 2
 */
static int test_q(void) {
    printf("--- Q: REAL backward-branch loop (count up to 2, GUARDED) ---\n");
    printf("    [if Q hangs, backward branches don't work — but forward chain does]\n");
    ucode_t p[] = {
        /* T0       */ { MOVE_DSZ64_DI(TMP0, 0),   NOP, NOP, NOP_SEQWORD },
        /* T1       */ { MOVE_DSZ64_DI(TMP1, 2),   NOP, NOP, NOP_SEQWORD },
        /* T2 LOOP  */ { ADD_DSZ64_DRI(TMP0, TMP0, 1), NOP, NOP, NOP_SEQWORD },
        /* T3       */ { XOR_DSZ64_DRR(TMP2, TMP0, TMP1),
                         NOP,
                         UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP2, T(2)),  /* backward! */
                         NOP_SEQWORD },
        /* T4 exit  */ { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, END_SEQWORD },
    };
    reinstall(p, ARRAY_SZ(p));
    uint64_t r = fire_patch();
    int ok = (r == 2);
    printf("  RAX = 0x%" PRIx64 "  expect 0x2  %s\n", r, ok ? "PASS" : "FAIL");
    if (!ok) {
        if      (r == 1) printf("  → loop ran once then fell through — backward branch not taken\n");
        else if (r >  2) printf("  → loop overran threshold — backward branch keeps firing past exit\n");
        else             printf("  → unexpected behavior\n");
    }
    return ok;
}

int main(void) {
    printf("=== Micro-op branch test (UJMP_I, UJMPCC) ===\n");
    printf("Region: U%04x — each test uses ≤11 triads at the same address\n\n", REGION);

    assign_to_core(0);

    int pass = 0, total = 0;
    total++; pass += test_a();
    total++; pass += test_b();
    total++; pass += test_c();
    total++; pass += test_d();
    total++; pass += test_e();
    total++; pass += test_f();
    /* G/H confirmed XOR→UJMPCC bridges flags in one triad. */
    total++; pass += test_g();
    total++; pass += test_h();
    /* I tests the actual loop primitive (SUB+XOR+UJMPCC in one triad). */
    total++; pass += test_i();
    /* J: XOR in slot 1 (not slot 0) → can UJMPCC slot 2 see the flag? */
    total++; pass += test_j();
    /* K: two-triad iteration — the robust loop primitive. */
    total++; pass += test_k();
    /* L / M: direct sanity check of SUB and ADD on TMP regs.
     * If K fails because SUB is a no-op on TMP, L will tell us. */
    total++; pass += test_l();
    total++; pass += test_m();
    /* N: ADD_DSZ64_DRI with NEGATIVE immediate — the decrement workaround. */
    total++; pass += test_n();
    /* O: re-run the K loop primitive using ADD(reg, reg, -1) instead of SUB. */
    total++; pass += test_o();
    /* P: count-UP loop primitive (works around the 16-bit unsigned immediate
     * field — decrement-by-immediate doesn't work, but increment-and-compare
     * does). This is the actual practical loop primitive. */
    total++; pass += test_p();
    /* Q: REAL backward branch using the verified primitive. Last test;
     * guarded with small iteration count. */
    total++; pass += test_q();

    init_match_and_patch();
    do_fix_IN_patch();

    printf("\n=== %d / %d passed ===\n", pass, total);

    /* Diagnostic decoder. */
    printf("\nInterpretation:\n");
    printf("  B PASS, C FAIL, E FAIL, F PASS: confirms UJMPCC reads RFLAGS,\n");
    printf("    not the register operand. RFLAGS appears frozen at ZF=0 at patch entry.\n");
    printf("\n  G/H tell us whether intra-triad XOR→UJMPCC bridges flags:\n");
    printf("    G=PASS, H=PASS: XOR sets ZF visible to UJMPCC same-triad.\n");
    printf("                    Real conditional branches possible via this pattern.\n");
    printf("    G=FAIL, H=PASS: XOR doesn't bridge to RFLAGS at all (everything CONDZ-false).\n");
    printf("    G=PASS, H=FAIL: XOR bridges sometimes; suspect unreliable. Try other ops.\n");
    printf("    G=FAIL, H=FAIL: arch RFLAGS unwritable from microcode patches.\n");
    printf("                    Only UJMP_I works; no real conditional control flow.\n");

    return (pass == total) ? 0 : 1;
}
