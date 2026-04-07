/*
 * bench_sq_mac3.c — 3-MAC-per-vmwrite batched hook for curve25519 square
 *
 * ═══════════════════════════════════════════════════════════════════
 *  Reduces vmwrite calls from 15 → 5 (one per limb).
 *
 *  Each vmwrite triggers an 18-triad hook that accumulates 3 products:
 *    MAC(RCX×RDX) + MAC(RSI×RDI) + MAC(RBX×R9) → RAX:R8
 *
 *  Expected: ~75–85 cycles (vs 112 for 1-MAC, vs 53 native)
 *
 *  Build:  make bench_sq_mac3_static
 *  Run:    sudo taskset -c 0 ./bench_sq_mac3_static
 * ═══════════════════════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL

/* ── Benchmark parameters ─────────────────────────────────────── */
#define OPS_PER_BATCH   1000
#define NUM_BATCHES     200
#define WARMUP_BATCHES  20


/* ══════════════════════════════════════════════════════════════════
 *  3-MAC HOOK INSTALLER (18 triads at 0x7c00)
 *
 *  vmwrite rcx, rdx  →  RAX:R8 += RCX×RDX + RSI×RDI + RBX×R9
 *
 *  Operand registers pre-loaded by x86 before vmwrite:
 *    RAX   = accumulator low  (carry seed or 0)
 *    R8    = accumulator high (0)
 *    RCX   = pair1_a     RDX = pair1_b
 *    RSI   = pair2_a     RDI = pair2_b
 *    RBX   = pair3_a     R9  = pair3_b
 *
 *  NOTE: R9 as microcode source register is used in the confirmed
 *  5-MAC batch pipeline. If it causes issues, try R10 or R11.
 *
 *  Hook: 0x0cd8 → patch 0x7c00
 * ══════════════════════════════════════════════════════════════════ */
static void install_mac3(void) {
        ucode_t mac3_patch[] = {

                /* ═══ MAC 1: RCX × RDX ═══════════════════════ */

                /* T0: save acc_lo, multiply pair 1 */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1: sum + carry operands */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T2: propagated overflow + acc_hi */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T3: merge carry */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T4: extract carry bit */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T5: fold carry into acc_hi */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },

                /* ═══ MAC 2: RSI × RDI ═══════════════════════ */

                /* T6: save acc_lo, multiply pair 2 */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(RSI, RSI, RDI),
                        NOP,
                        NOP_SEQWORD
                },
                /* T7: sum + carry operands */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RSI),
                        AND_DSZ64_DRR(TMP1, TMP3, RSI),
                        OR_DSZ64_DRR(TMP2, TMP3, RSI),
                        NOP_SEQWORD
                },
                /* T8: propagated overflow + acc_hi */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDI),
                        NOP,
                        NOP_SEQWORD
                },
                /* T9: merge carry */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T10: extract carry bit */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T11: fold carry into acc_hi */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },

                /* ═══ MAC 3: RBX × R9 ════════════════════════ */

                /* T12: save acc_lo, multiply pair 3 */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(RBX, RBX, R9),
                        NOP,
                        NOP_SEQWORD
                },
                /* T13: sum + carry operands */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RBX),
                        AND_DSZ64_DRR(TMP1, TMP3, RBX),
                        OR_DSZ64_DRR(TMP2, TMP3, RBX),
                        NOP_SEQWORD
                },
                /* T14: propagated overflow + acc_hi */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, R9),
                        NOP,
                        NOP_SEQWORD
                },
                /* T15: merge carry */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T16: extract carry bit */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T17: fold carry, DONE */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac3_patch, 18);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  REFERENCE
 * ══════════════════════════════════════════════════════════════════ */
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
 *  3-MAC CURVE25519 SQUARE — 5 vmwrite calls
 *
 *  Each vmwrite:  RAX:R8 += RCX×RDX + RSI×RDI + RBX×R9
 *  Carry propagation between limbs in x86.
 * ══════════════════════════════════════════════════════════════════ */
