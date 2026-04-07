/*
 * bench_twohooks.c — Curve25519 square benchmark: MAC128 (4 triads) vs native C
 *
 * Self-contained: installs the 4-triad MAC128 patch from mac128_nomovs.c,
 * then benchmarks it against native C.
 *
 * Build:  make PROG=bench_twohooks
 * Run:    sudo taskset -c 0 ./bench_twohooks_static
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
 *  MAC128 patch install — 4 triads (from mac128_nomovs.c)
 *
 *  vmwrite rcx, rdx  (hooked at 0x0cd8 → patch 0x7c00)
 *  acc_hi:acc_lo += RCX × RDX
 *
 *  INPUT:  RCX=multiplicand, RDX=multiplier, RAX=acc_lo, R8=acc_hi
 *  OUTPUT: RAX=acc_lo, R8=acc_hi
 *  CLOBBERS: RCX, RDX
 *
 *   T0: ZEROEXT TMP3, RAX | MUL RCX×RDX→RCX:RDX | NOP
 *   T1: ADD TMP0, TMP3, RCX | SETCC_CONDB TMP1, TMP0 | NOP
 *   T2: ZEROEXT RAX, TMP0 | ADD R8, R8, RDX | NOP
 *   T3: ADD R8, R8, TMP1 | NOP | END
 * ══════════════════════════════════════════════════════════════════ */
