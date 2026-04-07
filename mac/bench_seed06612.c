/*
 * bench_seed06612.c — benchmark seed2583118490413054_ratio06612.asm
 *
 * Compares CryptOpt-generated ASM (5 vmwrite MAC3) vs bench_3mac's
 * hand-written fe_sq_mac3x128 vs native C reference.
 *
 * Build:
 *   nasm -f elf64 seed2583118490413054_ratio06612.asm -o seed06612.o
 *   gcc -static -g -O0 -masm=intel -march=x86-64-v2 -I include/ \
 *       bench_seed06612.c seed06612.o ../../build/libmicro.a -o bench_seed06612_static
 *
 * Run:
 *   sudo taskset -c 0 ./bench_seed06612_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL


/* ══════════════════════════════════════════════════════════════════
 *  MAC3x128 patch — same 9-triad hook as bench_3mac.c
 * ══════════════════════════════════════════════════════════════════ */
static void install_mac3x128(void) {
        ucode_t mac3x128_patch[] = {
                /* T0: save acc_lo, multiply pair 1 (hi->RCX, lo->RDX) */
                {
                        ZEROEXT_DSZ64_DR(TMP0, RAX),
                        MUL_DSZ64_DRR(RCX, RCX, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1: acc_lo += p1_lo (RDX), capture carry1 */
                {
                        ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                        SETCC_CONDB_DR(TMP3, TMP2),
                        NOP,
                        NOP_SEQWORD
                },
                /* T2: fold carry1 into p1_hi, multiply pair 2 */
                {
                        ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                        MUL_DSZ64_DRR(RCX, R9, R10),
                        NOP,
                        NOP_SEQWORD
                },
                /* T3: acc_lo += p2_lo, capture carry2 */
                {
                        ADD_DSZ64_DRR(TMP0, TMP2, R10),
                        SETCC_CONDB_DR(TMP3, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                /* T4: fold carry2 into p2_hi, multiply pair 3 */
                {
                        ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                        MUL_DSZ64_DRR(RCX, R11, R14),
                        NOP,
                        NOP_SEQWORD
                },
                /* T5: acc_lo += p3_lo, capture carry3 */
                {
                        ADD_DSZ64_DRR(TMP2, TMP0, R14),
                        SETCC_CONDB_DR(TMP3, TMP2),
                        NOP,
                        NOP_SEQWORD
                },
                /* T6: finalize acc_lo, fold carry3 into p3_hi */
                {
                        ZEROEXT_DSZ64_DR(RAX, TMP2),
                        ADD_DSZ64_DRR(TMP8, RCX, TMP3),
                        NOP,
                        NOP_SEQWORD
                },
                /* T7: R8 += (p1_hi+c1), combine remaining hi terms */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP6),
                        ADD_DSZ64_DRR(TMP0, TMP7, TMP8),
                        NOP,
                        NOP_SEQWORD
                },
                /* T8: R8 += remaining, done */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP0),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac3x128_patch, ARRAY_SZ(mac3x128_patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


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


/* ══════════════════════════════════════════════════════════════════
 *  CryptOpt-generated ASM (seed2583118490413054, ratio 0.6612)
 *  Signature: fiat_curve25519_carry_square(uint64_t *out, const uint64_t *arg1)
 * ══════════════════════════════════════════════════════════════════ */
extern void fiat_curve25519_carry_square(uint64_t *out, const uint64_t *arg1);


/* ══════════════════════════════════════════════════════════════════
 *  Native C reference
 * ══════════════════════════════════════════════════════════════════ */
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


/* ══════════════════════════════════════════════════════════════════
 *  Hand-written MAC3 (from bench_3mac.c) for comparison
 * ══════════════════════════════════════════════════════════════════ */
__attribute__((noinline))
static void fe_sq_mac3x128(const uint64_t *a, uint64_t *out) {
        asm volatile(
                "sub rsp, 24\n\t"
                "mov rax, [%[a]+8]\n\t"
                "mov [rsp],    rax\n\t"
                "mov rax, [%[a]+16]\n\t"
                "mov [rsp+8],  rax\n\t"
                "mov rax, [%[a]+24]\n\t"
                "mov [rsp+16], rax\n\t"

                "mov r12,  [%[a]]\n\t"
                "lea r12,  [r12+r12]\n\t"
                "mov r13,  [rsp]\n\t"
                "lea r13,  [r13+r13]\n\t"
                "mov rbx,  [%[a]+32]\n\t"
                "imul r15, rbx, 19\n\t"

                /* c[0] = a0*a0 + d1*r4 + d2*r3 */
                "xor eax, eax\n\t"
                "xor r8d, r8d\n\t"
                "mov rcx,  [%[a]]\n\t"
                "mov rdx,  rcx\n\t"
                "mov r9,   r13\n\t"
                "mov r10,  r15\n\t"
                "mov r11,  [rsp+8]\n\t"
                "lea r11,  [r11+r11]\n\t"
                "mov r14,  [rsp+16]\n\t"
                "imul r14, r14, 19\n\t"
                "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]], rax\n\t"

                /* c[1] = carry + d0*a1 + r3*a3 + d2*r4 */
                "mov rax, r14\n\t"
                "xor r8d, r8d\n\t"
                "mov rcx,  r12\n\t"
                "mov rdx,  [rsp]\n\t"
                "mov r10,  [rsp+16]\n\t"
                "mov r9,   r10\n\t"
                "imul r9,  r9, 19\n\t"
                "mov r11,  [rsp+8]\n\t"
                "lea r11,  [r11+r11]\n\t"
                "mov r14,  r15\n\t"
                "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+8], rax\n\t"

                /* c[2] = carry + d0*a2 + a1*a1 + d3*r4 */
                "mov rax, r14\n\t"
                "xor r8d, r8d\n\t"
                "mov rcx,  r12\n\t"
                "mov rdx,  [rsp+8]\n\t"
                "mov r9,   [rsp]\n\t"
                "mov r10,  r9\n\t"
                "mov r11,  [rsp+16]\n\t"
                "lea r11,  [r11+r11]\n\t"
                "mov r14,  r15\n\t"
                "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+16], rax\n\t"

                /* c[3] = carry + d0*a3 + d1*a2 + r4*a4 */
                "mov rax, r14\n\t"
                "xor r8d, r8d\n\t"
                "mov rcx,  r12\n\t"
                "mov rdx,  [rsp+16]\n\t"
                "mov r9,   r13\n\t"
                "mov r10,  [rsp+8]\n\t"
                "mov r11,  r15\n\t"
                "mov r14,  rbx\n\t"
                "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+24], rax\n\t"

                /* c[4] = carry + d0*a4 + d1*a3 + a2*a2 */
                "mov rax, r14\n\t"
                "xor r8d, r8d\n\t"
                "mov rcx,  r12\n\t"
                "mov rdx,  rbx\n\t"
                "mov r9,   r13\n\t"
                "mov r10,  [rsp+16]\n\t"
                "mov r11,  [rsp+8]\n\t"
                "mov r14,  r11\n\t"
                "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+32], rax\n\t"

                "add rsp, 24\n\t"
                "imul r14, r14, 19\n\t"
                "add r14, [%[out]]\n\t"
                "mov rax, r14\n\t"
                "shr rax, 51\n\t"
                "shl r14, 13\n\t"
                "shr r14, 13\n\t"
                "mov [%[out]], r14\n\t"
                "add [%[out]+8], rax\n\t"

                :
                : [a] "r"(a), [out] "r"(out)
                : "rax", "rbx", "rcx", "rdx", "r8", "r9", "r10",
                  "r11", "r12", "r13", "r14", "r15", "memory"
        );
}


/* ══════════════════════════════════════════════════════════════════
 *  BENCHMARK
 * ══════════════════════════════════════════════════════════════════ */

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

        printf("Installing MAC3x128 patch...\n");
        install_mac3x128();

        printf("================================================================\n");
        printf("  seed2583118490413054_ratio06612 vs bench_3mac vs Native C\n");
        printf("================================================================\n\n");
        printf("  %d ops/batch, %d batches\n\n", BATCH, REPS);

        /* ── Native C ─────────────────────────────────────────────── */
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
        printf("  Native C (-O0):     min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);
        fe_sq_ref(state_ref, state_ref);

        /* ── Hand-written MAC3 (bench_3mac style) ─────────────────── */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                memcpy(tmp, state_mac, sizeof(tmp));
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++)
                        fe_sq_mac3x128(tmp, tmp);
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  bench_3mac (hand):  min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);
        fe_sq_mac3x128(state_mac, state_mac);

        /* ── CryptOpt seed ASM (5 vmwrite MAC3) ──────────────────── */
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
        printf("  CryptOpt seed06612: min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);
        fiat_curve25519_carry_square(state_asm, state_asm);

        /* ── Correctness ──────────────────────────────────────────── */
        printf("\n  ref vs bench_3mac  = %s\n",
               memcmp(state_ref, state_mac, sizeof(state_ref)) == 0 ? "OK" : "MISMATCH");
        printf("  ref vs seed06612   = %s\n",
               memcmp(state_ref, state_asm, sizeof(state_ref)) == 0 ? "OK" : "MISMATCH");

        return 0;
}
