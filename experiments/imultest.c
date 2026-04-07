#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

void install_addshl_vmwrite_patch(void) {
    ucode_t ucode_patch[] = {
        {
            IMUL_DSZ64_DRR(RAX,R64DST,R64SRC),
            NOP,
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
int main(void)
{
        uint64_t rax; uint64_t rbx; uint64_t rdx; uint64_t rcx; uint64_t rdi;
        printf("After patch:\n");
        install_addshl_vmwrite_patch();
        asm volatile( "movq %%rbx, 1; movq %%rcx, 2; movq %%rdx, 3; movq %%rax, 4\n;"
                        "vmwrite %%rdx, %%rcx\n\t"
                        : "=a"(rax), "=b"(rbx), "=c"(rcx) ,"=d"(rdx), "=D"(rdi) /* outputs: RAX, RDX after the patch */ /* inputs: a -> RAX, b -> RDX */
                        );
        printf("RBX after patch = 0x%016lx\n", rbx);
        printf("RCX after patch = 0x%016lx\n", rcx);
        printf("RDX after patch = 0x%016lx\n", rdx);
        printf("RAX after patch = 0x%016lx\n", rax);
        printf("RDI after patch = 0x%016lx\n", rdi);
        return 0;
}

~
~
~

