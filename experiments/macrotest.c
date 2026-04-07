#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MY_IMM 0x01UL

void install_vmread_patch(uint64_t imm ) {
    ucode_t ucode_patch[] = {
        {
          OR_DSZ64_DRM(RAX, RAX, imm), 
          NOP,
	  NOP,
          END_SEQWORD
        }
    };

    assign_to_core(0);
    do_fix_IN_patch();

    /* write patch to some free slot, e.g. 0x7c4c (same as your example) */
    patch_ucode(0x7da0, ucode_patch, ARRAY_SZ(ucode_patch));

    /* hook: vmwrite r64,r64 translator id is 0x0cd8 */
    hook_match_and_patch(0 /*core*/, 0x0428 /*vmwrite_r64_r64_xlat*/, 0x7da0 /*slot*/);
}
int main(void) {

    uint64_t rax; uint64_t rbx; uint64_t rdx; uint64_t rcx; uint64_t rdi;
    //uint64_t a = 0x0002ULL;
    //uint64_t b = 0x0006ULL;

    printf("After patch:\n");
    for (int i=0; i<128; i++)
    {
	     uint64_t imm = i;
	     install_vmread_patch(imm);
    
             asm volatile( "movq rax, 0\n;"
                           "rdrand %%rax\n\t"
                  : "=a"(rax), "=b"(rbx), "=c"(rcx) ,"=d"(rdx), "=D"(rdi)
                );                                                                                                                                                                                                                                                                                                   
                                                                                                                                                                                                                                                                                                                      
    printf("imm = 0x%016lx; RAX after patch = 0x%016lx\n",imm, rax);
    sleep(1);
    /*printf("RDX after patch = 0x%016lx\n", rdx);                                                                                                                                                                                                                                                                      
    printf("RBX after patch = 0x%016lx\n", rbx);                                                                                                                                                                                                                                                                      
    printf("RCX after patch = 0x%016lx\n", rcx);*/
    //}
}
    return 0;                                                                                                                                                                                                                                                                                                         
}
