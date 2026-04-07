/*
 * mul_bench.c — Benchmark 5×MUL via VMWRITE
 *
 * No patching. Run multest_static first.
 *
 * Build:  make PROG=mul_bench
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#define BATCH 1000
#define REPS  100

static inline uint64_t rdtsc_start(void) {
        uint32_t lo, hi;
        asm volatile(
                "cpuid\n\t"
                "rdtsc\n\t"
                : "=a"(lo), "=d"(hi)
                :
                : "rbx", "rcx"
        );
        return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtsc_end(void) {
        uint32_t lo, hi;
        asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
        asm volatile("cpuid" ::: "rax", "rbx", "rcx", "rdx");
        return ((uint64_t)hi << 32) | lo;
}

int main(void) {
        uint64_t t0, t1;
        uint64_t min, sum;
        uint64_t a = 0x0007FFFFFFFFFFFFULL;
        uint64_t b = 0x0006000000000000ULL;

        printf("Batched benchmark: %d ops/batch, %d batches\n\n", BATCH, REPS);

        /* ── 1 vmwrite = 5 MULs in microcode ─────────── */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "mov rcx, %[a]\n\t"
                                "mov rdx, %[b]\n\t"
                                "vmwrite rcx, rdx\n\t"
                                : : [a] "r"(a), [b] "r"(b)
                                : "rax", "rbx", "rcx", "rdx"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  1 vmwrite (5×MUL, 11 tri): min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);

        /* ── 5× native MUL ───────────────────────────── */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "mov rax, %[a]\n\t"  "mul %[b]\n\t"
                                "mov rax, %[a]\n\t"  "mul %[b]\n\t"
                                "mov rax, %[a]\n\t"  "mul %[b]\n\t"
                                "mov rax, %[a]\n\t"  "mul %[b]\n\t"
                                "mov rax, %[a]\n\t"  "mul %[b]\n\t"
                                : : [a] "r"(a), [b] "r"(b)
                                : "rax", "rdx"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  5× native MUL:             min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);

        return 0;
}
