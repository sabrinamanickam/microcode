/*
 * test_slot_raw_extended.c — Test MUL srcB RAW and slot 1→2 value RAW
 *
 * Test A: After MUL in slot 0, can slot 1 read the lo product from srcB (RDX)?
 *   If yes: merge mu_MUL + save_m into 1 triad, saving 2 triads total.
 *
 * Test B: Can slot 2 read a VALUE written by slot 1?
 *   If yes: merge carry chain triads, saving 2 more triads total.
 *
 * Build:  make PROG=test_slot_raw_extended
 * Run:    sudo taskset -c 0 ./test_slot_raw_extended_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* ── Test A: MUL srcB slot 0→1 RAW ── */
/* MUL(RCX, srcA, RDX) in slot 0: srcA preserved, RDX gets lo, RCX gets hi.
 * Can ZEROEXT in slot 1 read the NEW RDX (lo product)? */
static void install_testA(void) {
    ucode_t patch[] = {
    /* PREP: TMP10 = 7 (srcA for MUL) */
    { ZEROEXT_DSZ64_DR(TMP10, R15), NOP, NOP, NOP_SEQWORD },
    /* R15 = 7, R9 = 11. RDX = 11 (set by inline asm).
     * MUL: 7 × 11 = 77. lo = 77 → RDX. hi = 0 → RCX.
     * Test: slot 1 reads RDX → should get 77 (lo product), not 11 (old RDX). */
    { MUL_DSZ64_DRR(RCX, TMP10, RDX),          /* slot 0: MUL writes RDX=lo=77 */
      ZEROEXT_DSZ64_DR(R13, RDX),               /* slot 1: reads RDX → R13 */
      NOP, NOP_SEQWORD },
    { NOP, NOP, NOP, END_SEQWORD }
    };
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static uint64_t run_testA(void) {
    uint64_t result;
    asm volatile(
        "mov r15, 7\n\t"
        "mov rdx, 11\n\t"
        "xor r13d, r13d\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %0, r13\n\t"
        : "=r"(result)
        :
        : "r15", "r13", "r9", "rcx", "rdx", "memory", "cc"
    );
    return result;
}

/* ── Test A2: MUL srcB + ADD in slot 1 reading lo ── */
/* Same but slot 1 does ADD using the lo product */
static void install_testA2(void) {
    ucode_t patch[] = {
    { ZEROEXT_DSZ64_DR(TMP10, R15), ZEROEXT_DSZ64_DR(R13, R9),
      NOP, NOP_SEQWORD },
    /* MUL: 7 × 11 = 77. Slot 1: R13 = R13 + RDX (should be 100 + 77 = 177) */
    { MUL_DSZ64_DRR(RCX, TMP10, RDX),
      ADD_DSZ64_DRR(R13, R13, RDX),
      NOP, NOP_SEQWORD },
    { NOP, NOP, NOP, END_SEQWORD }
    };
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static uint64_t run_testA2(void) {
    uint64_t result;
    asm volatile(
        "mov r15, 7\n\t"
        "mov r9, 100\n\t"
        "mov rdx, 11\n\t"
        "xor r13d, r13d\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %0, r13\n\t"
        : "=r"(result)
        :
        : "r15", "r9", "r13", "rcx", "rdx", "memory", "cc"
    );
    return result;
}

/* ── Test B: Slot 1→2 value RAW ── */
/* ADD in slot 1 writes TMP0. Can slot 2 read TMP0's VALUE? */
static void install_testB(void) {
    ucode_t patch[] = {
    /* R15 = 0x100, R9 = 0x200, R10 = 0x300 */
    { NOP,
      ADD_DSZ64_DRR(TMP0, R15, R9),             /* slot 1: TMP0 = 0x300 */
      ADD_DSZ64_DRR(R13, TMP0, R10),            /* slot 2: R13 = TMP0 + R10 */
      NOP_SEQWORD },
    /* If slot 1→2 value RAW works: R13 = 0x300 + 0x300 = 0x600 */
    /* If NOT: R13 = old_TMP0 + 0x300 */
    { NOP, NOP, NOP, END_SEQWORD }
    };
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static uint64_t run_testB(void) {
    uint64_t result;
    asm volatile(
        "mov r15, 0x100\n\t"
        "mov r9, 0x200\n\t"
        "mov r10, 0x300\n\t"
        "xor r13d, r13d\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %0, r13\n\t"
        : "=r"(result)
        :
        : "r15", "r9", "r10", "r13", "rcx", "rdx", "memory", "cc"
    );
    return result;
}

/* ── Test B2: Slot 1→2 with SETCC in slot 0 (the actual P-224 pattern) ── */
/* { SETCC(slot0), ADD(slot1), ADD_reading_slot1(slot2) } */
static void install_testB2(void) {
    ucode_t patch[] = {
    /* First triad: set up TMP0 with a known value via ADD */
    { ADD_DSZ64_DRR(TMP0, R15, R9), NOP, NOP, NOP_SEQWORD },
    /* Second triad: the pattern we want to test */
    { SETCC_CONDB_DR(TMP1, TMP0),               /* slot 0: read TMP0 flags */
      ADD_DSZ64_DRR(TMP2, R10, R13),            /* slot 1: TMP2 = 0x300 + 0x400 = 0x700 */
      ADD_DSZ64_DRR(R13, TMP0, TMP2),           /* slot 2: R13 = TMP0 + TMP2 */
      NOP_SEQWORD },
    /* If slot 1→2 value RAW works: R13 = 0x300 + 0x700 = 0xA00 */
    /* If NOT: R13 = 0x300 + old_TMP2 */
    { NOP, NOP, NOP, END_SEQWORD }
    };
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static uint64_t run_testB2(void) {
    uint64_t result;
    asm volatile(
        "mov r15, 0x100\n\t"
        "mov r9, 0x200\n\t"
        "mov r10, 0x300\n\t"
        "mov r13, 0x400\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %0, r13\n\t"
        : "=r"(result)
        :
        : "r15", "r9", "r10", "r13", "rcx", "rdx", "memory", "cc"
    );
    return result;
}

int main(void) {
    printf("=== Extended Slot RAW Tests ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    /* Test A: MUL srcB slot 0→1 RAW */
    install_testA();
    uint64_t rA = run_testA();
    printf("Test A (MUL srcB s0→s1: ZEROEXT reads RDX after MUL):\n");
    printf("  R13 = %lu (expect 77 if MUL srcB RAW works, 11 if not)\n", rA);
    printf("  MUL srcB slot 0→1 RAW: %s\n\n",
           rA == 77 ? "WORKS" : (rA == 11 ? "DOES NOT WORK (reads old RDX)" : "UNEXPECTED"));

    /* Test A2: MUL srcB + ADD in slot 1 */
    init_match_and_patch(); do_fix_IN_patch();
    install_testA2();
    uint64_t rA2 = run_testA2();
    printf("Test A2 (MUL srcB s0→s1: ADD reads RDX after MUL):\n");
    printf("  R13 = %lu (expect 177 if works, 111 if not)\n", rA2);
    printf("  MUL srcB slot 0→1 RAW (ADD): %s\n\n",
           rA2 == 177 ? "WORKS" : (rA2 == 111 ? "DOES NOT WORK" : "UNEXPECTED"));

    /* Test B: Slot 1→2 value RAW */
    init_match_and_patch(); do_fix_IN_patch();
    install_testB();
    uint64_t rB = run_testB();
    printf("Test B (Slot 1→2 value RAW: ADD in s1, ADD in s2 reads result):\n");
    printf("  R13 = 0x%lx (expect 0x600 if works)\n", rB);
    printf("  Slot 1→2 value RAW: %s\n\n",
           rB == 0x600 ? "WORKS" : "DOES NOT WORK");

    /* Test B2: SETCC(s0) + ADD(s1) + ADD(s2) reading s1 */
    init_match_and_patch(); do_fix_IN_patch();
    install_testB2();
    uint64_t rB2 = run_testB2();
    printf("Test B2 (SETCC s0, ADD s1, ADD s2 reads s1 result):\n");
    printf("  R13 = 0x%lx (expect 0xA00 if slot 1→2 value RAW works)\n", rB2);
    printf("  Slot 1→2 value RAW (with SETCC): %s\n\n",
           rB2 == 0xA00 ? "WORKS" : "DOES NOT WORK");

    /* Summary */
    int mulRAW = (rA == 77) && (rA2 == 177);
    int s12RAW = (rB == 0x600) && (rB2 == 0xA00);

    printf("=== Summary ===\n");
    printf("  MUL srcB slot 0→1 RAW: %s\n", mulRAW ? "CONFIRMED" : "NOT AVAILABLE");
    printf("  Slot 1→2 value RAW: %s\n", s12RAW ? "CONFIRMED" : "NOT AVAILABLE");

    int savings = 0;
    if (mulRAW) savings += 2;
    if (s12RAW) savings += 2;
    printf("\n  P-224 triads: 43 → %d (saving %d)\n", 43-savings, savings);
    printf("  Estimated: ~%d cycles (CryptOpt target: ~122)\n",
           (int)((43-savings) * 2 * 1.35 + 10));

    if (mulRAW && s12RAW)
        printf("  → BOTH confirmed: can beat CryptOpt!\n");
    else if (mulRAW)
        printf("  → MUL RAW only: ~121 cycles, borderline\n");
    else
        printf("  → Cannot beat CryptOpt with current approach\n");

    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
