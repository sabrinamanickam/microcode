/*
 * mac3_curve25519.c — Batched 3×MAC128 per vmwrite for Curve25519
 *
 * ═══════════════════════════════════════════════════════════════════
 *  3-MAC BATCHED HOOK (19 triads)
 *
 *  One vmwrite invocation performs three multiply-accumulates.
 *  Operand pairs passed via registers, microcode shuffles internally.
 *
 *  Register convention:
 *    INPUT:   RCX = pair1_x   RDX = pair1_y   (vmwrite operands)
 *             RSI = pair2_x   RDI = pair2_y
 *             RBX = pair3_x   R8  = pair3_y
 *             RAX = acc_lo initial (carry_in or 0)
 *    OUTPUT:  RAX = acc_lo    R8  = acc_hi
 *    CLOBBER: RCX, RDX, RSI, RDI, RBX (all inputs consumed)
 *
 *  Microcode flow:
 *    T0:      save R8 (pair3_y) → TMP0,  zero R8 (acc_hi)
 *    T1–T6:   MAC 1 with RCX × RDX
 *    T6:      also loads RSI → RCX, RDI → RDX  (pair 2)
 *    T7–T12:  MAC 2 with new RCX × RDX
 *    T12:     also loads RBX → RCX, TMP0 → RDX  (pair 3)
 *    T13–T18: MAC 3 with new RCX × RDX, END
 *
 *  Hook:   0x0cd8 → patch 0x7c00
 *  Trigger: vmwrite rcx, rdx
 *
 *  Curve25519 square: 5 limbs × 1 vmwrite = 5 calls total.
 *  Expected: ~5×(5 redirect + ~19 exec) = ~120 cycles
 *  vs 15-MAC version: ~135 cycles measured
 *
 *  Build:  make PROG=mac3_curve25519
 *  Run:    sudo taskset -c 0 ./mac3_curve25519_static
 * ═══════════════════════════════════════════════════════════════════
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
 *  3-MAC BATCHED HOOK — 19 triads
 *
 *  Each MAC is the proven 6-triad carry chain:
 *    save_acc, MUL, ADD+AND+OR, NOTAND+ADD_hi, OR, SHR, ADD_carry
 *
 *  Between MACs, operand shuffle is free (merged into carry fold
 *  triad, slot 1 and slot 2).
 *
 *  TMP register map:
 *    TMP0 = pair3_y (saved from R8 in T0, consumed in T12)
 *    TMP1 = a & b (carry chain, reused each MAC)
 *    TMP2 = a | b (carry chain, reused each MAC)
 *    TMP3 = old acc_lo (carry chain, reused each MAC)
 *    TMP4 = ~sum & (a|b) (carry chain, reused each MAC)
 *    TMP5 = carry bit (carry chain, reused each MAC)
 * ══════════════════════════════════════════════════════════════════ */
void install_mac3(void) {
        ucode_t mac3_patch[] = {

                /* ── T0: Init — save pair3_y, zero acc_hi ──────── */
                {
                        ZEROEXT_DSZ64_DR(TMP0, R8),      /* TMP0 = pair3_y */
                        MOVE_DSZ64_DI(R8, 0),             /* acc_hi = 0 */
                        NOP,
                        NOP_SEQWORD
                },

                /* ══ MAC 1: RCX × RDX (vmwrite operands) ══════ */

                /* T1: save acc_lo + multiply */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T2: sum + carry operands */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T3: ~sum & (a|b) + acc_hi += prod_hi */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T4: merge carry chain */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T5: extract carry bit */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T6: fold carry + LOAD PAIR 2 */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        ZEROEXT_DSZ64_DR(RCX, RSI),       /* pair2_x → RCX */
                        ZEROEXT_DSZ64_DR(RDX, RDI),       /* pair2_y → RDX */
                        NOP_SEQWORD
                },

                /* ══ MAC 2: RSI × RDI (now in RCX × RDX) ═════ */

                /* T7 */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T8 */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T9 */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T10 */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T11 */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T12: fold carry + LOAD PAIR 3 */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        ZEROEXT_DSZ64_DR(RCX, RBX),       /* pair3_x → RCX */
                        ZEROEXT_DSZ64_DR(RDX, TMP0),      /* pair3_y → RDX */
                        NOP_SEQWORD
                },

                /* ══ MAC 3: RBX × pair3_y (now in RCX × RDX) ═ */

                /* T13 */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T14 */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T15 */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T16 */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T17 */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T18: fold carry, done */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c20, mac3_patch, 19);
        hook_match_and_patch(0, 0x0cd8, 0x7c20);
}


/* ══════════════════════════════════════════════════════════════════
 *  MAC3 UNIT TESTS — validate the 3-MAC batch hook
 * ══════════════════════════════════════════════════════════════════ */

