/*
 * cross_region_jump_test.c — Test cross-region patch RAM jumping
 *
 * Goal: Verify that:
 *   1. Two separate patch_ucode() calls coexist
 *   2. A GOTO seqword can jump between non-contiguous patch RAM regions
 *
 * Strategy:
 *   Fragment A @ REGION_A: writes markers to TMP0, TMP1, TMP2
 *                          last triad jumps to REGION_B
 *   Fragment B @ REGION_B: writes markers to TMP3, TMP4
 *                          merges all markers into RAX, ends
 *
 * Each fragment writes a known value. Final RAX = XOR of all markers.
 * If cross-region jump works: RAX = 0x1111 ^ 0x2222 ^ 0x3333 ^ 0x4444 ^ 0x5555
 * If fragment B never runs:   RAX will be wrong or we crash
 *
 * Hook: vmwrite rcx, rcx (0x0cd8 → patch @ REGION_A)
 * Input:  RCX = ignored
 * Output: RAX = expected XOR result
 *
 * Build: make PROG=cross_region_jump_test
 * Run:   sudo taskset -c 0 ./cross_region_jump_test_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* Pick two non-contiguous free regions from your dump.
 * Both must be triad-aligned (multiple of 4).
 * Region A: U7d7c (133 free) — using first 4 triads
 * Region B: U7cc8 (24 free, 7cc7 is not aligned, so 7cc8) — using 4 triads
 *
 * Adjust these if your layout changes.
 */
#define REGION_A    0x7d7c
#define REGION_B    0x7cc8
#define MP_INDEX    19          /* first free match-and-patch slot */

#define MARKER_1    0x1111
#define MARKER_2    0x2222
#define MARKER_3    0x3333
#define MARKER_4    0x4444
#define MARKER_5    0x5555

#define EXPECTED    (MARKER_1 ^ MARKER_2 ^ MARKER_3 ^ MARKER_4 ^ MARKER_5)

static void install_test(void) {

    /* Fragment A: 4 triads @ REGION_A
     * T0: load markers into TMP0, TMP1, TMP2
     * T1: XOR TMP0 ^= TMP1, keep TMP2 for later
     * T2: XOR TMP0 ^= TMP2 (TMP0 now = 0x1111^0x2222^0x3333)
     * T3: jump to REGION_B (3 NOPs + GOTO seqword)
     */
    ucode_t frag_a[] = {
        /* T0 */
        {   MOVE_DSZ64_DI(TMP0, MARKER_1),
            MOVE_DSZ64_DI(TMP1, MARKER_2),
            MOVE_DSZ64_DI(TMP2, MARKER_3),
            NOP_SEQWORD },
        /* T1 */
        {   XOR_DSZ64_DRR(TMP0, TMP0, TMP1),
            NOP,
            NOP,
            NOP_SEQWORD },
        /* T2 */
        {   XOR_DSZ64_DRR(TMP0, TMP0, TMP2),
            NOP,
            NOP,
            NOP_SEQWORD },
        /* T3: link triad — jump to fragment B */
        {   NOP,
            NOP,
            NOP,
            SEQ_GOTO0(REGION_B) },
    };

    /* Fragment B: 3 triads @ REGION_B
     * T0: load markers 4 and 5
     * T1: XOR them together, then XOR with TMP0 from fragment A
     * T2: move result to RAX, end
     */
    ucode_t frag_b[] = {
        /* T0 */
        {   MOVE_DSZ64_DI(TMP3, MARKER_4),
            MOVE_DSZ64_DI(TMP4, MARKER_5),
            NOP,
            NOP_SEQWORD },
        /* T1 */
        {   XOR_DSZ64_DRR(TMP3, TMP3, TMP4),
            XOR_DSZ64_DRR(TMP0, TMP0, TMP3),
            NOP,
            NOP_SEQWORD },
        /* T2 */
        {   MOVE_DSZ64_DR(RAX, TMP0),
            NOP,
            NOP,
            END_SEQWORD },
    };

    printf("Fragment A: %zu triads @ U%04x\n", ARRAY_SZ(frag_a), REGION_A);
    printf("Fragment B: %zu triads @ U%04x\n", ARRAY_SZ(frag_b), REGION_B);
    printf("Jump: U%04x -> U%04x (cross-region)\n", REGION_A + 0xc, REGION_B);
    printf("Expected RAX: 0x%04x\n\n", EXPECTED);

    assign_to_core(0);
    do_fix_IN_patch();

    /* Install fragment B first, then A — tests that order doesn't matter */
    printf("Installing fragment B...\n");
    patch_ucode(REGION_B, frag_b, ARRAY_SZ(frag_b));

    printf("Installing fragment A...\n");
    patch_ucode(REGION_A, frag_a, ARRAY_SZ(frag_a));

    /* Hook vmwrite to enter at fragment A */
    hook_match_and_patch(MP_INDEX, 0x0cd8, REGION_A);

    printf("Hook installed (slot %d: U%04x -> U%04x)\n\n", MP_INDEX, 0x0cd8, REGION_A);
}

static inline uint64_t do_vmwrite(uint64_t val) {
    uint64_t result;
    asm volatile(
        "mov rcx, %[v]\n\t"
        "vmwrite rcx, rcx\n\t"
        : "=a"(result)
        : [v] "r"(val)
        : "rcx", "rdx", "r8", "memory"
    );
    return result;
}

int main(void) {
    printf("=== Cross-region patch RAM jump test ===\n\n");

    install_test();

    /* Run multiple times to check stability */
    int pass = 1;
    for (int i = 0; i < 8; i++) {
        uint64_t r = do_vmwrite(0);
        int ok = (r == EXPECTED);
        if (!ok) pass = 0;
        printf("  run %d: RAX=0x%04" PRIx64 "  expected=0x%04x  %s\n",
               i, r, EXPECTED, ok ? "PASS" : "FAIL");
    }

    printf("\n%s\n", pass ? "All tests passed — cross-region jump works."
                          : "SOME TESTS FAILED.");
    return !pass;
}
