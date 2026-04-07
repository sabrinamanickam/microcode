#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

void install_mul_xchg_patch(void) {
    ucode_t ucode_patch[] = {
        {
            /* uop 0: TMP1 = RAX_in */
            MOVE_DSZ64_DR(TMP1, RAX),
            /* uop 1: RAX = TMP1 + RCX_in  (register + register) */
            /* ADD_DSZ64_DRR(dst, src, reg) */
            MOVE_DSZ64_DR(TMP2, RDX),

            MUL_DSZ64_DRR(RAX, TMP1, TMP2),

            /* uop 2: RAX <<= RCX_in  (variable shift by register) */
            /* SHL_DSZ64_DRR(dst, src, reg) */
            /* end of sequence */
            END_SEQWORD
        }
    };

    assign_to_core(0);
    do_fix_IN_patch();

    /* write patch to some free slot, e.g. 0x7c4c (same as your example) */
    patch_ucode(0x7c30, ucode_patch, ARRAY_SZ(ucode_patch));

    /* hook: vmwrite r64,r64 translator id is 0x0cd8 */
    hook_match_and_patch(0 /*core*/, 0x07e8 /*cmpxchg_r64_r64_xlat*/, 0x7c30/*slot*/);
}
int main(void) {
    uint64_t out_rax, out_rdx;
    uint64_t a = 0x0002ULL;  /* multiplicand in RAX */
    uint64_t b = 0x0004ULL;  /* multiplier   in RDX */

    printf("After patch:\n");
    install_mul_xchg_patch();

    /* Load inputs, run vmwrite (our hook), then read back RAX and RDX */
    asm volatile(
        "mov %2, %%rax\n\t"
        "mov %3, %%rdx\n\t"
        "cmpxchg %%rax, %%rdx\n\t"
        : "=a"(out_rax), "=d"(out_rdx)     /* outputs: RAX, RDX after the patch */
        : "r"(a), "r"(b)                   /* inputs: a -> RAX, b -> RDX */
        : "cc", "memory"                   /* clobbers */
    );

    printf("RAX after patch = 0x%016lx\n", out_rax);
    printf("RDX after patch = 0x%016lx\n", out_rdx);
    return 0;
}
