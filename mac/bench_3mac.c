/*
 * bench_3mac.c — Curve25519 square benchmark: MAC3x128 (9 triads) vs native C
 *
 * Optimized version of bench_twohooks.c:
 *   - 3 multiply-accumulates per vmwrite (was 1)
 *   - 5 vmwrite calls per fe_sq (was 15)
 *   - 9 triads per call, ~9 cycles body + ~5 redirect = ~14 cycles
 *   - Total MAC cost: ~70 cycles (was ~135)
 *
 * Build:  make PROG=bench_3mac
 * Run:    sudo taskset -c 0 ./bench_3mac_static
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
 *  MAC3x128 patch install — 9 triads
 *
 *  vmwrite rcx, rdx  (hooked at 0x0cd8 → patch 0x7c00)
 *  acc_hi:acc_lo += (RCX × RDX) + (R9 × R10) + (R11 × R14)
 *
 *  INPUT:  RCX,RDX=pair1  R9,R10=pair2  R11,R14=pair3
 *          RAX=acc_lo, R8=acc_hi
 *  OUTPUT: RAX=acc_lo, R8=acc_hi
 *  CLOBBERS: RCX, RDX, R9, R10, R11, R14
 *
 *  Microcode schedule (9 triads):
 *
 *   MUL convention: hi → dst, lo → src1.
 *   (Original MAC128 uses MUL(R64SRC,R64SRC,R64DST) where
 *    R64SRC→RDX, R64DST→RCX for vmwrite, so hi→RDX, lo→RCX.
 *    With explicit regs MUL(RCX,RCX,RDX): hi→RCX, lo→RDX.)
 *
 *   All 3 MULs write hi→RCX, lo→src1 (RDX, R10, R14).
 *   Odd triads (T1,T3,T5) add lo; even triads (T2,T4,T6) fold hi+carry.
 *   T2/T4: ADD reads old RCX (hi) while co-issued MUL writes new RCX.
 *
 *   T0: ZEROEXT TMP0, RAX          | MUL RCX, RCX, RDX        | NOP
 *       ; TMP0=acc_lo  RCX=p1_hi  RDX=p1_lo
 *
 *   T1: ADD TMP2, TMP0, RDX        | SETCC_CONDB TMP3, TMP2   | NOP
 *       ; TMP2=acc_lo+p1_lo  TMP3=carry1
 *
 *   T2: ADD TMP6, RCX, TMP3        | MUL RCX, R9, R10         | NOP
 *       ; TMP6=p1_hi+carry1  RCX=p2_hi  R10=p2_lo
 *
 *   T3: ADD TMP0, TMP2, R10        | SETCC_CONDB TMP3, TMP0   | NOP
 *       ; TMP0=acc+p1+p2 (lo)  TMP3=carry2
 *
 *   T4: ADD TMP7, RCX, TMP3        | MUL RCX, R11, R14        | NOP
 *       ; TMP7=p2_hi+carry2  RCX=p3_hi  R14=p3_lo
 *
 *   T5: ADD TMP2, TMP0, R14        | SETCC_CONDB TMP3, TMP2   | NOP
 *       ; TMP2=final_acc_lo  TMP3=carry3
 *
 *   T6: ZEROEXT RAX, TMP2          | ADD TMP8, RCX, TMP3      | NOP
 *       ; RAX=final_acc_lo  TMP8=p3_hi+carry3
 *
 *   T7: ADD R8, R8, TMP6           | ADD TMP0, TMP7, TMP8     | NOP
 *       ; R8 += (p1_hi+carry1)  TMP0=(p2_hi+carry2)+(p3_hi+carry3)
 *
 *   T8: ADD R8, R8, TMP0           | NOP                      | END
 *       ; R8 += remaining hi bits
 * ══════════════════════════════════════════════════════════════════ */
