/*
 * mac128_nomovs.c — MAC128 via VMWRITE hook (SETCC carry, 4 triads)
 *
 * vmwrite rcx, rdx  (hooked at 0x0cd8 → patch 0x7c00)
 * acc_hi:acc_lo += RCX × RDX
 *
 * INPUT:  RCX=multiplicand, RDX=multiplier, RAX=acc_lo, R8=acc_hi
 * OUTPUT: RAX=acc_lo, R8=acc_hi
 * CLOBBERS: RCX, RDX
 *
 * Flag findings from flag_test.c:
 *   - ADD sets internal flags → SETCC reads them (intra + cross-triad)
 *   - MUL does NOT poison flags (overturns earlier assumption)
 *   - CRITICAL: ADD must write to a TMP register, not an architectural
 *     register (RAX/RCX/RDX/R8 etc), for SETCC to see the carry flag.
 *     ADD→TMP0 + SETCC works; ADD→RAX + SETCC silently gets CF=0.
 *   - GENARITHFLAGS alone is useless (doesn't generate, only publishes)
 *   - CMOVCC/SELECTCC/UJMPCC read a different flag domain (broken)
 *
 *   T0: ZEROEXT TMP3, RAX | MUL RCX×RDX→RCX:RDX | NOP
 *   T1: ADD TMP0, TMP3, RCX | SETCC_CONDB TMP1, TMP0 | NOP
 *   T2: ZEROEXT RAX, TMP0 | ADD R8, R8, RDX | NOP
 *   T3: ADD R8, R8, TMP1 | NOP | END
 *
 * 4 triads, 6 real ops.
 *
 * Build:  make PROG=mac128_nomovs
 * Run:    sudo taskset -c 0 ./mac128_nomovs_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <x86intrin.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

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

static inline uint64_t rdtsc_fenced(void) {
        unsigned int aux;
        return __rdtscp(&aux);
}

static inline __uint128_t sw_mac128(uint64_t a, uint64_t b, __uint128_t acc) {
        return acc + (__uint128_t)a * b;
}

void install_mac128(void) {
        ucode_t mac128_patch[] = {
                /* T0: save acc_lo, multiply
                 *   TMP3 = old RAX (acc_lo)
                 *   MUL: RCX = product_lo, RDX = product_hi
                 */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1: ADD to TMP0 (not RAX!) + SETCC to TMP1
                 * Matches flag_test 2D register usage exactly */
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
 *  MAC128 CURVE25519 SQUARE — 15 vmwrite calls (1 MAC per vmwrite)
 *
 *  Register map:
 *    Persistent across vmwrites:
 *      r9  = d0 (2·a0)     r10 = d1 (2·a1)
 *      r11 = r4 (19·a4)    rbx = a4
 *      %[a], %[out] = pointers (compiler-assigned)
 *
 *    Per-vmwrite operand registers (clobbered by microcode):
 *      rcx, rdx = multiplicand, multiplier
 *
 *    Accumulator (per-limb):
 *      rax = acc_lo,  r8 = acc_hi
 * ══════════════════════════════════════════════════════════════════ */