__attribute__((noinline))
static void fe_sq_mac3(const uint64_t *a, uint64_t *out) {
        uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
        uint64_t d0 = 2*a0, d1 = 2*a1, d2 = 2*a2, d3 = 2*a3;
        uint64_t r3 = 19*a3, r4 = 19*a4;

        uint64_t c_lo, c_hi, carry;

        /* c[0] = a0·a0 + d1·r4 + d2·r3 */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"
                "mov rsi, %[x2]\n\t"  "mov rdi, %[y2]\n\t"
                "mov rbx, %[x3]\n\t"  "mov r9,  %[y3]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [x1] "r"(a0), [y1] "r"(a0),
                  [x2] "r"(d1), [y2] "r"(r4),
                  [x3] "r"(d2), [y3] "r"(r3)
                : "rax", "rcx", "rdx", "rsi", "rdi", "rbx", "r8", "r9"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[0] = c_lo & MASK51;

        /* c[1] = carry + d0·a1 + r3·a3 + d2·r4 */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"
                "mov rsi, %[x2]\n\t"  "mov rdi, %[y2]\n\t"
                "mov rbx, %[x3]\n\t"  "mov r9,  %[y3]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a1),
                  [x2] "r"(r3), [y2] "r"(a3),
                  [x3] "r"(d2), [y3] "r"(r4)
                : "rax", "rcx", "rdx", "rsi", "rdi", "rbx", "r8", "r9"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[1] = c_lo & MASK51;

        /* c[2] = carry + d0·a2 + a1·a1 + d3·r4 */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"
                "mov rsi, %[x2]\n\t"  "mov rdi, %[y2]\n\t"
                "mov rbx, %[x3]\n\t"  "mov r9,  %[y3]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a2),
                  [x2] "r"(a1), [y2] "r"(a1),
                  [x3] "r"(d3), [y3] "r"(r4)
                : "rax", "rcx", "rdx", "rsi", "rdi", "rbx", "r8", "r9"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[2] = c_lo & MASK51;

        /* c[3] = carry + d0·a3 + d1·a2 + r4·a4 */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"
                "mov rsi, %[x2]\n\t"  "mov rdi, %[y2]\n\t"
                "mov rbx, %[x3]\n\t"  "mov r9,  %[y3]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a3),
                  [x2] "r"(d1), [y2] "r"(a2),
                  [x3] "r"(r4), [y3] "r"(a4)
                : "rax", "rcx", "rdx", "rsi", "rdi", "rbx", "r8", "r9"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[3] = c_lo & MASK51;

        /* c[4] = carry + d0·a4 + d1·a3 + a2·a2 */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"
                "mov rsi, %[x2]\n\t"  "mov rdi, %[y2]\n\t"
                "mov rbx, %[x3]\n\t"  "mov r9,  %[y3]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a4),
                  [x2] "r"(d1), [y2] "r"(a3),
                  [x3] "r"(a2), [y3] "r"(a2)
                : "rax", "rcx", "rdx", "rsi", "rdi", "rbx", "r8", "r9"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[4] = c_lo & MASK51;

        /* Final reduction */
        out[0] += carry * 19;
        carry = out[0] >> 51;
        out[0] &= MASK51;
        out[1] += carry;
}


/* ══════════════════════════════════════════════════════════════════
 *  1-MAC HOOK (original, for A/B comparison)
 * ══════════════════════════════════════════════════════════════════ */
