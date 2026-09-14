/*
 * test_slot02_raw.c — Test if slot 0 → slot 2 value RAW works in Goldmont microcode.
 *
 * Tests whether slot 2 of a triad can read a VALUE written by slot 0.
 * We know slot 0→1 value RAW works (confirmed by P-256 code).
 * We know slot 1→2 FLAGS RAW works (SETCC reads flags from slot 1).
 * This tests slot 0→2 VALUE forwarding.
 *
 * If this works, P-521 squaring can fit in 114 triads (under 120 limit).
 *
 * Build:  make PROG=test_slot02_raw
 * Run:    sudo taskset -c 0 ./test_slot02_raw_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* ── Test 1: ADD in slot 0, ADD using result in slot 2 ── */
static void install_test1(void) {
    ucode_t patch[] = {
    /* R15 = 0x100, R9 = 0x200, R10 = 0x300 at entry */
    /* Test: slot 0 writes TMP0, slot 2 reads TMP0 */
    { ADD_DSZ64_DRR(TMP0, R15, R9),       /* TMP0 = 0x100 + 0x200 = 0x300 (slot 0) */
      NOP,                                  /* slot 1: nothing */
      ADD_DSZ64_DRR(R13, TMP0, R10),       /* R13 = TMP0 + R10 (slot 2 reads TMP0) */
      NOP_SEQWORD },
    /* If slot 0→2 RAW works: R13 = 0x300 + 0x300 = 0x600 */
    /* If NOT: R13 = old_TMP0 + 0x300 (whatever TMP0 was before) */
    { NOP, NOP, NOP, END_SEQWORD }
    };
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static uint64_t run_test1(void) {
    uint64_t result;
    asm volatile(
        "mov r15, 0x100\n\t"
        "mov r9, 0x200\n\t"
        "mov r10, 0x300\n\t"
        "xor r13d, r13d\n\t"    /* clear R13 (old TMP0 will be 0 if not forwarded) */
        "vmwrite rcx, rdx\n\t"
        "mov %0, r13\n\t"
        : "=r"(result)
        :
        : "r15", "r9", "r10", "r13", "rcx", "rdx", "memory", "cc"
    );
    return result;
}

/* ── Test 2: SHR in slot 0, read result in slot 2 ── */
static void install_test2(void) {
    ucode_t patch[] = {
    /* R15 = 0xABCD000000000000 at entry */
    /* Test: SHR in slot 0, ADD using result in slot 2 */
    { SHR_DSZ64_DRI(TMP0, R15, 48),        /* TMP0 = 0xABCD (slot 0) */
      NOP,
      ADD_DSZ64_DRR(R13, TMP0, TMP0),      /* R13 = TMP0 + TMP0 = 2*0xABCD? (slot 2) */
      NOP_SEQWORD },
    { NOP, NOP, NOP, END_SEQWORD }
    };
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static uint64_t run_test2(void) {
    uint64_t result;
    asm volatile(
        "mov r15, 0xABCD000000000000\n\t"
        "xor r13d, r13d\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %0, r13\n\t"
        : "=r"(result)
        :
        : "r15", "r13", "rcx", "rdx", "memory", "cc"
    );
    return result;
}

/* ── Test 3: Slot 0+2 WAW (both write same register) ── */
static void install_test3(void) {
    ucode_t patch[] = {
    /* Test: slot 0 writes R13=0x111, slot 2 writes R13=0x333. Which wins? */
    { ADD_DSZ64_DRR(R13, R15, R15),         /* R13 = 2*R15 = 0x200 (slot 0) */
      NOP,
      ADD_DSZ64_DRR(R13, R9, R10),          /* R13 = R9 + R10 = 0x500 (slot 2) */
      NOP_SEQWORD },
    { NOP, NOP, NOP, END_SEQWORD }
    };
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static uint64_t run_test3(void) {
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

/* ── Test 4: Slot 0→2 with MUL in slot 1 (the actual P-521 pattern) ── */
static void install_test4(void) {
    ucode_t patch[] = {
    /* PREP: copy R9 to TMP10 for MUL srcA */
    { ZEROEXT_DSZ64_DR(TMP10, R9),
      ZEROEXT_DSZ64_DR(RDX, R10),
      NOP, NOP_SEQWORD },
    /* The actual pattern: ADD slot 0, MUL slot 1, ADD slot 2 reading slot 0's result */
    { ADD_DSZ64_DRR(TMP0, R15, R15),       /* TMP0 = 2*R15 = 0x200 (slot 0) */
      MUL_DSZ64_DRR(RCX, TMP10, RDX),      /* MUL in slot 1 (doesn't touch TMP0) */
      ADD_DSZ64_DRR(R13, TMP0, TMP0),       /* R13 = 2*TMP0 = 0x400? (slot 2) */
      NOP_SEQWORD },
    { NOP, NOP, NOP, END_SEQWORD }
    };
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static uint64_t run_test4(void) {
    uint64_t result;
    asm volatile(
        "mov r15, 0x100\n\t"
        "mov r9, 7\n\t"        /* a value for MUL srcA */
        "mov r10, 11\n\t"      /* a value for MUL srcB */
        "xor r13d, r13d\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %0, r13\n\t"
        : "=r"(result)
        :
        : "r15", "r9", "r10", "r13", "rcx", "rdx", "memory", "cc"
    );
    return result;
}

int main(void) {
    printf("=== Slot 0→2 Value RAW Test ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    /* Test 1: ADD slot 0, ADD slot 2 reading result */
    install_test1();
    uint64_t r1 = run_test1();
    printf("Test 1 (ADD s0, ADD s2 reads TMP0):\n");
    printf("  Result: 0x%lx (expect 0x600 if s0→s2 RAW works, else 0x300)\n", r1);
    printf("  Slot 0→2 RAW: %s\n\n", r1 == 0x600 ? "WORKS" : "DOES NOT WORK");

    /* Test 2: SHR slot 0, ADD slot 2 */
    init_match_and_patch(); do_fix_IN_patch();
    install_test2();
    uint64_t r2 = run_test2();
    printf("Test 2 (SHR s0, ADD s2 reads TMP0):\n");
    printf("  Result: 0x%lx (expect 0x1579A if s0→s2 RAW works)\n", r2);
    printf("  Slot 0→2 RAW: %s\n\n", r2 == 0x1579A ? "WORKS" : "DOES NOT WORK");

    /* Test 3: WAW — slot 0 and slot 2 both write R13 */
    init_match_and_patch(); do_fix_IN_patch();
    install_test3();
    uint64_t r3 = run_test3();
    printf("Test 3 (WAW: s0 writes 0x200, s2 writes 0x500):\n");
    printf("  Result: 0x%lx\n", r3);
    printf("  Slot 2 wins: %s, Slot 0 wins: %s\n\n",
           r3 == 0x500 ? "YES" : "no", r3 == 0x200 ? "YES" : "no");

    /* Test 4: ADD s0, MUL s1, ADD s2 reads s0 (the actual P-521 pattern) */
    init_match_and_patch(); do_fix_IN_patch();
    install_test4();
    uint64_t r4 = run_test4();
    printf("Test 4 (ADD s0, MUL s1, ADD s2 reads s0 result):\n");
    printf("  Result: 0x%lx (expect 0x400 if s0→s2 RAW works with MUL in s1)\n", r4);
    printf("  Slot 0→2 RAW (with MUL): %s\n\n", r4 == 0x400 ? "WORKS" : "DOES NOT WORK");

    /* Summary */
    int s02_works = (r1 == 0x600) && (r2 == 0x1579A) && (r4 == 0x400);
    int waw_s2_wins = (r3 == 0x500);
    printf("=== Summary ===\n");
    printf("  Slot 0→2 value RAW: %s\n", s02_works ? "CONFIRMED" : "NOT AVAILABLE");
    printf("  Slot 0+2 WAW (s2 wins): %s\n", waw_s2_wins ? "CONFIRMED" : "NOT CONFIRMED");

    if (s02_works) {
        printf("\n  → P-521 squaring can use 114-triad patch (under 120 limit)!\n");
        printf("  → Expected ~119 cycles, beating GCC's 127.\n");
    } else {
        printf("\n  → P-521 squaring needs 122 triads (exceeds 120 limit).\n");
        printf("  → Cannot beat GCC with single-vmwrite approach.\n");
    }

    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
