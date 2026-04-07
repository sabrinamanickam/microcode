#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#define ITERATIONS 100

static inline uint64_t rdtsc_start(void)
{
    unsigned cycles_low, cycles_high;
    asm volatile (
        "cpuid\n\t"
        "rdtsc\n\t"
        : "=a" (cycles_low), "=d" (cycles_high)
        : "a"(0)
        : "%rbx", "%rcx");
    return ((uint64_t)cycles_high << 32) | cycles_low;
}

static inline uint64_t rdtsc_end(void)
{
    unsigned cycles_low, cycles_high;
    asm volatile (
        "rdtscp\n\t"
        "mov %%eax, %0\n\t"
        "mov %%edx, %1\n\t"
        "cpuid\n\t"
        : "=r" (cycles_low), "=r" (cycles_high)
        :
        : "%rax", "%rbx", "%rcx", "%rdx");
    return ((uint64_t)cycles_high << 32) | cycles_low;
}

int main(void)
{
    uint64_t start, end;
    uint64_t rax, rbx, rcx, rdx;
    volatile uint64_t sink;

    start = rdtsc_start();

    for (int i = 0; i < ITERATIONS; i++) {
        asm volatile(
            "movq $5, %%rcx\n\t"
            "movq $5, %%rbx\n\t"
            "movq $0, %%rdx\n\t"
            "movq $0, %%rax\n\t"
            "vmwrite %%rbx, %%rcx\n\t"
            : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx)
            :
            : 
        );
        sink = rax;
    }

    end = rdtsc_end();

    uint64_t total_cycles = end - start;

    printf("RAX = 0x%016" PRIx64 "\n", rax);
    printf("RBX = 0x%016" PRIx64 "\n", rbx);
    printf("RCX = 0x%016" PRIx64 "\n", rcx);
    printf("RDX = 0x%016" PRIx64 "\n", rdx);
    printf("Total cycles: %" PRIu64 "\n", total_cycles);
    printf("Cycles per iteration: %.2f\n",
           (double)total_cycles / ITERATIONS);

    return 0;
}
