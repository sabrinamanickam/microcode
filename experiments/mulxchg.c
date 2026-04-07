#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/*
 * Semantics (with our patch):
 *   Inputs from the vmwrite r64,r64 operands:
 *     RAX_in = multiplicand (A)
 *     RCX_in = multiplier   (B)
 *   Microcode:
 *     RBX = RAX
 *     RBX = RBX * RCX   (signed IMUL, low 64-bit result)
 *     XCHG RAX, RBX     ; result ends in RAX
 */
static void install_imulxchg_vmwrite_patch(void) {
    ucode_t ucode_patch[] = {
        {
            /* RBX = RAX */
            MOVE_DSZ64_DR(TMP1, RAX),

            /* RBX = RBX * RCX  (signed multiply, low 64 bits) */
            /* IMUL64L_DSZ64_DRR(dst, src0, src1) */
            MUL_DSZ64_DRR(RAX, RAX, RCX),

            /* swap product into RAX so caller reads RAX */
	    NOP,

            END_SEQWORD
        }
    };

    assign_to_core(0);
    do_fix_IN_patch();

    patch_ucode(0x7c4c, ucode_patch, ARRAY_SZ(ucode_patch));
    hook_match_and_patch(0 /*core*/, 0x0cd8, /*vmwrite_r64_r64_xlat*/ 0x7c4c /*slot*/);
}

int main(void) {
    uint64_t result;
    uint64_t a = 0x1234;   /* multiplicand */
    uint64_t b = 0x10;     /* multiplier  */

    printf("Installing MOV/IMUL/XCHG vmwrite patch…\n");
    install_imulxchg_vmwrite_patch();

    asm volatile(
        "mov %1, %%rax\n\t"
        "mov %2, %%rcx\n\t"
        "vmwrite %%rax, %%rcx\n\t"
        : "=a"(result)
        : "r"(a), "r"(b)
        );

    printf("RAX (A*B low 64) = 0x%lx\n", result);
    return 0;
}

