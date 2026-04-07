#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"


void addshl(void) {
    ucode_t ucode_patch[] = {
        {
            /* uop 0: TMP1 = 0xcafe */
            MOVE_DSZ64_DI(TMP1, 0xcafe),

            /* uop 1: RAX = TMP1 + 0x10 */
            /* ADD_DSZ64_DRI(dst, src, imm) */
            ADD_DSZ64_DRI(RAX, TMP1, 0x10),

            /* uop 2: RAX <<= 4  (shift left immediate) */
            /* SHL_DSZ64_DRI(dst, src, imm) */
            SHL_DSZ64_DRI(RAX, RAX, 4),

            /* sequence word terminator */
            END_SEQWORD
        }
    };

    assign_to_core(0);
    do_fix_IN_patch();
    /* write the patch into ucode RAM at a patch slot */
    patch_ucode(0x7c4c, ucode_patch, ARRAY_SZ(ucode_patch));
    /* hook match-and-patch so rdrand entrypoint now jumps to our patch */
    hook_match_and_patch(0, 0x0428, 0x7c4c);
}
int main() {
    uint64_t rax;
    printf("After patch:");
    addshl();
    asm volatile("rdrand %0" : "=a" (rax));
    printf("a=%lx\n", rax);
    return 0;
}