typedef struct {
        const char *name;
        uint64_t acc_in;
        uint64_t p1x, p1y, p2x, p2y, p3x, p3y;
        uint64_t exp_lo, exp_hi;
} mac3_test_t;

static int run_mac3_test(const mac3_test_t *t) {
        uint64_t c_lo, c_hi;

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
                : [cin]  "r"(t->acc_in),
                  [_p1x] "r"(t->p1x), [_p1y] "r"(t->p1y),
                  [_p2x] "r"(t->p2x), [_p2y] "r"(t->p2y),
                  [_p3x] "r"(t->p3x), [_p3y] "r"(t->p3y)
                : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8"
        );

        int pass = (c_lo == t->exp_lo && c_hi == t->exp_hi);
        printf("%s:\n", t->name);
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")%s\n",
               c_lo, t->exp_lo, (c_lo == t->exp_lo) ? "" : " ✗");
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")%s\n",
               c_hi, t->exp_hi, (c_hi == t->exp_hi) ? "" : " ✗");
        printf("  %s\n\n", pass ? "✓ PASS" : "✗ FAIL");
        return pass;
}


static void test_mac3_unit(void) {
        printf("═══════════════════════════════════════════\n");
        printf("  MAC3 Unit Tests (3×MAC128 batched)\n");
        printf("═══════════════════════════════════════════\n\n");

        /* ref: acc += p1x*p1y + p2x*p2y + p3x*p3y */
        mac3_test_t tests[] = {
                {
                        .name = "MAC3 1: small (7×13 + 11×17 + 3×5) = 293",
                        .acc_in = 0,
                        .p1x = 7,  .p1y = 13,
                        .p2x = 11, .p2y = 17,
                        .p3x = 3,  .p3y = 5,
                        .exp_lo = 293, .exp_hi = 0
                },
                {
                        .name = "MAC3 2: with carry-in (100 + 7×13 + 11×17 + 3×5) = 393",
                        .acc_in = 100,
                        .p1x = 7,  .p1y = 13,
                        .p2x = 11, .p2y = 17,
                        .p3x = 3,  .p3y = 5,
                        .exp_lo = 393, .exp_hi = 0
                },
                {
                        .name = "MAC3 3: max×max + 1×1 + 1×1",
                        .acc_in = 0,
                        .p1x = 0xFFFFFFFFFFFFFFFFULL,
                        .p1y = 0xFFFFFFFFFFFFFFFFULL,
                        .p2x = 1, .p2y = 1,
                        .p3x = 1, .p3y = 1,
                        /* max*max = FFFFFFFFFFFFFFFE:0000000000000001
                         * + 1 + 1 = FFFFFFFFFFFFFFFE:0000000000000003 */
                        .exp_lo = 0x0000000000000003ULL,
                        .exp_hi = 0xFFFFFFFFFFFFFFFEULL
                },
                {
                        .name = "MAC3 4: Curve25519 limb products",
                        .acc_in = 0,
                        .p1x = 0x0007FFFFFFFFFFFFULL,  /* a0 */
                        .p1y = 0x0007FFFFFFFFFFFFULL,  /* a0 */
                        .p2x = 0x0002000000000000ULL,   /* d1 */
                        .p2y = 0x0000002540BE3F79ULL,   /* r4=19*a4 */
                        .p3x = 0x0004000000000000ULL,   /* d2 */
                        .p3y = 0x0000001D1A94A1FFULL,   /* r3=19*a3 */
                        /* compute reference */
                        .exp_lo = 0, .exp_hi = 0  /* filled below */
                },
                {
                        .name = "MAC3 5: carry from low to high",
                        .acc_in = 0xFFFFFFFFFFFFFFF0ULL,
                        .p1x = 1, .p1y = 0x20,
                        .p2x = 0, .p2y = 0,
                        .p3x = 0, .p3y = 0,
                        /* 0xFFFFFFFFFFFFFFF0 + 0x20 = 0x10, carry */
                        .exp_lo = 0x0000000000000010ULL,
                        .exp_hi = 0x0000000000000001ULL
                },
        };

        /* Fill test 4 reference */
        __uint128_t ref4 = (__uint128_t)tests[3].p1x * tests[3].p1y
                         + (__uint128_t)tests[3].p2x * tests[3].p2y
                         + (__uint128_t)tests[3].p3x * tests[3].p3y;
        tests[3].exp_lo = (uint64_t)ref4;
        tests[3].exp_hi = (uint64_t)(ref4 >> 64);

        int n = sizeof(tests) / sizeof(tests[0]);
        int pass = 0;
        for (int i = 0; i < n; i++)
                pass += run_mac3_test(&tests[i]);

        printf("════════════════════════════════════════\n");
        printf("  MAC3 Unit: %d / %d passed\n", pass, n);
        printf("════════════════════════════════════════\n\n");
}