static void install_mac128(void) {
        ucode_t mac128_patch[] = {
                /* T0: save acc_lo, multiply */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1: ADD to TMP0 + SETCC to TMP1 */
                {
                        ADD_DSZ64_DRR(TMP0, TMP3, RCX),
                        SETCC_CONDB_DR(TMP1, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                /* T2: copy sum to RAX, acc_hi += prod_hi */
                {
                        ZEROEXT_DSZ64_DR(RAX, TMP0),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T3: fold carry into acc_hi, done */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP1),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac128_patch, ARRAY_SZ(mac128_patch));
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
 *  MAC128 CURVE25519 SQUARE — pure assembly, 15 vmwrite calls
 *
 *  Everything in one asm block: limb loads, precomputation,
 *  15 MAC accumulations, carry chain, and final reduction.
 *
 *  Register map:
 *    rbx  = a0 → d0 (2·a0) after c[0]
 *    r9   = a1          r13 = d1 (2·a1)
 *    r10  = a2          r15 = r4 (19·a4)
 *    r11  = a3
 *    r12  = a4          r14 = carry between limbs
 *    rax/r8 = MAC accumulator (lo/hi)
 *    rcx/rdx = MAC operands (clobbered per vmwrite)
 *    d2 (2·a2), d3 (2·a3) computed on the fly via lea
 *    r3 (19·a3) computed on the fly via imul (only needed in c[0], c[1])
 *    %[a] and %[out] stay in rsi/rdi (compiler-assigned, untouched)
 * ══════════════════════════════════════════════════════════════════ */
__attribute__((noinline))
static void fe_sq_mac128(const uint64_t *a, uint64_t *out) {
        asm volatile(
                /* ── Load limbs ─────────────────────────────────── */
                "mov rbx, [%[a]]\n\t"           /* rbx = a0 */
                "mov r9,  [%[a]+8]\n\t"         /* r9  = a1 */
                "mov r10, [%[a]+16]\n\t"        /* r10 = a2 */
                "mov r11, [%[a]+24]\n\t"        /* r11 = a3 */
                "mov r12, [%[a]+32]\n\t"        /* r12 = a4 */

                /* ── Pre-compute coefficients ───────────────────── */
                "lea r13, [r9+r9]\n\t"          /* r13 = d1 = 2·a1 */
                "imul r15, r12, 19\n\t"         /* r15 = r4 = 19·a4 */

                /* ── c[0] = a0·a0 + d1·r4 + d2·r3 ─────────────── */
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, rbx\n\t"      "mov rdx, rbx\n\t"     "vmwrite rcx, rdx\n\t"
                "mov rcx, r13\n\t"      "mov rdx, r15\n\t"     "vmwrite rcx, rdx\n\t"
                "lea rcx, [r10+r10]\n\t" "imul rdx, r11, 19\n\t" "vmwrite rcx, rdx\n\t"

                /* carry = (hi<<13)|(lo>>51); out[0] = lo & MASK51 */
                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]], rax\n\t"

                /* a0 → d0 */
                "lea rbx, [rbx+rbx]\n\t"

                /* ── c[1] = carry + d0·a1 + r3·a3 + d2·r4 ─────── */
                "mov rax, r14\n\t"
                "xor r8, r8\n\t"
                "mov rcx, rbx\n\t"      "mov rdx, r9\n\t"      "vmwrite rcx, rdx\n\t"
                "imul rcx, r11, 19\n\t" "mov rdx, r11\n\t"     "vmwrite rcx, rdx\n\t"
                "lea rcx, [r10+r10]\n\t" "mov rdx, r15\n\t"    "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+8], rax\n\t"

                /* ── c[2] = carry + d0·a2 + a1·a1 + d3·r4 ─────── */
                "mov rax, r14\n\t"
                "xor r8, r8\n\t"
                "mov rcx, rbx\n\t"       "mov rdx, r10\n\t"    "vmwrite rcx, rdx\n\t"
                "mov rcx, r9\n\t"        "mov rdx, r9\n\t"     "vmwrite rcx, rdx\n\t"
                "lea rcx, [r11+r11]\n\t" "mov rdx, r15\n\t"    "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+16], rax\n\t"

                /* ── c[3] = carry + d0·a3 + d1·a2 + r4·a4 ─────── */
                "mov rax, r14\n\t"
                "xor r8, r8\n\t"
                "mov rcx, rbx\n\t"      "mov rdx, r11\n\t"     "vmwrite rcx, rdx\n\t"
                "mov rcx, r13\n\t"      "mov rdx, r10\n\t"     "vmwrite rcx, rdx\n\t"
                "mov rcx, r15\n\t"      "mov rdx, r12\n\t"     "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+24], rax\n\t"

                /* ── c[4] = carry + d0·a4 + d1·a3 + a2·a2 ─────── */
                "mov rax, r14\n\t"
                "xor r8, r8\n\t"
                "mov rcx, rbx\n\t"      "mov rdx, r12\n\t"     "vmwrite rcx, rdx\n\t"
                "mov rcx, r13\n\t"      "mov rdx, r11\n\t"     "vmwrite rcx, rdx\n\t"
                "mov rcx, r10\n\t"      "mov rdx, r10\n\t"     "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+32], rax\n\t"

                /* ── Final reduction: out[0] += carry·19 ───────── */
                "imul r14, r14, 19\n\t"
                "add r14, [%[out]]\n\t"         /* r14 = out[0] + carry*19 */
                "mov rax, r14\n\t"
                "shr rax, 51\n\t"               /* rax = carry2 */
                "shl r14, 13\n\t"
                "shr r14, 13\n\t"               /* r14 &= MASK51 */
                "mov [%[out]], r14\n\t"
                "add [%[out]+8], rax\n\t"       /* out[1] += carry2 */

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

        /* Starting value — square iteratively to prevent dead code */
        uint64_t state_ref[5] = { 0x00062D608F25D51AULL,
                                  0x000412A4B4F6592AULL,
                                  0x00075B7171A4B31DULL,
                                  0x0001FF60527118FEULL,
                                  0x000216936D3CD6E5ULL };
        uint64_t state_mac[5];
        uint64_t tmp[5];
        memcpy(state_mac, state_ref, sizeof(state_ref));

        printf("Installing MAC128 patch (4 triads, SETCC carry)...\n");
        install_mac128();

        printf("╔════════════════════════════════════════════════════╗\n");
        printf("║  Curve25519 fe_sq: Pure ASM+MAC128 vs Native C      ║\n");
        printf("╚════════════════════════════════════════════════════╝\n\n");

        /* ── Hook sanity tests ────────────────────────────────────── */
        uint64_t tlo, thi;

        /* Test accumulation: 0 + 3*7 + 5*11 = 76 */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 3\n\t"  "mov rdx, 7\n\t"   "vmwrite rcx, rdx\n\t"
                "mov rcx, 5\n\t"  "mov rdx, 11\n\t"  "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(tlo), [hi] "=r"(thi)
                :: "rax", "rcx", "rdx", "r8"
        );
        printf("  Hook test: 3*7+5*11 = {%lu, %lu} (expect {0, 76})\n", thi, tlo);

        /* Test carry-in: 100 + 3*7 = 121 */
        asm volatile(
                "mov rax, 100\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 3\n\t"  "mov rdx, 7\n\t"  "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(tlo), [hi] "=r"(thi)
                :: "rax", "rcx", "rdx", "r8"
        );
        printf("  Carry-in:  100+3*7 = {%lu, %lu} (expect {0, 121})\n", thi, tlo);

        /* Test 128-bit overflow: 2^63 + 2^63*2 = 2^63 + 2^64 = {1, 2^63} */
        asm volatile(
                "mov rax, %[init]\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[big]\n\t"  "mov rdx, 2\n\t"  "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(tlo), [hi] "=r"(thi)
                : [init] "r"(1ULL << 63), [big] "r"(1ULL << 63)
                : "rax", "rcx", "rdx", "r8"
        );
        printf("  Overflow:  2^63+2^63*2 = {%lu, %lu} (expect {1, %lu})\n\n",
               thi, tlo, (unsigned long)(1ULL << 63));

        printf("Batched: %d ops/batch, %d batches\n\n", BATCH, REPS);

        /* ── Native C (reference) ─────────────────────────────────── */

        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                memcpy(tmp, state_ref, sizeof(tmp));
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        fe_sq_ref(tmp, tmp);
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  Native C:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);
        fe_sq_ref(state_ref, state_ref);  /* advance state to prevent opt */

        /* ── MAC128 via vmwrite ───────────────────────────────────── */

        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                memcpy(tmp, state_mac, sizeof(tmp));
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++) {
                        fe_sq_mac128(tmp, tmp);
                }
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  ASM+MAC128: min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);
        fe_sq_mac128(state_mac, state_mac);

        /* ── Sanity: both should produce the same final state ─────── */

        printf("\n  Sanity check (final state match): ");
        if (memcmp(state_ref, state_mac, sizeof(state_ref)) == 0)
                printf("✓ OK\n");
        else
                printf("✗ MISMATCH\n");

        /* ── Breakdown ─────────────────────────────────────────────── */

        printf("\n  Breakdown (pure ASM + MAC128, 4 triads):\n");
        printf("    15 × ~5 cycle redirect  = ~75 cycles\n");
        printf("    15 × ~4 cycle MAC body  = ~60 cycles  (4 triads)\n");
        printf("    asm carry+reduce chain  = ~20 cycles  (shld/shl/shr/store)\n");
        printf("    asm precompute+loads    = ~10 cycles\n");
        printf("    ─────────────────────────────────────\n");
        printf("    Estimated total:          ~165 cycles\n\n");

        printf("  Optimization path:\n");
        printf("    3-MAC/vmwrite (5 calls):  ~70 cycles\n");
        printf("    Monolithic (1 call):      ~60 cycles\n");

        return 0;
}
