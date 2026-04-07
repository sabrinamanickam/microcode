/*
 * bench_mac128.c — Time MAC128 via VMWRITE
 *
 * No microcode patching here. Run mac128_nomovs_static first
 * to install the hook, then run this.
 *
 * Build:  make PROG=bench_mac128
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>


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

/*
 * Batch N ops inside one timed window, repeat REPS times.
 * Report total / N = per-op cost with fencing amortized away.
 */
#define BATCH 1000
#define REPS  100

int main(void) {
        uint64_t t0, t1;
        uint64_t min, sum;
        uint64_t a  = 0x0007FFFFFFFFFFFFULL;
        uint64_t b  = 0x0006000000000000ULL;
        uint64_t b2 = 0x0005000000000000ULL;
        uint64_t b3 = 0x0004000000000000ULL;

        printf("Batched benchmark: %d ops/batch, %d batches\n\n", BATCH, REPS);

        /* ── Single MAC128 ────────────────────────────── */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "xor rax, rax\n\t"
                                "xor r8, r8\n\t"
                                "mov rcx, %[a]\n\t"
                                "mov rdx, %[b]\n\t"
                                "vmwrite rcx, rdx\n\t"
                                : : [a] "r"(a), [b] "r"(b)
                                : "rax", "rcx", "rdx", "r8"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  Single MAC128:  min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);

        /* ── Chain×3 MAC128 ───────────────────────────── */
       /* min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "xor rax, rax\n\t"
                                "xor r8, r8\n\t"
                                "mov rcx, %[a]\n\t"  "mov rdx, %[b]\n\t"
                                "vmwrite rcx, rdx\n\t"
                                "mov rcx, %[a]\n\t"  "mov rdx, %[b2]\n\t"
                                "vmwrite rcx, rdx\n\t"
                                "mov rcx, %[a]\n\t"  "mov rdx, %[b3]\n\t"
                                "vmwrite rcx, rdx\n\t"
                                :
                                : [a] "r"(a), [b] "r"(b), [b2] "r"(b2), [b3] "r"(b3)
                                : "rax", "rcx", "rdx", "r8"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  Chain×3 MAC128: min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles  (per chain, /3 = per MAC)\n",
               min / BATCH, sum / REPS / BATCH);*/

        /* ── Native MUL+ADD/ADC single ────────────────── */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "xor r8, r8\n\t"
                                "xor r9, r9\n\t"
                                "mov rax, %[a]\n\t"
                                "mul %[b]\n\t"
                                "add r9, rax\n\t"
                                "adc r8, rdx\n\t"
                                : : [a] "r"(a), [b] "r"(b)
                                : "rax", "rdx", "r8", "r9"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  Native MUL+ADC: min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);

        /* ── Native chain×3 MUL+ADD/ADC ──────────────── */
        //min = UINT64_MAX; sum = 0;
        /* for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "xor r8, r8\n\t"
                                "xor r9, r9\n\t"
                                "mov rax, %[a]\n\t"  "mul %[b]\n\t"
                                "add r9, rax\n\t"    "adc r8, rdx\n\t"
                                "mov rax, %[a]\n\t"  "mul %[b2]\n\t"
                                "add r9, rax\n\t"    "adc r8, rdx\n\t"
                                "mov rax, %[a]\n\t"  "mul %[b3]\n\t"
                                "add r9, rax\n\t"    "adc r8, rdx\n\t"
                                :
                                : [a] "r"(a), [b] "r"(b), [b2] "r"(b2), [b3] "r"(b3)
                                : "rax", "rdx", "r8", "r9"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  Native chain×3: min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles  (per chain, /3 = per MAC)\n",
               min / BATCH, sum / REPS / BATCH);*/

        return 0;
}
