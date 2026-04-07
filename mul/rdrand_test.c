
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
int main(void)
{ 
	uint64_t rax; uint64_t rbx; uint64_t rdx; uint64_t rcx; uint64_t rdi; 
	printf("After patch:\n"); 

	asm volatile(
		       "rdrand %%rbx\n\t" 
			: "=a"(rax), "=b"(rbx), "=c"(rcx) ,"=d"(rdx), "=D"(rdi) /* outputs: RAX, RDX after the patch */ /* inputs: a -> RAX, b -> RDX */ 
			); 
	/*printf("RBX after patch = 0x%016lx (%lu)\n", rbx); 
	printf("RCX after patch = 0x%016lx (%lu)\n", rcx); 
	printf("RDX after patch = 0x%016lx (%lu)\n", rdx); 
	printf("RAX after patch = 0x%016lx (%lu)\n", rax);
	printf("RDI after patch = 0x%016lx (%lu)\n", rdi); */
	printf("RBX = 0x%016" PRIx64 " (%" PRIu64 ")\n", rbx, rbx);
	printf("RCX = 0x%016" PRIx64 " (%" PRIu64 ")\n", rcx, rcx);
	printf("RDX = 0x%016" PRIx64 " (%" PRIu64 ")\n", rdx, rdx);
	printf("RAX = 0x%016" PRIx64 " (%" PRIu64 ")\n", rax, rax);
	return 0; 
}

