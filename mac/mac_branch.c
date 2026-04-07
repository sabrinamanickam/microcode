/*
 * mac_branch.c — Branchless conditional select in Goldmont microcode
 *
 * UJMPCC has a micro-branch predictor that latches on the previous
 * outcome, making dynamic (alternating) branches unreliable.
 * SUBR + SEQ_GOTO doesn't gate on Goldmont. Sync barriers don't help.
 *
 * Working solution: SUBR→TMP sets internal ALU flags, SETCC_CONDZ
 * reads ZF correctly. Build a mask and bitwise-select the result.
 *
 *   SUBR TMP0, RCX, 0     → TMP0 = RCX, ZF set if RCX==0
 *   SETCC_CONDZ TMP1       → TMP1 = 1 if ZF, else 0
 *   mask = 0 - TMP1        → all-ones or all-zeros
 *   RAX = (val_a & mask) | (val_b & ~mask)
 *
 * Hook: vmwrite rcx, rcx  (0x0cd8 → patch @ 0x7c00)
 * Input:  RCX = test value
 * Output: RAX = 0xAAAA if RCX==0, 0xBBBB otherwise
 *
 * Build: make PROG=mac_branch
 * Run:   sudo taskset -c 0 ./mac_branch_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define PATCH_ADDR  0x7C00
#define MP_INDEX    18

static void install_cond_select(void) {
    ucode_t patch[] = {
        /* T0: SUBR→TMP0 sets ZF; SETCC_CONDZ reads it */
        {   SUBR_DSZ64_DRI(TMP0, RCX, 0),
            SETCC_CONDZ_DR(TMP1, TMP0),
            NOP,
            NOP_SEQWORD },

        /* T1: mask = 0 - TMP1 (all-ones if zero, all-zeros if not) */
        {   SUBR_DSZ64_DIR(TMP2, 0, TMP1),
            MOVE_DSZ64_DI(TMP3, 0xAAAA),
            MOVE_DSZ64_DI(TMP4, 0xBBBB),
            NOP_SEQWORD },

        /* T2: select: (val_a & mask) | (val_b & ~mask) */
        {   AND_DSZ64_DRR(TMP5, TMP3, TMP2),
            NOTAND_DSZ64_DRR(TMP6, TMP2, TMP4),
            NOP,
            NOP_SEQWORD },

        /* T3: merge and end */
        {   OR_DSZ64_DRR(RAX, TMP5, TMP6),
            NOP,
            NOP,
            END_SEQWORD },
    };

    printf("Installing branchless select (%zu triads)\n", ARRAY_SZ(patch));

    assign_to_core(0);
    do_fix_IN_patch();

    patch_ucode(PATCH_ADDR, patch, ARRAY_SZ(patch));
    hook_match_and_patch(MP_INDEX, 0x0cd8, PATCH_ADDR);
    printf("Hook installed.\n");
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
    printf("=== Branchless conditional select (SETCC_CONDZ) ===\n\n");
    install_cond_select();

    struct { uint64_t val; uint64_t expect; } tests[] = {
        { 0,          0xAAAA },
        { 1,          0xBBBB },
        { 0xFFFF,     0xBBBB },
        { 0,          0xAAAA },
        { 1,          0xBBBB },
        { 0,          0xAAAA },
        { 0,          0xAAAA },
        { 42,         0xBBBB },
    };
    int n = sizeof(tests) / sizeof(tests[0]);
    int pass = 1;

    for (int i = 0; i < n; i++) {
        uint64_t r = do_vmwrite(tests[i].val);
        int ok = (r == tests[i].expect);
        if (!ok) pass = 0;
        printf("  RCX=%-10" PRIu64 " → RAX=0x%04" PRIx64 "  %s\n",
               tests[i].val, r, ok ? "PASS" : "FAIL");
    }

    printf("\n%s\n", pass ? "All tests passed." : "SOME TESTS FAILED.");
    return !pass;
}
