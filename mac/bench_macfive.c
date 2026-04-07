/*
 * bench_mac5.c — Benchmark batched 5×MAC128 via single VMWRITE
 *
 * Run AFTER mac5_ldzx installs the batch patch on core 0.
 * Uses taskset -c 0 to run on the patched core.
 *
 * Compares:
 *   1. Batched MAC5:   1 vmwrite, 5 MACs from memory
 *   2. 5× MAC128:      5 vmwrites, 1 MAC each (needs mac128 hook!)
 *   3. Native 5×MUL:   5 × (MUL + ADD + ADC), no microcode
 *
 * NOTE: Test 2 (5× MAC128) only valid if the single-MAC hook is
 * installed. When mac5_ldzx is installed, test 2 will call the
 * batch patch 5 times which is wrong — so it's #ifdef'd out.
 * Uncomment and re-install mac128_nomovs to compare.
 *
 * Build: make PROG=bench_mac5
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

#define BATCH 1000
#define REPS  100

int main(void) {
        uint64_t t0, t1;
        uint64_t min, sum;

        /* Operand pairs for batched test — sits in L1 */
        uint64_t pairs[10] = {
                0x0007FFFFFFFFFFFFULL, 0x0006000000000000ULL,
                0x0005FFFFFFFFFFFFULL, 0x0004000000000000ULL,
                0x0003FFFFFFFFFFFFULL, 0x0002000000000000ULL,
                0x0001FFFFFFFFFFFFULL, 0x0001000000000000ULL,
                0x0000FFFFFFFFFFFFULL, 0x0000800000000000ULL
        };

        /* Individual operands for native test */
        uint64_t x0 = pairs[0], y0 = pairs[1];
        uint64_t x1 = pairs[2], y1 = pairs[3];
        uint64_t x2 = pairs[4], y2 = pairs[5];
        uint64_t x3 = pairs[6], y3 = pairs[7];
        uint64_t x4 = pairs[8], y4 = pairs[9];

        printf("Batched benchmark: %d ops/batch, %d batches\n\n", BATCH, REPS);

        /* ── Batched MAC5: 1 vmwrite, 5 MACs ─────────── */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "xor rax, rax\n\t"
                                "xor r8, r8\n\t"
                                "lea rcx, [%[p]]\n\t"
                                "xor rdx, rdx\n\t"
                                "vmwrite rcx, rdx\n\t"
                                :
                                : [p] "m"(pairs[0])
                                : "rax", "rcx", "rdx", "r8", "memory"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  Batch MAC5:     min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles  (1 vmwrite, 5 MACs)\n",
               min / BATCH, sum / REPS / BATCH);

        /* ── Native 5× MUL+ADD+ADC ───────────────────── */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "xor r8, r8\n\t"
                                "xor r9, r9\n\t"

                                "mov rax, %[x0]\n\t"  "mul %[y0]\n\t"
                                "add r9, rax\n\t"      "adc r8, rdx\n\t"

                                "mov rax, %[x1]\n\t"  "mul %[y1]\n\t"
                                "add r9, rax\n\t"      "adc r8, rdx\n\t"

                                "mov rax, %[x2]\n\t"  "mul %[y2]\n\t"
                                "add r9, rax\n\t"      "adc r8, rdx\n\t"

                                "mov rax, %[x3]\n\t"  "mul %[y3]\n\t"
                                "add r9, rax\n\t"      "adc r8, rdx\n\t"

                                "mov rax, %[x4]\n\t"  "mul %[y4]\n\t"
                                "add r9, rax\n\t"      "adc r8, rdx\n\t"
                                :
                                : [x0] "r"(x0), [y0] "r"(y0),
                                  [x1] "r"(x1), [y1] "r"(y1),
                                  [x2] "r"(x2), [y2] "r"(y2),
                                  [x3] "r"(x3), [y3] "r"(y3),
                                  [x4] "r"(x4), [y4] "r"(y4)
                                : "rax", "rdx", "r8", "r9"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  Native 5×MUL:   min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles  (5 MUL+ADC, register ops)\n",
               min / BATCH, sum / REPS / BATCH);

        /* ── Native 5× MUL+ADD+ADC from memory ──────── */
        /* Fairer comparison: native code also loads from the array */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "xor r8, r8\n\t"
                                "xor r9, r9\n\t"
                                "lea r10, [%[p]]\n\t"

                                "mov rax, [r10]\n\t"      "mul QWORD PTR [r10+8]\n\t"
                                "add r9, rax\n\t"          "adc r8, rdx\n\t"

                                "mov rax, [r10+16]\n\t"   "mul QWORD PTR [r10+24]\n\t"
                                "add r9, rax\n\t"          "adc r8, rdx\n\t"

                                "mov rax, [r10+32]\n\t"   "mul QWORD PTR [r10+40]\n\t"
                                "add r9, rax\n\t"          "adc r8, rdx\n\t"

                                "mov rax, [r10+48]\n\t"   "mul QWORD PTR [r10+56]\n\t"
                                "add r9, rax\n\t"          "adc r8, rdx\n\t"

                                "mov rax, [r10+64]\n\t"   "mul QWORD PTR [r10+72]\n\t"
                                "add r9, rax\n\t"          "adc r8, rdx\n\t"
                                :
                                : [p] "m"(pairs[0])
                                : "rax", "rdx", "r8", "r9", "r10", "memory"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  Native 5×MUL(mem): min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles  (loads from array)\n",
               min / BATCH, sum / REPS / BATCH);

        return 0;
}/*
 * bench_mac5.c — Benchmark batched 5×MAC128 via single VMWRITE
 *
 * Run AFTER mac5_ldzx installs the batch patch on core 0.
 * Uses taskset -c 0 to run on the patched core.
 *
 * Compares:
 *   1. Batched MAC5:   1 vmwrite, 5 MACs from memory
 *   2. 5× MAC128:      5 vmwrites, 1 MAC each (needs mac128 hook!)
 *   3. Native 5×MUL:   5 × (MUL + ADD + ADC), no microcode
 *
 * NOTE: Test 2 (5× MAC128) only valid if the single-MAC hook is
 * installed. When mac5_ldzx is installed, test 2 will call the
 * batch patch 5 times which is wrong — so it's #ifdef'd out.
 * Uncomment and re-install mac128_nomovs to compare.
 *
 * Build: make PROG=bench_mac5
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

#define BATCH 1000
#define REPS  100

int main(void) {
        uint64_t t0, t1;
        uint64_t min, sum;

        /* Operand pairs for batched test — sits in L1 */
        uint64_t pairs[10] = {
                0x0007FFFFFFFFFFFFULL, 0x0006000000000000ULL,
                0x0005FFFFFFFFFFFFULL, 0x0004000000000000ULL,
                0x0003FFFFFFFFFFFFULL, 0x0002000000000000ULL,
                0x0001FFFFFFFFFFFFULL, 0x0001000000000000ULL,
                0x0000FFFFFFFFFFFFULL, 0x0000800000000000ULL
        };

        /* Individual operands for native test */
        uint64_t x0 = pairs[0], y0 = pairs[1];
        uint64_t x1 = pairs[2], y1 = pairs[3];
        uint64_t x2 = pairs[4], y2 = pairs[5];
        uint64_t x3 = pairs[6], y3 = pairs[7];
        uint64_t x4 = pairs[8], y4 = pairs[9];

        printf("Batched benchmark: %d ops/batch, %d batches\n\n", BATCH, REPS);

        /* ── Batched MAC5: 1 vmwrite, 5 MACs ─────────── */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "xor rax, rax\n\t"
                                "xor r8, r8\n\t"
                                "lea rcx, [%[p]]\n\t"
                                "xor rdx, rdx\n\t"
                                "vmwrite rcx, rdx\n\t"
                                :
                                : [p] "m"(pairs[0])
                                : "rax", "rcx", "rdx", "r8", "memory"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  Batch MAC5:     min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles  (1 vmwrite, 5 MACs)\n",
               min / BATCH, sum / REPS / BATCH);

        /* ── Native 5× MUL+ADD+ADC ───────────────────── */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "xor r8, r8\n\t"
                                "xor r9, r9\n\t"

                                "mov rax, %[x0]\n\t"  "mul %[y0]\n\t"
                                "add r9, rax\n\t"      "adc r8, rdx\n\t"

                                "mov rax, %[x1]\n\t"  "mul %[y1]\n\t"
                                "add r9, rax\n\t"      "adc r8, rdx\n\t"

                                "mov rax, %[x2]\n\t"  "mul %[y2]\n\t"
                                "add r9, rax\n\t"      "adc r8, rdx\n\t"

                                "mov rax, %[x3]\n\t"  "mul %[y3]\n\t"
                                "add r9, rax\n\t"      "adc r8, rdx\n\t"

                                "mov rax, %[x4]\n\t"  "mul %[y4]\n\t"
                                "add r9, rax\n\t"      "adc r8, rdx\n\t"
                                :
                                : [x0] "r"(x0), [y0] "r"(y0),
                                  [x1] "r"(x1), [y1] "r"(y1),
                                  [x2] "r"(x2), [y2] "r"(y2),
                                  [x3] "r"(x3), [y3] "r"(y3),
                                  [x4] "r"(x4), [y4] "r"(y4)
                                : "rax", "rdx", "r8", "r9"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  Native 5×MUL:   min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles  (5 MUL+ADC, register ops)\n",
               min / BATCH, sum / REPS / BATCH);

        /* ── Native 5× MUL+ADD+ADC from memory ──────── */
        /* Fairer comparison: native code also loads from the array */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        asm volatile(
                                "xor r8, r8\n\t"
                                "xor r9, r9\n\t"
                                "lea r10, [%[p]]\n\t"

                                "mov rax, [r10]\n\t"      "mul QWORD PTR [r10+8]\n\t"
                                "add r9, rax\n\t"          "adc r8, rdx\n\t"

                                "mov rax, [r10+16]\n\t"   "mul QWORD PTR [r10+24]\n\t"
                                "add r9, rax\n\t"          "adc r8, rdx\n\t"

                                "mov rax, [r10+32]\n\t"   "mul QWORD PTR [r10+40]\n\t"
                                "add r9, rax\n\t"          "adc r8, rdx\n\t"

                                "mov rax, [r10+48]\n\t"   "mul QWORD PTR [r10+56]\n\t"
                                "add r9, rax\n\t"          "adc r8, rdx\n\t"

                                "mov rax, [r10+64]\n\t"   "mul QWORD PTR [r10+72]\n\t"
                                "add r9, rax\n\t"          "adc r8, rdx\n\t"
                                :
                                : [p] "m"(pairs[0])
                                : "rax", "rdx", "r8", "r9", "r10", "memory"
                        );
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  Native 5×MUL(mem): min/op %3" PRIu64 "  avg/op %3" PRIu64 " cycles  (loads from array)\n",
               min / BATCH, sum / REPS / BATCH);

        return 0;
}
