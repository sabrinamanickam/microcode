
#define _GNU_SOURCE 
#include <stdio.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"


void yolo(void){        // Build an array of one microcode triad (3 uops+ seq word)
        ucode_t ucode_patch[]={
                {
                        MOVE_DSZ64_DI(TMP1,0x7878),
                        MOVE_DSZ64_DI(RBX, 0xbead),
                        MOVE_DSZ64_DR(RAX, TMP1),
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        //Applies a speacial "IN"-instruction fix by hooking ucode at U58BA -> U017A
        patch_ucode(0x7da0,ucode_patch,ARRAY_SZ(ucode_patch));
        //Writes the patch into ucode RAM at addresses starting U7AD0
        hook_match_and_patch(0, 0x0788, 0x7da0);
        // Sets match and patch slot #0 so that whenever the CPU would execute the ucode at U0740, it instead jumps to U7DA0
}



int main(){                                                                                                                                                                          
        uint64_t rax, rbx;                                                                                                                                                           
        yolo();                                                                                                                                                                      
        asm("rdtscp" : "=a" (rax), "=b" (rbx));                                                                                                                                    
        //EXECute a SYSEXITQ to trigger the oatched oatch; capture the resulting                                                                                                     
        //RAX/RBX outputs back into C variables                                                                                                                                     
        printf("a=%lx b=%lx\n", rax, rbx);                                                                                                                                           
}        