__attribute__((noinline))
static void fe_sq_mac128(const uint64_t *a, uint64_t *out) {
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

                "mov r9,   [%[a]]\n\t"
                "lea r9,   [r9+r9]\n\t"             /* r9  = d0 = 2·a0 */
                "mov r10,  [rsp]\n\t"
                "lea r10,  [r10+r10]\n\t"           /* r10 = d1 = 2·a1 */
                "mov rbx,  [%[a]+32]\n\t"           /* rbx = a4 */
                "imul r11, rbx, 19\n\t"             /* r11 = r4 = 19·a4 */

                /* ── c[0] = a0·a0 + d1·r4 + d2·r3 ─────────────────
                 *    3 vmwrites */
                "xor eax, eax\n\t"
                "xor r8d, r8d\n\t"
                /* MAC 1: a0 × a0 */
                "mov rcx, [%[a]]\n\t"
                "mov rdx, rcx\n\t"
                "vmwrite rcx, rdx\n\t"
                /* MAC 2: d1 × r4 */
                "mov rcx, r10\n\t"
                "mov rdx, r11\n\t"
                "vmwrite rcx, rdx\n\t"
                /* MAC 3: d2 × r3 (2·a2 × 19·a3) */
                "mov rcx, [rsp+8]\n\t"
                "lea rcx, [rcx+rcx]\n\t"            /* d2 */
                "mov rdx, [rsp+16]\n\t"
                "imul rdx, rdx, 19\n\t"             /* r3 */
                "vmwrite rcx, rdx\n\t"

                /* extract c[0], carry */
                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]], rax\n\t"

                /* ── c[1] = carry + d0·a1 + r3·a3 + d2·r4 ────────── */
                "mov rax, r14\n\t"
                "xor r8d, r8d\n\t"
                /* MAC 4: d0 × a1 */
                "mov rcx, r9\n\t"
                "mov rdx, [rsp]\n\t"
                "vmwrite rcx, rdx\n\t"
                /* MAC 5: r3 × a3  (19·a3 × a3) */
                "mov rcx, [rsp+16]\n\t"
                "imul rdx, rcx, 19\n\t"
                "mov rdx, rcx\n\t"          /* rdx = a3 */
                "imul rcx, rcx, 19\n\t"     /* rcx = 19·a3 = r3 */
                "vmwrite rcx, rdx\n\t"
                /* MAC 6: d2 × r4  (2·a2 × 19·a4) */
                "mov rcx, [rsp+8]\n\t"
                "lea rcx, [rcx+rcx]\n\t"            /* d2 */
                "mov rdx, r11\n\t"                  /* r4 */
                "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+8], rax\n\t"

                /* ── c[2] = carry + d0·a2 + a1·a1 + d3·r4 ────────── */
                "mov rax, r14\n\t"
                "xor r8d, r8d\n\t"
                /* MAC 7: d0 × a2 */
                "mov rcx, r9\n\t"
                "mov rdx, [rsp+8]\n\t"
                "vmwrite rcx, rdx\n\t"
                /* MAC 8: a1 × a1 */
                "mov rcx, [rsp]\n\t"
                "mov rdx, rcx\n\t"
                "vmwrite rcx, rdx\n\t"
                /* MAC 9: d3 × r4  (2·a3 × 19·a4) */
                "mov rcx, [rsp+16]\n\t"
                "lea rcx, [rcx+rcx]\n\t"            /* d3 */
                "mov rdx, r11\n\t"                  /* r4 */
                "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+16], rax\n\t"

                /* ── c[3] = carry + d0·a3 + d1·a2 + r4·a4 ────────── */
                "mov rax, r14\n\t"
                "xor r8d, r8d\n\t"
                /* MAC 10: d0 × a3 */
                "mov rcx, r9\n\t"
                "mov rdx, [rsp+16]\n\t"
                "vmwrite rcx, rdx\n\t"
                /* MAC 11: d1 × a2 */
                "mov rcx, r10\n\t"
                "mov rdx, [rsp+8]\n\t"
                "vmwrite rcx, rdx\n\t"
                /* MAC 12: r4 × a4 */
                "mov rcx, r11\n\t"
                "mov rdx, rbx\n\t"
                "vmwrite rcx, rdx\n\t"

                "mov r14, r8\n\t"
                "shld r14, rax, 13\n\t"
                "shl rax, 13\n\t"
                "shr rax, 13\n\t"
                "mov [%[out]+24], rax\n\t"

                /* ── c[4] = carry + d0·a4 + d1·a3 + a2·a2 ────────── */
                "mov rax, r14\n\t"
                "xor r8d, r8d\n\t"
                /* MAC 13: d0 × a4 */
                "mov rcx, r9\n\t"
                "mov rdx, rbx\n\t"
                "vmwrite rcx, rdx\n\t"
                /* MAC 14: d1 × a3 */
                "mov rcx, r10\n\t"
                "mov rdx, [rsp+16]\n\t"
                "vmwrite rcx, rdx\n\t"
                /* MAC 15: a2 × a2 */
                "mov rcx, [rsp+8]\n\t"
                "mov rdx, rcx\n\t"
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
                  "r11", "r14", "memory"
        );
}