static void install_mac3x128(void) {
        ucode_t mac3x128_patch[] = {
                /* T0: save acc_lo, multiply pair 1 (hi→RCX, lo→RDX) */
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
                /* T2: fold carry1 into p1_hi (RCX), multiply pair 2 (hi→RCX, lo→R10) */
                {
                        ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                        MUL_DSZ64_DRR(RCX, R9, R10),
                        NOP,
                        NOP_SEQWORD
                },
                /* T3: acc_lo += p2_lo (R10), capture carry2 */
                {
                        ADD_DSZ64_DRR(TMP0, TMP2, R10),
                        SETCC_CONDB_DR(TMP3, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                /* T4: fold carry2 into p2_hi (RCX), multiply pair 3 (hi→RCX, lo→R14) */
                {
                        ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                        MUL_DSZ64_DRR(RCX, R11, R14),
                        NOP,
                        NOP_SEQWORD
                },
                /* T5: acc_lo += p3_lo (R14), capture carry3 */
                {
                        ADD_DSZ64_DRR(TMP2, TMP0, R14),
                        SETCC_CONDB_DR(TMP3, TMP2),
                        NOP,
                        NOP_SEQWORD
                },
                /* T6: finalize acc_lo, fold carry3 into p3_hi (RCX) */
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
                /* T8: R8 += (p2_hi+c2)+(p3_hi+c3), done */
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
 *  REFERENCE C — native field square
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
 *  MAC3x128 CURVE25519 SQUARE — 5 vmwrite calls (was 15)
 *
 *  Register map:
 *    Persistent across vmwrites:
 *      r12 = d0 (2·a0)     r13 = d1 (2·a1)
 *      r15 = r4 (19·a4)    rbx = a4
 *      %[a], %[out] = pointers (compiler-assigned)
 *
 *    Per-vmwrite operand registers (clobbered by microcode):
 *      rcx, rdx  = pair 1
 *      r9,  r10  = pair 2
 *      r11, r14  = pair 3
 *
 *    Accumulator (per-limb):
 *      rax = acc_lo,  r8 = acc_hi
 * ══════════════════════════════════════════════════════════════════ */
__attribute__((noinline))
static void fe_sq_mac3x128(const uint64_t *a, uint64_t *out) {
        asm volatile(
                /* ── Precompute persistent values ──────────────────── */
                /* Save a1,a2,a3 to stack — when a==out, stores to
                 * [out+N] corrupt later reads from [a+N]. */
                "sub rsp, 24\n\t"
                "mov rax, [%[a]+8]\n\t"
                "mov [rsp],    rax\n\t"             /* [rsp+0]  = a1 */
                "mov rax, [%[a]+16]\n\t"
                "mov [rsp+8],  rax\n\t"             /* [rsp+8]  = a2 */
                "mov rax, [%[a]+24]\n\t"
                "mov [rsp+16], rax\n\t"             /* [rsp+16] = a3 */

                "mov r12,  [%[a]]\n\t"
                "lea r12,  [r12+r12]\n\t"           /* r12 = d0 = 2·a0 */
                "mov r13,  [rsp]\n\t"
                "lea r13,  [r13+r13]\n\t"           /* r13 = d1 = 2·a1 */
                "mov rbx,  [%[a]+32]\n\t"           /* rbx = a4 */
                "imul r15, rbx, 19\n\t"             /* r15 = r4 = 19·a4 */

                /* ── c[0] = a0·a0 + d1·r4 + d2·r3 ─────────────────
                 *    pair1: a0 × a0
                 *    pair2: d1 × r4  (precomputed)
                 *    pair3: 2·a2 × 19·a3 */
                "xor eax, eax\n\t"
                "xor r8d, r8d\n\t"
                "mov rcx,  [%[a]]\n\t"
                "mov rdx,  rcx\n\t"
                "mov r9,   r13\n\t"
                "mov r10,  r15\n\t"
                "mov r11,  [rsp+8]\n\t"
                "lea r11,  [r11+r11]\n\t"           /* r11 = d2 */
                "mov r14,  [rsp+16]\n\t"
                "imul r14, r14, 19\n\t"             /* r14 = r3 = 19·a3 */
                "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]], rax\n\t"

                /* ── c[1] = carry + d0·a1 + r3·a3 + d2·r4 ─────────
                 *    pair1: d0 × a1
                 *    pair2: 19·a3 × a3
                 *    pair3: 2·a2 × r4 */
                "mov rax, r14\n\t"
                "xor r8d, r8d\n\t"
                "mov rcx,  r12\n\t"
                "mov rdx,  [rsp]\n\t"               /* a1 */
                "mov r10,  [rsp+16]\n\t"            /* a3 */
                "mov r9,   r10\n\t"
                "imul r9,  r9, 19\n\t"              /* r9 = r3 = 19·a3 */
                                                    /* r10 = a3 already */
                "mov r11,  [rsp+8]\n\t"             /* a2 */
                "lea r11,  [r11+r11]\n\t"           /* r11 = d2 */
                "mov r14,  r15\n\t"                 /* r14 = r4 */
                "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+8], rax\n\t"

                /* ── c[2] = carry + d0·a2 + a1·a1 + d3·r4 ─────────
                 *    pair1: d0 × a2
                 *    pair2: a1 × a1
                 *    pair3: 2·a3 × r4 */
                "mov rax, r14\n\t"
                "xor r8d, r8d\n\t"
                "mov rcx,  r12\n\t"
                "mov rdx,  [rsp+8]\n\t"             /* a2 */
                "mov r9,   [rsp]\n\t"               /* a1 */
                "mov r10,  r9\n\t"                  /* pair2: a1, a1 */
                "mov r11,  [rsp+16]\n\t"            /* a3 */
                "lea r11,  [r11+r11]\n\t"           /* r11 = d3 = 2·a3 */
                "mov r14,  r15\n\t"                 /* r14 = r4 */
                "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+16], rax\n\t"

                /* ── c[3] = carry + d0·a3 + d1·a2 + r4·a4 ─────────
                 *    pair1: d0 × a3
                 *    pair2: d1 × a2
                 *    pair3: r4 × a4 */
                "mov rax, r14\n\t"
                "xor r8d, r8d\n\t"
                "mov rcx,  r12\n\t"
                "mov rdx,  [rsp+16]\n\t"            /* a3 */
                "mov r9,   r13\n\t"
                "mov r10,  [rsp+8]\n\t"             /* a2 */
                "mov r11,  r15\n\t"                 /* r11 = r4 */
                "mov r14,  rbx\n\t"                 /* r14 = a4 */
                "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+24], rax\n\t"

                /* ── c[4] = carry + d0·a4 + d1·a3 + a2·a2 ─────────
                 *    pair1: d0 × a4
                 *    pair2: d1 × a3
                 *    pair3: a2 × a2 */
                "mov rax, r14\n\t"
                "xor r8d, r8d\n\t"
                "mov rcx,  r12\n\t"
                "mov rdx,  rbx\n\t"                 /* pair1: d0, a4 */
                "mov r9,   r13\n\t"
                "mov r10,  [rsp+16]\n\t"            /* a3 */
                "mov r11,  [rsp+8]\n\t"             /* a2 */
                "mov r14,  r11\n\t"                 /* pair3: a2, a2 */
                "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+32], rax\n\t"

                /* ── Restore stack, final reduction ────────────── */
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
        uint64_t state_mac[5];
        uint64_t tmp[5];
        memcpy(state_mac, state_ref, sizeof(state_ref));

        printf("Installing MAC3x128 patch (9 triads, 3-MAC per vmwrite)...\n");
        install_mac3x128();

        printf("==========================================================\n");
        printf("  Curve25519 fe_sq: 3-MAC/vmwrite (5 calls) vs Native C\n");
        printf("==========================================================\n\n");

        /* ── Hook sanity: single 3-MAC ────────────────────────────── */
        uint64_t tlo, thi;

        /* 0 + 3*7 + 5*11 + 2*13 = 21 + 55 + 26 = 102 */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 3\n\t"   "mov rdx, 7\n\t"
                "mov r9,  5\n\t"   "mov r10, 11\n\t"
                "mov r11, 4\n\t"   "mov r14, 13\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(tlo), [hi] "=r"(thi)
                :: "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r14"
        );
        printf("  3-MAC test: 3*7+5*11+2*13 = {%lu, %lu} (expect {0, 102})\n",
               thi, tlo);

        /* Overflow test: large values */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[big]\n\t"  "mov rdx, 4\n\t"
                "mov r9,  %[big]\n\t"  "mov r10, 4\n\t"
                "mov r11, %[big]\n\t"  "mov r14, 4\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(tlo), [hi] "=r"(thi)
                : [big] "r"(1ULL << 62)
                : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r14"
        );
        /* 3 × (2^62 * 4) = 3 × 2^64 = {3, 0} */
        printf("  Overflow:  3*(2^62*4) = {%lu, %lu} (expect {3, 0})\n\n",
               thi, tlo);

        /* ── Single fe_sq correctness ─────────────────────────────── */
        fe_sq_ref(state_ref, tmp);
        uint64_t tmp2[5];
        memcpy(tmp2, state_ref, sizeof(tmp2));
        fe_sq_mac3x128(tmp2, tmp2);
        printf("  Single fe_sq match: %s\n\n",
               memcmp(tmp, tmp2, sizeof(tmp)) == 0 ? "OK" : "MISMATCH");

        printf("Batched: %d ops/batch, %d batches\n\n", BATCH, REPS);

        /* ── Native C (reference) ─────────────────────────────────── */
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
        printf("  Native C:    min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);
        fe_sq_ref(state_ref, state_ref);

        /* ── MAC3x128 via vmwrite ────────────────────────────────── */
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
        printf("  ASM+3MAC:    min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);
        fe_sq_mac3x128(state_mac, state_mac);

        /* ── Sanity check ─────────────────────────────────────────── */
        printf("\n  Final state match: ");
        if (memcmp(state_ref, state_mac, sizeof(state_ref)) == 0)
                printf("OK\n");
        else
                printf("MISMATCH\n");

        /* ── Breakdown ─────────────────────────────────────────────── */
        printf("\n  Breakdown (3-MAC, 9 triads, 5 vmwrites):\n");
        printf("    5 x ~5 cycle redirect  = ~25 cycles\n");
        printf("    5 x ~9 cycle body      = ~45 cycles  (9 triads)\n");
        printf("    asm carry+reduce chain  = ~25 cycles  (shld/shl/shr/store x5)\n");
        printf("    asm precompute+loads    = ~15 cycles  (reg setup per limb)\n");
        printf("    -------------------------------------------\n");
        printf("    Estimated total:          ~110 cycles\n\n");
        printf("  vs bench_twohooks (15 vmwrites): ~165 cycles\n");
        printf("  Savings: ~55 cycles (~33%%)\n\n");

        printf("  Next: monolithic patch (1 vmwrite, all 15 MACs in ucode)\n");

        return 0;
}
