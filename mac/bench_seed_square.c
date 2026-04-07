/*
 * bench_seed_square.c — benchmark the CryptOpt-generated curve25519 square
 *
 * The .asm file uses vmwrite as MAC3, so the 19-triad hook must be installed
 * before running (run mac3_curve25519_static first).
 *
 * Build:
 *   nasm -f elf64 seed0001774706802347_ratio03298.asm -o seed_square.o
 *   gcc -static -g -O0 -masm=intel -march=x86-64-v2 -I include/ \
 *       bench_seed_square.c seed_square.o ../../build/libmicro.a -o bench_seed_square_static
 *
 * Run:
 *   sudo taskset -c 0 ./bench_seed_square_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#define MASK51 0x7FFFFFFFFFFFFULL

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


/* external: CryptOpt-generated assembly */
extern void fiat_curve25519_carry_square(uint64_t *out, const uint64_t *arg1);


/* reference C implementation */
__attribute__((noinline))
static void fe_sq_ref(const uint64_t *a, uint64_t *out) {
        uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
        uint64_t d0 = 2*a0, d1 = 2*a1, d2 = 2*a2, d3 = 2*a3;
        uint64_t r3 = 19*a3, r4 = 19*a4;

        __uint128_t c0 = (__uint128_t)a0*a0 + (__uint128_t)d1*r4 + (__uint128_t)d2*r3;
        __uint128_t c1 = (__uint128_t)d0*a1 + (__uint128_t)r3*a3 + (__uint128_t)d2*r4;
        __uint128_t c2 = (__uint128_t)d0*a2 + (__uint128_t)a1*a1 + (__uint128_t)d3*r4;
        __uint128_t c3 = (__uint128_t)d0*a3 + (__uint128_t)d1*a2 + (__uint128_t)r4*a4;
        __uint128_t c4 = (__uint128_t)d0*a4 + (__uint128_t)d1*a3 + (__uint128_t)a2*a2;

        uint64_t carry;
        carry = (uint64_t)(c0 >> 51); out[0] = (uint64_t)c0 & MASK51;
        c1 += carry;
        carry = (uint64_t)(c1 >> 51); out[1] = (uint64_t)c1 & MASK51;
        c2 += carry;
        carry = (uint64_t)(c2 >> 51); out[2] = (uint64_t)c2 & MASK51;
        c3 += carry;
        carry = (uint64_t)(c3 >> 51); out[3] = (uint64_t)c3 & MASK51;
        c4 += carry;
        carry = (uint64_t)(c4 >> 51); out[4] = (uint64_t)c4 & MASK51;

        out[0] += carry * 19;
        carry = out[0] >> 51; out[0] &= MASK51;
        out[1] += carry;
}