/* ══════════════════════════════════════════════════════════════════
 *  BENCHMARK
 * ══════════════════════════════════════════════════════════════════ */

#define BATCH 1000
#define REPS  100

int main(void) {
        uint64_t rax_out, r8_out;
        uint64_t t0, t1;
        uint64_t min, sum;

        printf("Installing MAC128 patch (4 triads, SETCC carry)...\n");
        install_mac128();

        /* ── Unit tests ───────────────────────────────────────────── */

        /* Test 1: acc=100, += 7×13 → 191 */
        asm volatile(
                "mov rax, 100\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 7\n\t"
                "mov rdx, 13\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8"
        );
        printf("\nTest 1: acc=100, += 7*13\n");
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x00000000000000bf)\n", rax_out);
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0x0000000000000000)\n", r8_out);
        printf("  %s\n", (rax_out == 191 && r8_out == 0) ? "PASS" : "FAIL");

        /* Test 2: acc=0, += max×max */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 0xFFFFFFFFFFFFFFFF\n\t"
                "mov rdx, 0xFFFFFFFFFFFFFFFF\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8"
        );
        printf("\nTest 2: acc=0, += max*max\n");
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x0000000000000001)\n", rax_out);
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0xfffffffffffffffe)\n", r8_out);
        printf("  %s\n", (rax_out == 0x1ULL && r8_out == 0xFFFFFFFFFFFFFFFEULL) ? "PASS" : "FAIL");

        /* Test 3: low overflow → carry to hi */
        asm volatile(
                "mov rax, 0xFFFFFFFFFFFFFFFF\n\t"
                "mov r8, 5\n\t"
                "mov rcx, 1\n\t"
                "mov rdx, 3\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8"
        );
        printf("\nTest 3: acc_lo=0xFF..F, acc_hi=5, += 1*3\n");
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x0000000000000002)\n", rax_out);
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0x0000000000000006)\n", r8_out);
        printf("  %s\n", (rax_out == 2 && r8_out == 6) ? "PASS" : "FAIL");

        /* Test 4: 3-MAC chain: 0 + 7*13 + 11*17 + 3*5 = 293 */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 7\n\t"
                "mov rdx, 13\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov rcx, 11\n\t"
                "mov rdx, 17\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov rcx, 3\n\t"
                "mov rdx, 5\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8"
        );
        printf("\nTest 4: chain 0 + 7*13 + 11*17 + 3*5\n");
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x0000000000000125 = 293)\n", rax_out);
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0x0000000000000000)\n", r8_out);
        printf("  %s\n", (rax_out == 293 && r8_out == 0) ? "PASS" : "FAIL");

        /* ── Raw MAC128 microbenchmark ────────────────────────────── */
        printf("\n=== Raw MAC128 Benchmark (1000000 iterations) ===\n");

        /* Warm up */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 1\n\t"
                "mov rdx, 1\n\t"
                "vmwrite rcx, rdx\n\t"
                ::: "rax", "rcx", "rdx", "r8"
        );

        /* Benchmark: ucode MAC128 (vmwrite hook) */
        uint64_t iters = 1000000;
        t0 = rdtsc_fenced();
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 0x123456789abcdef0\n\t"
                "mov rdx, 0xfedcba9876543210\n\t"
                ".align 16\n\t"
                "1:\n\t"
                "vmwrite rcx, rdx\n\t"
                "dec %[n]\n\t"
                "jnz 1b\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(rax_out), [hi] "=r"(r8_out), [n] "+r"(iters)
                :
                : "rax", "rcx", "rdx", "r8", "cc"
        );
        t1 = rdtsc_fenced();
        uint64_t ucode_cycles = t1 - t0;
        printf("ucode MAC128:    %" PRIu64 " cycles  (%.2f cycles/op)\n",
               ucode_cycles, (double)ucode_cycles / 1000000);
        printf("  result lo=0x%016" PRIx64 " hi=0x%016" PRIx64 "\n", rax_out, r8_out);

        /* Benchmark: software 128-bit MAC (mul + adc) */
        __uint128_t sw_acc = 0;
        uint64_t a_val = 0x123456789abcdef0ULL;
        uint64_t b_val = 0xfedcba9876543210ULL;

        t0 = rdtsc_fenced();
        for (int i = 0; i < 1000000; i++) {
                sw_acc = sw_mac128(a_val, b_val, sw_acc);
        }
        t1 = rdtsc_fenced();
        uint64_t sw_cycles = t1 - t0;
        uint64_t sw_lo = (uint64_t)sw_acc;
        uint64_t sw_hi = (uint64_t)(sw_acc >> 64);
        printf("software MAC128: %" PRIu64 " cycles  (%.2f cycles/op)\n",
               sw_cycles, (double)sw_cycles / 1000000);
        printf("  result lo=0x%016" PRIx64 " hi=0x%016" PRIx64 "\n", sw_lo, sw_hi);

        printf("Results match: %s\n",
               (rax_out == sw_lo && r8_out == sw_hi) ? "YES" : "NO");

        double speedup = (double)sw_cycles / ucode_cycles;
        printf("Speedup (sw/ucode): %.2fx\n", speedup);

        /* ══════════════════════════════════════════════════════════════
         *  Curve25519 fe_sq: MAC128 (15 vmwrites) vs Native C
         * ══════════════════════════════════════════════════════════════ */

        uint64_t state_ref[5] = { 0x00062D608F25D51AULL,
                                  0x000412A4B4F6592AULL,
                                  0x00075B7171A4B31DULL,
                                  0x0001FF60527118FEULL,
                                  0x000216936D3CD6E5ULL };
        uint64_t state_mac[5];
        uint64_t tmp[5];
        memcpy(state_mac, state_ref, sizeof(state_ref));

        printf("\n==========================================================\n");
        printf("  Curve25519 fe_sq: 1-MAC/vmwrite (15 calls) vs Native C\n");
        printf("==========================================================\n\n");

        /* ── Single fe_sq correctness ─────────────────────────────── */
        fe_sq_ref(state_ref, tmp);
        uint64_t tmp2[5];
        memcpy(tmp2, state_ref, sizeof(tmp2));
        fe_sq_mac128(tmp2, tmp2);
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

        /* ── MAC128 via vmwrite (15 calls per fe_sq) ────────────── */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
                memcpy(tmp, state_mac, sizeof(tmp));
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH; i++)
                        fe_sq_mac128(tmp, tmp);
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt;
                if (dt < min) min = dt;
        }
        printf("  ASM+1MAC:    min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min / BATCH, sum / REPS / BATCH);
        fe_sq_mac128(state_mac, state_mac);

        /* ── Sanity check ─────────────────────────────────────────── */
        printf("\n  Final state match: ");
        if (memcmp(state_ref, state_mac, sizeof(state_ref)) == 0)
                printf("OK\n");
        else
                printf("MISMATCH\n");

        /* ── Breakdown ─────────────────────────────────────────────── */
        printf("\n  Breakdown (1-MAC, 4 triads, 15 vmwrites):\n");
        printf("    15 x ~5 cycle redirect  = ~75 cycles\n");
        printf("    15 x ~4 cycle body      = ~60 cycles  (4 triads)\n");
        printf("    asm carry+reduce chain  = ~25 cycles  (shld/shl/shr/store x5)\n");
        printf("    asm precompute+loads    = ~30 cycles  (reg setup per MAC)\n");
        printf("    -------------------------------------------\n");
        printf("    Estimated total:          ~190 cycles\n\n");
        printf("  vs bench_3mac (5 vmwrites):  ~110 cycles\n");
        printf("  Overhead from 10 extra redirects: ~50 cycles\n\n");

        return 0;
}
