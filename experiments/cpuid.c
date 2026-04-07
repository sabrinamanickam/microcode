


#define _GNU_SOURCE 
#include <stdio.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

void hook_cpuid(void){
	const uint16_t PATCH_ADDR = 0x7c44;
	const uint16_t CPUID_XLAT = 0x0be2;

	ucode_t ucode_patch[] = {{
		MOVE_DSZ64_DI(TMP1, 0x2345),
		MOVE_DSZ64_DI(RBX, 0xaead),
		MOVE_DSZ64_DR(RAX, TMP1),
		END_SEQWORD
	}};
        assign_to_core(0);
	do_fix_IN_patch();
	patch_ucode(PATCH_ADDR, ucode_patch, ARRAY_SZ(ucode_patch));
	hook_match_and_patch(0,CPUID_XLAT, PATCH_ADDR);
}

int main(void)
{
	hook_cpuid();
	uint64_t ra, rb, rc, rd;
	asm("cpuid" : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd) : "0"(0), "2"(0));
	printf("CPUID -> RAX=%#lx RBX=%#lx RCX=%#lx RDX=%#lx\n", ra, rb, rc, rd);
}