/* ══════════════════════════════════════════════════════════════════
 *  REFERENCE C — Curve25519 field square
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
 *  MAC3 CURVE25519 SQUARE — 5 vmwrite calls
 *
 *  Products per limb (3 MACs each):
 *
 *   c[0] = a0·a0 + d1·r4 + d2·r3       pair1=(a0,a0) pair2=(d1,r4) pair3=(d2,r3)
 *   c[1] = d0·a1 + r3·a3 + d2·r4       pair1=(d0,a1) pair2=(r3,a3) pair3=(d2,r4)
 *   c[2] = d0·a2 + a1·a1 + d3·r4       pair1=(d0,a2) pair2=(a1,a1) pair3=(d3,r4)
 *   c[3] = d0·a3 + d1·a2 + r4·a4       pair1=(d0,a3) pair2=(d1,a2) pair3=(r4,a4)
 *   c[4] = d0·a4 + d1·a3 + a2·a2       pair1=(d0,a4) pair2=(d1,a3) pair3=(a2,a2)
 *
 *  Register map per vmwrite call:
 *   RAX = carry_in (0 for limb 0)
 *   RCX = pair1_x   RDX = pair1_y
 *   RSI = pair2_x   RDI = pair2_y
 *   RBX = pair3_x   R8  = pair3_y
 * ══════════════════════════════════════════════════════════════════ */
__attribute__((noinline))
void fe_sq_mac3(const uint64_t *a, uint64_t *out) {
        uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
        uint64_t d0 = 2*a0, d1 = 2*a1, d2 = 2*a2, d3 = 2*a3;
        uint64_t r3 = 19*a3, r4 = 19*a4;

        uint64_t c_lo, c_hi, carry;

        /* ── c[0] = a0·a0 + d1·r4 + d2·r3  (no carry in) ─────── */
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

        /* ── c[1] = carry + d0·a1 + r3·a3 + d2·r4 ────────────── */
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
                : [cin]  "r"(carry),
                  [_p1x] "r"(d0),  [_p1y] "r"(a1),
                  [_p2x] "r"(r3),  [_p2y] "r"(a3),
                  [_p3x] "r"(d2),  [_p3y] "r"(r4)
                : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[1] = c_lo & MASK51;

        /* ── c[2] = carry + d0·a2 + a1·a1 + d3·r4 ────────────── */
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
                : [cin]  "r"(carry),
                  [_p1x] "r"(d0),  [_p1y] "r"(a2),
                  [_p2x] "r"(a1),  [_p2y] "r"(a1),
                  [_p3x] "r"(d3),  [_p3y] "r"(r4)
                : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[2] = c_lo & MASK51;

        /* ── c[3] = carry + d0·a3 + d1·a2 + r4·a4 ────────────── */
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
                : [cin]  "r"(carry),
                  [_p1x] "r"(d0),  [_p1y] "r"(a3),
                  [_p2x] "r"(d1),  [_p2y] "r"(a2),
                  [_p3x] "r"(r4),  [_p3y] "r"(a4)
                : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[3] = c_lo & MASK51;

        /* ── c[4] = carry + d0·a4 + d1·a3 + a2·a2 ────────────── */
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
                : [cin]  "r"(carry),
                  [_p1x] "r"(d0),  [_p1y] "r"(a4),
                  [_p2x] "r"(d1),  [_p2y] "r"(a3),
                  [_p3x] "r"(a2),  [_p3y] "r"(a2)
                : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[4] = c_lo & MASK51;

        /* ── Final reduction ──────────────────────────────────── */
        out[0] += carry * 19;
        carry = out[0] >> 51;
        out[0] &= MASK51;
        out[1] += carry;
}


/* ══════════════════════════════════════════════════════════════════
 *  CURVE25519 SQUARE TESTS
 * ══════════════════════════════════════════════════════════════════ */

typedef struct {
        const char *name;
        uint64_t in[5];
} sq_test_t;

static int run_sq_test(const sq_test_t *t) {
        uint64_t ref[5], mac[5];

        fe_sq_ref(t->in, ref);
        fe_sq_mac3(t->in, mac);

        int pass = (memcmp(ref, mac, sizeof(ref)) == 0);

        printf("%s:\n", t->name);
        printf("  ref:  [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ",\n"
               "         %016" PRIx64 ", %016" PRIx64 "]\n",
               ref[0], ref[1], ref[2], ref[3], ref[4]);
        printf("  mac3: [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ",\n"
               "         %016" PRIx64 ", %016" PRIx64 "]\n",
               mac[0], mac[1], mac[2], mac[3], mac[4]);

        if (!pass) {
                printf("  MISMATCH:");
                for (int i = 0; i < 5; i++)
                        if (ref[i] != mac[i])
                                printf(" limb[%d]", i);
                printf("\n");
        }
        printf("  %s\n\n", pass ? "✓ PASS" : "✗ FAIL");
        return pass;
}