static void install_mac128(void) {
        ucode_t mac128_patch[] = {
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        /* Overwrites whatever is at 0x7c00 — call AFTER mac3 bench */
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac128_patch, 6);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

__attribute__((noinline))
static void fe_sq_mac128(const uint64_t *a, uint64_t *out) {
        uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
        uint64_t d0 = 2*a0, d1 = 2*a1, d2 = 2*a2, d3 = 2*a3;
        uint64_t r3 = 19*a3, r4 = 19*a4;

        uint64_t c_lo, c_hi, carry;

#define MAC1(xA, yA) \
        "mov rcx, %[" xA "]\n\t" \
        "mov rdx, %[" yA "]\n\t" \
        "vmwrite rcx, rdx\n\t"

        /* c[0] = a0·a0 + d1·r4 + d2·r3 */
        asm volatile(
                "xor rax, rax\n\t" "xor r8, r8\n\t"
                MAC1("x1","y1") MAC1("x2","y2") MAC1("x3","y3")
                "mov %[lo], rax\n\t" "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [x1] "r"(a0), [y1] "r"(a0),
                  [x2] "r"(d1), [y2] "r"(r4),
                  [x3] "r"(d2), [y3] "r"(r3)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51); out[0] = c_lo & MASK51;

        /* c[1] = carry + d0·a1 + r3·a3 + d2·r4 */
        asm volatile(
                "mov rax, %[cin]\n\t" "xor r8, r8\n\t"
                MAC1("x1","y1") MAC1("x2","y2") MAC1("x3","y3")
                "mov %[lo], rax\n\t" "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a1),
                  [x2] "r"(r3), [y2] "r"(a3),
                  [x3] "r"(d2), [y3] "r"(r4)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51); out[1] = c_lo & MASK51;

        /* c[2] = carry + d0·a2 + a1·a1 + d3·r4 */
        asm volatile(
                "mov rax, %[cin]\n\t" "xor r8, r8\n\t"
                MAC1("x1","y1") MAC1("x2","y2") MAC1("x3","y3")
                "mov %[lo], rax\n\t" "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a2),
                  [x2] "r"(a1), [y2] "r"(a1),
                  [x3] "r"(d3), [y3] "r"(r4)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51); out[2] = c_lo & MASK51;

        /* c[3] = carry + d0·a3 + d1·a2 + r4·a4 */
        asm volatile(
                "mov rax, %[cin]\n\t" "xor r8, r8\n\t"
                MAC1("x1","y1") MAC1("x2","y2") MAC1("x3","y3")
                "mov %[lo], rax\n\t" "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a3),
                  [x2] "r"(d1), [y2] "r"(a2),
                  [x3] "r"(r4), [y3] "r"(a4)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51); out[3] = c_lo & MASK51;

        /* c[4] = carry + d0·a4 + d1·a3 + a2·a2 */
        asm volatile(
                "mov rax, %[cin]\n\t" "xor r8, r8\n\t"
                MAC1("x1","y1") MAC1("x2","y2") MAC1("x3","y3")
                "mov %[lo], rax\n\t" "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a4),
                  [x2] "r"(d1), [y2] "r"(a3),
                  [x3] "r"(a2), [y3] "r"(a2)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51); out[4] = c_lo & MASK51;

#undef MAC1

        out[0] += carry * 19;
        carry = out[0] >> 51; out[0] &= MASK51;
        out[1] += carry;
}


/* ══════════════════════════════════════════════════════════════════
 *  TSC + HELPERS
 * ══════════════════════════════════════════════════════════════════ */
static inline uint64_t rdtscp_start(void) {
        uint32_t lo, hi;
        asm volatile("cpuid\n\t" "rdtsc"
                     : "=a"(lo), "=d"(hi) :: "rbx", "rcx");
        return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtscp_end(void) {
        uint32_t lo, hi;
        asm volatile("rdtscp\n\t"
                     "mov %0, eax\n\t" "mov %1, edx\n\t"
                     "cpuid"
                     : "=r"(lo), "=r"(hi)
                     :: "rax", "rbx", "rcx", "rdx");
        return ((uint64_t)hi << 32) | lo;
}

static void print_limbs(const char *label, const uint64_t *v) {
        printf("  %-10s [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ",\n"
               "             %016" PRIx64 ", %016" PRIx64 "]\n",
               label, v[0], v[1], v[2], v[3], v[4]);
}

static void fe_normalize(const uint64_t *in, uint64_t *out) {
        uint64_t t[5], carry;
        memcpy(t, in, sizeof(uint64_t) * 5);
        for (int pass = 0; pass < 2; pass++) {
                carry = t[0] >> 51; t[0] &= MASK51;
                t[1] += carry;
                carry = t[1] >> 51; t[1] &= MASK51;
                t[2] += carry;
                carry = t[2] >> 51; t[2] &= MASK51;
                t[3] += carry;
                carry = t[3] >> 51; t[3] &= MASK51;
                t[4] += carry;
                carry = t[4] >> 51; t[4] &= MASK51;
                t[0] += carry * 19;
        }
        memcpy(out, t, sizeof(uint64_t) * 5);
}

static int fe_equal(const uint64_t *a, const uint64_t *b) {
        uint64_t na[5], nb[5];
        fe_normalize(a, na);
        fe_normalize(b, nb);
        return memcmp(na, nb, sizeof(na)) == 0;
}


/* ══════════════════════════════════════════════════════════════════
 *  CORRECTNESS
 * ══════════════════════════════════════════════════════════════════ */
typedef struct { const char *name; uint64_t in[5]; } sq_test_t;

typedef void (*sq_fn)(const uint64_t *, uint64_t *);

static int test_one(const char *label, sq_fn fn, const sq_test_t *t,
                    const uint64_t *ref) {
        uint64_t out[5];
        fn(t->in, out);
        print_limbs(label, out);
        if (!fe_equal(ref, out)) {
                printf("  *** %s MISMATCH ***\n", label);
                return 0;
        }
        return 1;
}

static int test_iterated(const char *label, sq_fn fn) {
        uint64_t ref[5] = {1,0,0,0,0}, v[5] = {1,0,0,0,0};
        for (int i = 0; i < 1000; i++) {
                uint64_t tmp[5];
                fe_sq_ref(ref, tmp); memcpy(ref, tmp, sizeof(ref));
                fn(v, tmp);          memcpy(v, tmp, sizeof(v));
        }
        printf("  %-10s ", label);
        int ok = fe_equal(ref, v);
        printf("%s\n", ok ? "✓" : "✗ MISMATCH");
        return ok;
}


/* ══════════════════════════════════════════════════════════════════
 *  BENCHMARK
 * ══════════════════════════════════════════════════════════════════ */
static int cmp_u64(const void *a, const void *b) {
        uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
        return (x > y) - (x < y);
}

static void bench_one(const char *name, sq_fn fn, const uint64_t *input) {
        uint64_t timings[NUM_BATCHES];
        uint64_t out[5];
        volatile uint64_t sink = 0;
        (void)sink;

        for (int i = 0; i < WARMUP_BATCHES; i++)
                for (int j = 0; j < OPS_PER_BATCH; j++)
                        fn(input, out);

        for (int b = 0; b < NUM_BATCHES; b++) {
                uint64_t t0 = rdtscp_start();
                for (int j = 0; j < OPS_PER_BATCH; j++)
                        fn(input, out);
                uint64_t t1 = rdtscp_end();
                timings[b] = t1 - t0;
                sink = out[0];
        }

        qsort(timings, NUM_BATCHES, sizeof(uint64_t), cmp_u64);
        uint64_t median = timings[NUM_BATCHES / 2] / OPS_PER_BATCH;
        uint64_t p10    = timings[NUM_BATCHES / 10] / OPS_PER_BATCH;
        uint64_t p90    = timings[NUM_BATCHES * 9 / 10] / OPS_PER_BATCH;

        printf("  %-12s  median: %4" PRIu64 " cyc/op   "
               "[p10: %4" PRIu64 ",  p90: %4" PRIu64 "]\n",
               name, median, p10, p90);
}


/* ══════════════════════════════════════════════════════════════════
 *  MAIN
 * ══════════════════════════════════════════════════════════════════ */
int main(void) {
        printf("╔═══════════════════════════════════════════════════════════╗\n");
        printf("║  Curve25519 Field Square — 3-MAC vs 1-MAC Benchmark      ║\n");
        printf("╚═══════════════════════════════════════════════════════════╝\n\n");

        uint64_t bench_input[5] = {
                0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                0x000216936D3CD6E5ULL
        };

        sq_test_t tests[] = {
                { "identity",      { 1, 0, 0, 0, 0 } },
                { "small",         { 7, 11, 3, 5, 2 } },
                { "near-max",      { MASK51, MASK51, MASK51, MASK51, MASK51 } },
                { "basepoint",     { 0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                                     0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                                     0x000216936D3CD6E5ULL } },
                { "alternating",   { MASK51, 0, MASK51, 0, MASK51 } },
        };
        int n = sizeof(tests) / sizeof(tests[0]);

        /* ── Phase 1: 3-MAC hook ────────────────────────────────── */

        printf("Installing 3-MAC hook (18 triads, 0x0cd8 → 0x7c00)...\n\n");
        install_mac3();

        printf("─── Correctness: mac3 (3-MAC/vmwrite) ───\n\n");
        int mac3_ok = 1;
        for (int i = 0; i < n; i++) {
                uint64_t ref[5];
                fe_sq_ref(tests[i].in, ref);
                printf("  %s\n", tests[i].name);
                print_limbs("ref:", ref);
                mac3_ok &= test_one("mac3:", fe_sq_mac3, &tests[i], ref);
                printf("\n");
        }
        mac3_ok &= test_iterated("mac3:", fe_sq_mac3);
        printf("\n");

        if (!mac3_ok) {
                printf("*** mac3 CORRECTNESS FAILURE — check register usage ***\n");
                printf("    If crash/wrong results, try R10 or R11 instead of R9\n");
                printf("    for the third operand pair.\n");
                return 1;
        }

        printf("─── Performance: mac3 ───\n\n");
        bench_one("ref (C)", fe_sq_ref, bench_input);
        bench_one("mac3", fe_sq_mac3, bench_input);
        printf("\n");

        /* ── Phase 2: re-install 1-MAC hook, benchmark for comparison */

        printf("Re-installing 1-MAC hook (6 triads)...\n\n");
        install_mac128();

        /* Quick sanity check */
        {
                uint64_t ref[5], out[5];
                fe_sq_ref(bench_input, ref);
                fe_sq_mac128(bench_input, out);
                if (!fe_equal(ref, out)) {
                        printf("*** mac128 sanity check FAILED ***\n");
                        return 1;
                }
        }

        printf("─── Performance: mac128 (1-MAC, baseline) ───\n\n");
        bench_one("ref (C)", fe_sq_ref, bench_input);
        bench_one("mac128", fe_sq_mac128, bench_input);
        printf("\n");

        printf("═══════════════════════════════════════════\n");
        printf("  Summary\n");
        printf("═══════════════════════════════════════════\n");
        printf("  mac128:  15 vmwrite × 6 triads  = 90 triads, 15 CAM redirects\n");
        printf("  mac3:     5 vmwrite × 18 triads = 90 triads,  5 CAM redirects\n");
        printf("  Savings: 10 fewer CAM redirect overheads\n\n");

        return 0;
}
