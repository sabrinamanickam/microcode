#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"
#include "../../include/opcode.h"
#include "../../include/inst.h"
#include "../../include/ucode_macro.h"


/*
 * Patch semantics:
 *   Inputs via vmwrite r64,r64:
 *     RAX_in = A (multiplicand)
 *     RCX_in = B (multiplier)
 *   Microcode:
 *     RAX = RAX * RCX   (low 64-bit product)
 */
static void install_mul_vmwrite_patch(void) {
    ucode_t ucode_patch[] = {
        {
            /* single uop: 64-bit multiply, reg * reg -> dst */
            MUL_DSZ64_DRR(RAX, RAX, RBX),
	    NOP,
	    NOP,

            END_SEQWORD
        }
    };

    assign_to_core(0);
    do_fix_IN_patch();

    /* write triad to a free slot */
    patch_ucode(0x7c4c, ucode_patch, ARRAY_SZ(ucode_patch));

    /* hook vmwrite r64,r64 (xlat id 0x0cd8) to our patch */
    hook_match_and_patch(0 /*core*/, 0x0cd8 /*vmwrite_r64_r64_xlat*/, 0x7c4c /*slot*/);
}

int main(void) {
    uint64_t result;
    uint64_t a = 0x1212ULL;  /* multiplicand */
    uint64_t b = 0x2ULL;    /* multiplier  */

    printf("Installing MUL vmwrite patch…\n");
    install_mul_vmwrite_patch();

    /* Load A in RAX, B in RCX; execute our hooked vmwrite */
    asm volatile(
        "mov %1, %%rax\n\t"
        "mov %2, %%rbx\n\t"
        "vmwrite %%rax, %%rcx\n\t"
        : "=a"(result)
        : "r"(a), "r"(b)
        : "rcx", "cc", "memory");

    printf("RAX (A*B low64) = 0x%016lx  [expected 0x%016lx]\n",
           result, (unsigned long)(a * b));
    return 0;
}