static int test_iterated(void) {
        printf("═══════════════════════════════════════════\n");
        printf("  Iterated Square Test (1000 rounds)\n");
        printf("═══════════════════════════════════════════\n\n");

        uint64_t ref[5] = { 1, 0, 0, 0, 0 };
        uint64_t mac[5] = { 1, 0, 0, 0, 0 };

        for (int i = 0; i < 1000; i++) {
                uint64_t tmp_r[5], tmp_m[5];
                fe_sq_ref(ref, tmp_r);
                fe_sq_mac3(mac, tmp_m);
                memcpy(ref, tmp_r, sizeof(ref));
                memcpy(mac, tmp_m, sizeof(mac));
        }

        int pass = (memcmp(ref, mac, sizeof(ref)) == 0);
        printf("  After 1000 iterations:\n");
        printf("  ref:  [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ",\n"
               "         %016" PRIx64 ", %016" PRIx64 "]\n",
               ref[0], ref[1], ref[2], ref[3], ref[4]);
        printf("  mac3: [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ",\n"
               "         %016" PRIx64 ", %016" PRIx64 "]\n",
               mac[0], mac[1], mac[2], mac[3], mac[4]);
        printf("  %s\n\n", pass ? "✓ PASS" : "✗ FAIL");
        return pass;
}


/* ══════════════════════════════════════════════════════════════════ */

int main(void) {
        printf("╔════════════════════════════════════════════════════╗\n");
        printf("║  3-MAC Batched Curve25519 Square — Goldmont        ║\n");
        printf("╚════════════════════════════════════════════════════╝\n\n");

        printf("Installing MAC3 hook (19 triads, 0x0cd8 → 0x7c00)...\n\n");
        install_mac3();

        /* ── MAC3 unit tests ────────────────────────────────────── */

        test_mac3_unit();

        /* ── Curve25519 square tests ────────────────────────────── */

        printf("═══════════════════════════════════════════\n");
        printf("  Curve25519 Square Tests (MAC3, 5 calls)\n");
        printf("═══════════════════════════════════════════\n\n");

        sq_test_t tests[] = {
                { .name = "SQ 1: identity (1,0,0,0,0)²",
                  .in   = { 1, 0, 0, 0, 0 } },
                { .name = "SQ 2: small (7,11,3,5,2)²",
                  .in   = { 7, 11, 3, 5, 2 } },
                { .name = "SQ 3: mid-range limbs",
                  .in   = { 0x1234567890ULL,
                            0x0ABCDEF01234ULL,
                            0x0000F00DCAFEULL,
                            0x0007000000000ULL,
                            0x00055555555555ULL } },
                { .name = "SQ 4: near-max 51-bit limbs",
                  .in   = { MASK51, MASK51, MASK51, MASK51, MASK51 } },
                { .name = "SQ 5: basepoint x",
                  .in   = { 0x00062D608F25D51AULL,
                            0x000412A4B4F6592AULL,
                            0x00075B7171A4B31DULL,
                            0x0001FF60527118FEULL,
                            0x000216936D3CD6E5ULL } },
                { .name = "SQ 6: one-hot limb[2]",
                  .in   = { 0, 0, 1, 0, 0 } },
                { .name = "SQ 7: alternating (max, 0, max, 0, max)",
                  .in   = { MASK51, 0, MASK51, 0, MASK51 } },
                { .name = "SQ 8: powers of two",
                  .in   = { 1ULL << 10, 1ULL << 20,
                            1ULL << 30, 1ULL << 40, 1ULL << 50 } },
        };

        int n = sizeof(tests) / sizeof(tests[0]);
        int pass = 0;
        for (int i = 0; i < n; i++)
                pass += run_sq_test(&tests[i]);

        int iter_pass = test_iterated();

        int total = pass + iter_pass;
        int total_n = n + 1;

        printf("════════════════════════════════════════\n");
        printf("  TOTAL: %d / %d passed\n", total, total_n);
        printf("════════════════════════════════════════\n\n");

        if (total == total_n) {
                printf("MAC3 curve25519_square is operational!\n\n");
                printf("  Hook:     19 triads, 3×MAC128 per vmwrite\n");
                printf("  Calls:    5 vmwrite per square\n");
                printf("  Redirect: 5 × ~5 = ~25 cycles overhead\n");
                printf("  vs 15-MAC: 15 × ~5 = ~75 cycles overhead\n");
                printf("  Savings:  ~50 cycles redirect overhead\n\n");
                printf("  Next: run bench_mac3_curve25519 for timing.\n");
        }

        return (total == total_n) ? 0 : 1;
}
