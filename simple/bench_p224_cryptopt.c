/*
 * bench_p224_cryptopt.c — Benchmark CryptOpt P-224 squaring
 *
 * Build:
 *   nasm -f elf64 -o seed0043918537067620_ratio13401.o seed0043918537067620_ratio13401.asm
 *   gcc -static -O3 -masm=intel -o bench_p224_cryptopt_static bench_p224_cryptopt.c seed0043918537067620_ratio13401.o
 * Run:
 *   sudo taskset -c 0 ./bench_p224_cryptopt_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <sched.h>

/* CryptOpt function: void fiat_p224_square(uint64_t out[4], const uint64_t a[4]) */
extern void fiat_p224_square(uint64_t *out, const uint64_t *a);

static inline uint64_t rdtsc_start(void) {
    uint32_t lo, hi;
    asm volatile("cpuid\n\trdtsc" : "=a"(lo), "=d"(hi) :: "rbx", "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t rdtsc_end(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx", "memory");
    asm volatile("cpuid" ::: "rax", "rbx", "rcx", "rdx", "memory");
    return ((uint64_t)hi << 32) | lo;
}

static void assign_to_core(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

#define BATCH 10000
#define REPS  200

int main(void) {
    printf("=== CryptOpt P-224 squaring benchmark ===\n\n");
    assign_to_core(0);

    uint64_t state[4] = {
        UINT64_C(0xFFFFFFFF00000000), UINT64_C(0xFFFFFFFFFFFFFFFF), 0, 0
    };
    uint64_t tmp[4];
    uint64_t t0, t1, min, sum;

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++)
            fiat_p224_square(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("CryptOpt:  min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    printf("\nDone.\n");
    return 0;
}
