#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/*
 * Hook PAUSE (pause_xlat = 0x0bf0) to do: RAX = RAX * RDX  (low 64-bit)
 * Inputs:  RAX = A,  RDX = B
 * Output:  RAX = A * B
 */
static void install_mul_pause_patch(void) {
    ucode_t ucode_patch[] = {{
        /* Latch inputs first (good style even if regs are safe) */
        MOVE_DSZ64_DR(TMP1, RAX),         /* TMP1 = A */
        MOVE_DSZ64_DR(TMP2, RDX),         /* TMP2 = B */

        /* RAX = TMP1 * TMP2  (low 64-bit result) */
        MUL_DSZ64_DRR(RAX, TMP1, TMP2),

        END_SEQWORD
    }};

    assign_to_core(0);
    do_fix_IN_patch();

    /* Write triad and hook PAUSE instead of VMWRITE */
    patch_ucode(0x7c4c, ucode_patch, ARRAY_SZ(ucode_patch));
    hook_match_and_patch(0 /* core */, 0x0bf0 /* pause_xlat */, 0x7c4c /* slot */);
}

int main(void) {
    uint64_t result;
    uint64_t a = 0x0003ULL;  /* multiplicand */
    uint64_t b = 0x0002ULL;  /* multiplier   */

    printf("After patch:\n");
    install_mul_pause_patch();

    /* A in RAX, B in RDX; PAUSE triggers our ucode */
    asm volatile(
        "mov %1, %%rax\n\t"
        "mov %2, %%rdx\n\t"
        "pause\n\t"
        : "=a"(result)
        : "r"(a), "r"(b)
        : "rdx", "cc", "memory"
    );

    printf("RAX result = 0x%lx\n", result);  /* expect 0x6 for 3*2 */
    return 0;
}

