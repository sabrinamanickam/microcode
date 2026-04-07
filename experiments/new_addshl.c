#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

void install_addshl_vmwrite_patch(void) {
    ucode_t ucode_patch[] = {
        {
            
            MOVE_DSZ64_DR(RAX, RBX),

            MOVE_DSZ64_DR(RDX, RCX),
	    NOP,

            END_SEQWORD
        }
    };

    assign_to_core(0);
    do_fix_IN_patch();

    /* write patch to some free slot, e.g. 0x7c4c (same as your example) */
    patch_ucode(0x7c4c, ucode_patch, ARRAY_SZ(ucode_patch));

    /* hook: vmwrite r64,r64 translator id is 0x0cd8 */
    hook_match_and_patch(0 /*core*/, 0x0cd8 /*vmwrite_r64_r64_xlat*/, 0x7c4c /*slot*/);
}
int main(void) {
    uint64_t out_rax, out_rdx;
    uint64_t a = 0x0002ULL;  
    uint64_t b = 0x0006ULL;  

    printf("After patch:\n");
    install_addshl_vmwrite_patch();

    
    asm volatile(
        "mov %2, %%rbc\n\t"
        "mov %3, %%rcx\n\t"
        "vmwrite %%rbx, %%rcx\n\t"
        : "=a"(out_rax), "=d"(out_rdx)     /* outputs: RAX, RDX after the patch */
        : "r"(a), "r"(b)                   /* inputs: a -> RAX, b -> RDX */
    );

    printf("RAX after patch = 0x%016lx\n", out_rax);
    printf("RDX after patch = 0x%016lx\n", out_rdx);
    return 0;
}