/* inline MAC3 version (from bench_mac3_curve25519.c) for 3-way comparison */
__attribute__((noinline))
static void fe_sq_mac3(const uint64_t *a, uint64_t *out) {
        uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
        uint64_t d0 = 2*a0, d1 = 2*a1, d2 = 2*a2, d3 = 2*a3;
        uint64_t r3 = 19*a3, r4 = 19*a4;
        uint64_t c_lo, c_hi, carry;

        /* c[0] = a0*a0 + d1*r4 + d2*r3 */
        asm volatile(
                "xor rax, rax\n\t"
                "mov r8,  %[_p3y]\n\t"
                "mov rbx, %[_p3x]\n\t"
                "mov rsi, %[_p2x]\n\t"
                "mov rdi, %[_p2y]\n\t"
                "mov rcx, %[_p1x]\n\t"
                "mov rdx, %[_p1y]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [_p1x] "r"(a0),  [_p1y] "r"(a0),
                  [_p2x] "r"(d1),  [_p2y] "r"(r4),
                  [_p3x] "r"(d2),  [_p3y] "r"(r3)
                : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[0] = c_lo & MASK51;

        /* c[1] */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "mov r8,  %[_p3y]\n\t"
                "mov rbx, %[_p3x]\n\t"
                "mov rsi, %[_p2x]\n\t"
                "mov rdi, %[_p2y]\n\t"
                "mov rcx, %[_p1x]\n\t"
                "mov rdx, %[_p1y]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [_p1x] "r"(d0),  [_p1y] "r"(a1),
                  [_p2x] "r"(r3),  [_p2y] "r"(a3),
                  [_p3x] "r"(d2),  [_p3y] "r"(r4)
                : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[1] = c_lo & MASK51;

        /* c[2] */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "mov r8,  %[_p3y]\n\t"
                "mov rbx, %[_p3x]\n\t"
                "mov rsi, %[_p2x]\n\t"
                "mov rdi, %[_p2y]\n\t"
                "mov rcx, %[_p1x]\n\t"
                "mov rdx, %[_p1y]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [_p1x] "r"(d0),  [_p1y] "r"(a2),
                  [_p2x] "r"(a1),  [_p2y] "r"(a1),
                  [_p3x] "r"(d3),  [_p3y] "r"(r4)
                : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[2] = c_lo & MASK51;

        /* c[3] */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "mov r8,  %[_p3y]\n\t"
                "mov rbx, %[_p3x]\n\t"
                "mov rsi, %[_p2x]\n\t"
                "mov rdi, %[_p2y]\n\t"
                "mov rcx, %[_p1x]\n\t"
                "mov rdx, %[_p1y]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [_p1x] "r"(d0),  [_p1y] "r"(a3),
                  [_p2x] "r"(d1),  [_p2y] "r"(a2),
                  [_p3x] "r"(r4),  [_p3y] "r"(a4)
                : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[3] = c_lo & MASK51;

        /* c[4] */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "mov r8,  %[_p3y]\n\t"
                "mov rbx, %[_p3x]\n\t"
                "mov rsi, %[_p2x]\n\t"
                "mov rdi, %[_p2y]\n\t"
                "mov rcx, %[_p1x]\n\t"
                "mov rdx, %[_p1y]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [_p1x] "r"(d0),  [_p1y] "r"(a4),
                  [_p2x] "r"(d1),  [_p2y] "r"(a3),
                  [_p3x] "r"(a2),  [_p3y] "r"(a2)
                : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[4] = c_lo & MASK51;

        out[0] += carry * 19;
        carry = out[0] >> 51;
        out[0] &= MASK51;
        out[1] += carry;
}


#define BATCH 1000
#define REPS  100

int main(void) {
        uint64_t t0, t1;
        uint64_t min, sum;

        uint64_t state_ref[5] = { 0x00062D608F25D51AULL,
                                  0x000412A4B4F6592AULL,
                                  0x00075B7171A4B31DULL,
                                  0x0001FF60527118FEULL,
                                  0x000216936D3CD6E5ULL };
        uint64_t state_mac[5], state_asm[5];
        uint64_t tmp[5];
        memcpy(state_mac, state_ref, sizeof(state_ref));
        memcpy(state_asm, state_ref, sizeof(state_ref));

        printf("================================================================\n");
        printf("  Curve25519 Square: CryptOpt ASM vs MAC3 (5-call) vs Native C\n");
        printf("================================================================\n\n");
        printf("  Batched: %d ops/batch, %d batches\n\n", BATCH, REPS);

        /* -- Native C ------------------------------------------------- */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                memcpy(tmp, state_ref, sizeof(tmp));
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++)
                        fe_sq_ref(tmp, tmp);
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  Native C  (-O0):  min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);
        fe_sq_ref(state_ref, state_ref);

        /* -- Inline MAC3 (5 vmwrite) ---------------------------------- */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                memcpy(tmp, state_mac, sizeof(tmp));
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++)
                        fe_sq_mac3(tmp, tmp);
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  MAC3 (5 call):    min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);
        fe_sq_mac3(state_mac, state_mac);

        /* -- CryptOpt ASM (13 vmwrite) -------------------------------- */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                memcpy(tmp, state_asm, sizeof(tmp));
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++)
                        fiat_curve25519_carry_square(tmp, tmp);
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  CryptOpt ASM:     min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);
        fiat_curve25519_carry_square(state_asm, state_asm);

        /* -- Sanity check --------------------------------------------- */
        printf("\n  Sanity: ref vs MAC3  = %s\n",
               memcmp(state_ref, state_mac, sizeof(state_ref)) == 0 ? "OK" : "MISMATCH");
        printf("  Sanity: ref vs ASM   = %s\n",
               memcmp(state_ref, state_asm, sizeof(state_ref)) == 0 ? "OK" : "MISMATCH");

        /* -- Analysis ------------------------------------------------- */
        printf("\n  Overhead estimate (CryptOpt ASM):\n");
        printf("    13 vmwrite traps x ~5 cycle redirect = ~65 cycles\n");
        printf("    13 single-MAC triads                 = ~65 cycles\n");
        printf("    x86 glue (imul, add/adc, shrd, mov)  = ~40 cycles\n");
        printf("    ----------------------------------------\n");
        printf("    Estimated total:                       ~170 cycles\n\n");

        printf("  vs MAC3 (5-call): 5 traps, each does 3 MACs = 15 MACs in 5 traps\n");
        printf("  vs CryptOpt ASM:  13 traps, each does 1 MAC = 13 MACs in 13 traps\n");
        printf("  Extra trap overhead: 8 x ~5 = ~40 cycles\n");

        return 0;
}
