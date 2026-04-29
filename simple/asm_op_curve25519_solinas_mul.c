/*
 * asm_op_curve25519_solinas_mul.c — Solinas multiplication for curve25519 via microcode
 *
 * Field: GF(2^255 - 19), saturated 4 x 64-bit limbs.
 * Reduction constant: 38 (since 2^256 ≡ 38 mod p).
 *
 * Algorithm:
 *   1. Full 4x4 schoolbook multiplication → 8-word (512-bit) product
 *   2. Multiply upper 4 words by 38, add to lower 4 words
 *   3. Final carry * 38 fold-back
 *   4. Conditional subtract of p (for full reduction)
 *
 * Single vmwrite patch (127 triads):
 *   PREP(2) + 4 * [START(1) + BODY(22)] + 3 * SHIFT(2) + REDUCE(27) = 127
 *
 * For multiplication: a and b are distinct.
 *   b values:  RSI=b[0]  R12=b[1]  R11=b[2]  R14=b[3]  →  TMP10-13 via PREP
 *   a values:  RDI=a[0]  RDX→R14=a[1]  RBP→TMP15=a[2]  RBX=a[3]
 *   (PREP saves RDX→R14 and RBP→TMP15 before b[3] overwrites R14)
 *
 * Register convention:
 *   Accumulator: R15=acc[0]  R9=acc[1]  R10=acc[2]  R13=acc[3]  RAX=acc[4]
 *   b values:    TMP10=b[0]  TMP11=b[1]  TMP12=b[2]  TMP13=b[3]
 *   a values:    RDI=a[0] (row 0), R14=a[1] (saved in PREP),
 *                TMP15=a[2] (saved in PREP), RBX=a[3]
 *   R8 = 38 (reduction constant)
 *
 * Product words saved in:  RSI=p[0]  R12=p[1]  R11=p[2]  R14→overwritten
 * After row 3: acc = {p[3], p[4], p[5], p[6], p[7]}
 *
 * Build:  make PROG=asm_op_curve25519_solinas_mul
 * Run:    sudo taskset -c 0 ./asm_op_curve25519_solinas_mul_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* p = 2^255 - 19 */
static const uint64_t CURVE25519_P[4] = {
    UINT64_C(0xFFFFFFFFFFFFFFED), UINT64_C(0xFFFFFFFFFFFFFFFF),
    UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x7FFFFFFFFFFFFFFF)
};

/* ── fiat-crypto reference (the REAL GCC baseline CryptOpt compares against) ── */
#include "../curvesC/curve25519_solinas_mul.c"

static void fe_mul_fiat(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    fiat_curve25519_solinas_mul(out, a, b);
}

/* ── native C reference (schoolbook + Solinas) ──────────────────── */

static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    __uint128_t t;
    uint64_t prod[8] = {0};

    /* Full 4x4 schoolbook: prod[0..7] = a * b */
    for (int i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 4; j++) {
            t = (__uint128_t)a[i] * b[j] + prod[i+j] + carry;
            prod[i+j] = (uint64_t)t;
            carry = (uint64_t)(t >> 64);
        }
        prod[i+4] += carry;
    }

    /* Solinas reduction: multiply upper half by 38, add to lower */
    uint64_t r[4];
    uint64_t c = 0;
    for (int i = 0; i < 4; i++) {
        t = (__uint128_t)38 * prod[i+4] + prod[i] + c;
        r[i] = (uint64_t)t;
        c = (uint64_t)(t >> 64);
    }

    /* Final fold: carry * 38 */
    t = (__uint128_t)38 * c + r[0];
    r[0] = (uint64_t)t;
    c = (uint64_t)(t >> 64);
    r[1] += c; c = (r[1] < c);
    r[2] += c; c = (r[2] < c);
    r[3] += c;

    /* Conditional subtract of p */
    uint64_t diff[4];
    __uint128_t b128;
    b128 = (__uint128_t)r[0] - CURVE25519_P[0];
    diff[0] = (uint64_t)b128;
    b128 = (__uint128_t)r[1] - CURVE25519_P[1] - ((uint64_t)(b128 >> 64) & 1);
    diff[1] = (uint64_t)b128;
    b128 = (__uint128_t)r[2] - CURVE25519_P[2] - ((uint64_t)(b128 >> 64) & 1);
    diff[2] = (uint64_t)b128;
    b128 = (__uint128_t)r[3] - CURVE25519_P[3] - ((uint64_t)(b128 >> 64) & 1);
    diff[3] = (uint64_t)b128;
    uint64_t borrow = (uint64_t)(b128 >> 64) & 1;
    uint64_t mask = (uint64_t)0 - borrow;
    out[0] = (r[0] & mask) | (diff[0] & ~mask);
    out[1] = (r[1] & mask) | (diff[1] & ~mask);
    out[2] = (r[2] & mask) | (diff[2] & ~mask);
    out[3] = (r[3] & mask) | (diff[3] & ~mask);
}

/* ── independent reference (same algorithm, separate implementation) ── */

static void fe_mul_reference(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    __uint128_t t;
    uint64_t prod[8] = {0};

    for (int i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 4; j++) {
            t = (__uint128_t)a[i] * b[j] + prod[i+j] + carry;
            prod[i+j] = (uint64_t)t;
            carry = (uint64_t)(t >> 64);
        }
        prod[i+4] += carry;
    }

    uint64_t r[4];
    uint64_t c = 0;
    for (int i = 0; i < 4; i++) {
        t = (__uint128_t)38 * prod[i+4] + prod[i] + c;
        r[i] = (uint64_t)t;
        c = (uint64_t)(t >> 64);
    }

    t = (__uint128_t)38 * c + r[0];
    r[0] = (uint64_t)t;
    c = (uint64_t)(t >> 64);
    r[1] += c; c = (r[1] < c);
    r[2] += c; c = (r[2] < c);
    r[3] += c;

    uint64_t diff[4];
    __uint128_t b128;
    b128 = (__uint128_t)r[0] - CURVE25519_P[0];
    diff[0] = (uint64_t)b128;
    b128 = (__uint128_t)r[1] - CURVE25519_P[1] - ((uint64_t)(b128 >> 64) & 1);
    diff[1] = (uint64_t)b128;
    b128 = (__uint128_t)r[2] - CURVE25519_P[2] - ((uint64_t)(b128 >> 64) & 1);
    diff[2] = (uint64_t)b128;
    b128 = (__uint128_t)r[3] - CURVE25519_P[3] - ((uint64_t)(b128 >> 64) & 1);
    diff[3] = (uint64_t)b128;
    uint64_t borrow = (uint64_t)(b128 >> 64) & 1;
    uint64_t mask = (uint64_t)0 - borrow;
    out[0] = (r[0] & mask) | (diff[0] & ~mask);
    out[1] = (r[1] & mask) | (diff[1] & ~mask);
    out[2] = (r[2] & mask) | (diff[2] & ~mask);
    out[3] = (r[3] & mask) | (diff[3] & ~mask);
}

/* ── microcode patch ──────────────────────────────────────────── */

/*
 * SCHOOLBOOK_ROW_START: first triad of a schoolbook row.
 * Sets RDX = a_i for first MUL and RDI = a_i for subsequent MULs.
 * For mul: row 0 uses RDI (a[0]), rows 1-3 use R14/TMP15/RBX.
 */
#define SCHOOLBOOK_ROW_START(a_src) \
    { ZEROEXT_DSZ64_DR(RDX, a_src), ZEROEXT_DSZ64_DR(RDI, a_src), \
      NOP, NOP_SEQWORD }

/*
 * SCHOOLBOOK_ROW_BODY: Phase A (13 triads) + Phase A' (9 triads) = 22 triads.
 * Identical to squaring: multiplies RDI by b[0..3] (TMP10-13),
 * accumulates into R15/R9/R10/R13/RAX.  TMP14 = extra carry.
 */
#define SCHOOLBOOK_ROW_BODY \
    /* ── Phase A: schoolbook RDI × TMP10..TMP13 (13 triads) ── */ \
    { MUL_DSZ64_DRR(RCX, TMP10, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP0, RDX), ZEROEXT_DSZ64_DR(TMP1, RCX), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP11, RDX), ADD_DSZ64_DRR(TMP0, R15, TMP0), \
      SETCC_CONDB_DR(TMP3, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP2, TMP1, RDX), SETCC_CONDB_DR(TMP8, TMP2), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    /* writeback w0→R15 + early w1 start */ \
    { ZEROEXT_DSZ64_DR(RDX, RDI), ZEROEXT_DSZ64_DR(R15, TMP0), \
      ADD_DSZ64_DRR(TMP0, R9, TMP2), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP12, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* b3 MUL merge: save hi(b2), reload RDX, merge ADD into MUL */ \
    { ADD_DSZ64_DRR(TMP4, TMP1, RDX), SETCC_CONDB_DR(TMP5, TMP4), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP8), SETCC_CONDB_DR(TMP6, TMP4), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP13, RDX), ADD_DSZ64_DRR(TMP8, TMP5, TMP6), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP5, TMP1, RDX), SETCC_CONDB_DR(TMP6, TMP5), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP5, TMP5, TMP8), SETCC_CONDB_DR(TMP7, TMP5), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP8, TMP6, TMP7), ADD_DSZ64_DRR(TMP6, RCX, TMP8), \
      NOP, NOP_SEQWORD }, \
    /* ── Phase A': accumulate (8 triads) ── */ \
    /* w1 triple-pack (SETCC reads TMP0 flags from early start) */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R9, TMP0), \
      ADD_DSZ64_DRR(TMP0, R10, TMP4), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R10, TMP0), \
      ADD_DSZ64_DRR(TMP0, R13, TMP5), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R13, TMP0), \
      ADD_DSZ64_DRR(TMP0, RAX, TMP6), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP14, TMP1, TMP8), ZEROEXT_DSZ64_DR(RAX, TMP0), \
      NOP, NOP_SEQWORD }

/*
 * Shift macro: save product word, slide accumulator (2 triads).
 * RDI is set by the NEXT row's SCHOOLBOOK_ROW_START.
 */
#define SHIFT_ROW(save_reg) \
    { ZEROEXT_DSZ64_DR(save_reg, R15), ZEROEXT_DSZ64_DR(R15, R9), \
      ZEROEXT_DSZ64_DR(R9, R10), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(R10, R13), ZEROEXT_DSZ64_DR(R13, RAX), \
      ZEROEXT_DSZ64_DR(RAX, TMP14), NOP_SEQWORD }

static void install_solinas_mul_patch(void) {
    ucode_t patch[] = {

    /* ═══ PREP: copy b values to TMP10-13, save a[1]→R14, a[2]→TMP15 ═══ */
    /*
     * At entry:
     *   RSI=b[0]  R12=b[1]  R11=b[2]  R14=b[3]  (b values in arch regs)
     *   RDI=a[0]  RDX=a[1]  RBP=a[2]  RBX=a[3]  (a values)
     *   R8=38  R15=0  R9=0  R10=0  R13=0  RAX=0
     *
     * After PREP:
     *   TMP10=b[0]  TMP11=b[1]  TMP12=b[2]  TMP13=b[3]
     *   R14=a[1] (overwritten from b[3])  TMP15=a[2]
     *   RBX=a[3] (unchanged)  RDI=a[0] (unchanged)
     */
    { ZEROEXT_DSZ64_DR(TMP10, RSI), ZEROEXT_DSZ64_DR(TMP11, R12),
      ZEROEXT_DSZ64_DR(TMP12, R11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R14), ZEROEXT_DSZ64_DR(R14, RDX),
      ZEROEXT_DSZ64_DR(TMP15, RBP), NOP_SEQWORD },

    /* ═══ ROW 0: a[0] × b[0..3] (23 triads) ═══ */
    SCHOOLBOOK_ROW_START(RDI),  /* RDI = a[0] from inline asm */
    SCHOOLBOOK_ROW_BODY,
    SHIFT_ROW(RSI),             /* save product[0] → RSI */

    /* ═══ ROW 1: a[1] × b[0..3] (23 triads) ═══ */
    SCHOOLBOOK_ROW_START(R14),  /* a[1] saved in R14 by PREP */
    SCHOOLBOOK_ROW_BODY,
    SHIFT_ROW(R12),             /* save product[1] → R12 */

    /* ═══ ROW 2: a[2] × b[0..3] (23 triads) ═══ */
    SCHOOLBOOK_ROW_START(TMP15),  /* a[2] saved in TMP15 by PREP */
    SCHOOLBOOK_ROW_BODY,
    SHIFT_ROW(R11),             /* save product[2] → R11 */

    /* ═══ ROW 3: a[3] × b[0..3] (23 triads) ═══ */
    SCHOOLBOOK_ROW_START(RBX),  /* a[3] in RBX from inline asm */
    SCHOOLBOOK_ROW_BODY,
    /* No shift: R15=p[3], R9=p[4], R10=p[5], R13=p[6], RAX=p[7], TMP14=carry8
     * Saved: RSI=p[0], R12=p[1], R11=p[2]
     */

    /* ═══ SOLINAS REDUCTION (Fiat-style two-phase, 27 triads) ═══ */
    /*
     * Step 1: Multiply product[4..7] by 38 using MUL-by-immediate.
     * MUL_DSZ64_DIR(hi_out, imm, src_and_lo_out): src overwritten with lo.
     * Also fold TMP14 (carry8) into RAX before multiplying.
     *
     * After: R9=lo4, R10=lo5, R13=lo6, RAX=lo7
     *        TMP0=hi4, TMP1=hi5, TMP2=hi6, RCX=hi7
     */
    { ADD_DSZ64_DRR(RAX, RAX, TMP14), MUL_DSZ64_DIR(RCX, 38, R9),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP0, RCX), MUL_DSZ64_DIR(RCX, 38, R10),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP1, RCX), MUL_DSZ64_DIR(RCX, 38, R13),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP2, RCX), MUL_DSZ64_DIR(RCX, 38, RAX),
      NOP, NOP_SEQWORD },

    /*
     * Step 2, Phase 1: add lo(38*p[5..7]) to product[1..3]
     */
    /* w1: R12 += R10 (lo5), no carry in */
    { ADD_DSZ64_DRR(TMP4, R12, R10), SETCC_CONDB_DR(TMP3, TMP4),
      ZEROEXT_DSZ64_DR(R12, TMP4), NOP_SEQWORD },
    /* w2: R11 += R13 (lo6) + carry */
    { ADD_DSZ64_DRR(TMP4, R11, R13), SETCC_CONDB_DR(TMP5, TMP4),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP3), SETCC_CONDB_DR(TMP6, TMP4),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP5, TMP6), ZEROEXT_DSZ64_DR(R11, TMP4),
      ADD_DSZ64_DRR(TMP4, R15, RAX), NOP_SEQWORD },
    /* w3: R15 += RAX (lo7) + carry — triple-pack */
    { SETCC_CONDB_DR(TMP5, TMP4), ADD_DSZ64_DRR(TMP4, TMP4, TMP3),
      SETCC_CONDB_DR(TMP6, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP5, TMP6), ZEROEXT_DSZ64_DR(R15, TMP4),
      NOP, NOP_SEQWORD },
    /* overflow: carry from w3 + hi7(RCX) */
    { ADD_DSZ64_DRR(TMP7, TMP3, RCX), NOP, NOP, NOP_SEQWORD },

    /*
     * Step 2, Phase 2: add lo4(R9) + hi(38*p[4..6]) to result[0..3]
     */
    /* w0: RSI += R9 (lo4), no carry in */
    { ADD_DSZ64_DRR(TMP4, RSI, R9), SETCC_CONDB_DR(TMP3, TMP4),
      ZEROEXT_DSZ64_DR(RSI, TMP4), NOP_SEQWORD },
    /* w1: R12 += TMP0 (hi4) + carry */
    { ADD_DSZ64_DRR(TMP4, R12, TMP0), SETCC_CONDB_DR(TMP5, TMP4),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP3), SETCC_CONDB_DR(TMP6, TMP4),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP5, TMP6), ZEROEXT_DSZ64_DR(R12, TMP4),
      ADD_DSZ64_DRR(TMP4, R11, TMP1), NOP_SEQWORD },
    /* w2: R11 += TMP1 (hi5) + carry — triple-pack */
    { SETCC_CONDB_DR(TMP5, TMP4), ADD_DSZ64_DRR(TMP4, TMP4, TMP3),
      SETCC_CONDB_DR(TMP6, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP5, TMP6), ZEROEXT_DSZ64_DR(R11, TMP4),
      ADD_DSZ64_DRR(TMP4, R15, TMP2), NOP_SEQWORD },
    /* w3: R15 += TMP2 (hi6) + carry — triple-pack */
    { SETCC_CONDB_DR(TMP5, TMP4), ADD_DSZ64_DRR(TMP4, TMP4, TMP3),
      SETCC_CONDB_DR(TMP6, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP5, TMP6), ZEROEXT_DSZ64_DR(R15, TMP4),
      NOP, NOP_SEQWORD },

    /*
     * Step 3: Final fold — total_carry * 38 + result[0]
     */
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP7), NOP, NOP, NOP_SEQWORD },
    { MUL_DSZ64_DIR(RCX, 38, TMP3), NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RSI, TMP3), SETCC_CONDB_DR(TMP3, TMP4),
      ZEROEXT_DSZ64_DR(RSI, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, R12, TMP3), SETCC_CONDB_DR(TMP3, TMP4),
      ZEROEXT_DSZ64_DR(R12, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, R11, TMP3), SETCC_CONDB_DR(TMP3, TMP4),
      ZEROEXT_DSZ64_DR(R11, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R15, R15, TMP3), NOP, NOP, NOP_SEQWORD },

    /* Move to output convention: R15=r[0], R9=r[1], R10=r[2], R13=r[3] */
    { ZEROEXT_DSZ64_DR(R13, R15), ZEROEXT_DSZ64_DR(R15, RSI),
      ZEROEXT_DSZ64_DR(R9, R12), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R10, R11), NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("solinas_mul: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ── fe_mul via microcode ─────────────────────────────────────── */

static void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t r[4];
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_b   asm("rbx") = (uint64_t *)b;
    register uint64_t *_out asm("r15") = r;

    asm volatile(
        "push r15\n\t"
        "push rbp\n\t"
        "push rcx\n\t"

        /* Load b[0..3] → arch regs that PREP will copy to TMP10-13 */
        "mov rsi, [rbx]\n\t"        /* b[0] */
        "mov r12, [rbx + 8]\n\t"    /* b[1] */
        "mov r11, [rbx + 16]\n\t"   /* b[2] */
        "mov r14, [rbx + 24]\n\t"   /* b[3] */

        /* Load a[0..3] → RDI, RDX, RBP, RBX */
        "mov rdi, [rcx]\n\t"        /* a[0] → RDI (row 0 multiplier) */
        "mov rdx, [rcx + 8]\n\t"    /* a[1] → RDX (PREP saves to R14) */
        "mov rbp, [rcx + 16]\n\t"   /* a[2] → RBP (PREP saves to TMP15) */
        "mov rbx, [rcx + 24]\n\t"   /* a[3] → RBX (stays, used row 3) */

        /* Reduction constant */
        "mov r8, 38\n\t"

        /* Zero accumulator */
        "xor r15d, r15d\n\t"
        "xor r9d, r9d\n\t"
        "xor r10d, r10d\n\t"
        "xor r13d, r13d\n\t"
        "xor eax, eax\n\t"

        /* Fire microcode patch */
        "vmwrite rcx, rdx\n\t"

        /* Results: R15=r[0], R9=r[1], R10=r[2], R13=r[3] */
        /* Stack: [&r, old_rbp, a_ptr]. Pop in reverse order. */
        "pop rcx\n\t"       /* discard a_ptr */
        "pop rbp\n\t"       /* restore rbp */
        "pop rcx\n\t"       /* rcx = &r (output array) */
        "mov [rcx],      r15\n\t"
        "mov [rcx + 8],  r9\n\t"
        "mov [rcx + 16], r10\n\t"
        "mov [rcx + 24], r13\n\t"

        : "+r"(_a), "+r"(_b), "+r"(_out)
        :
        : "rax", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14",
          "memory", "cc"
    );

    /* Conditional subtract of p (done in native C) */
    uint64_t diff[4];
    __uint128_t b128;
    b128 = (__uint128_t)r[0] - CURVE25519_P[0];
    diff[0] = (uint64_t)b128;
    b128 = (__uint128_t)r[1] - CURVE25519_P[1] - ((uint64_t)(b128 >> 64) & 1);
    diff[1] = (uint64_t)b128;
    b128 = (__uint128_t)r[2] - CURVE25519_P[2] - ((uint64_t)(b128 >> 64) & 1);
    diff[2] = (uint64_t)b128;
    b128 = (__uint128_t)r[3] - CURVE25519_P[3] - ((uint64_t)(b128 >> 64) & 1);
    diff[3] = (uint64_t)b128;
    uint64_t borrow = (uint64_t)(b128 >> 64) & 1;
    uint64_t mask = (uint64_t)0 - borrow;
    out[0] = (r[0] & mask) | (diff[0] & ~mask);
    out[1] = (r[1] & mask) | (diff[1] & ~mask);
    out[2] = (r[2] & mask) | (diff[2] & ~mask);
    out[3] = (r[3] & mask) | (diff[3] & ~mask);
}

/* ── verification ────────────────────────────────────────────── */

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void rand_mod_p(uint64_t out[4], uint64_t *rng) {
    for (;;) {
        for (int j = 0; j < 4; j++) out[j] = splitmix64(rng);
        out[3] &= 0x7FFFFFFFFFFFFFFFULL;
        int lt = 0;
        for (int j = 3; j >= 0; j--) {
            if (out[j] < CURVE25519_P[j]) { lt = 1; break; }
            if (out[j] > CURVE25519_P[j]) break;
        }
        if (lt) break;
    }
}

static int verify_all(void) {
    int pass = 0, fail = 0;

    printf("--- Known vectors ---\n");
    struct { const char *name; uint64_t a[4]; uint64_t b[4]; } vecs[] = {
        { "0*0",
          {0, 0, 0, 0}, {0, 0, 0, 0} },
        { "1*1",
          {1, 0, 0, 0}, {1, 0, 0, 0} },
        { "0*1",
          {0, 0, 0, 0}, {1, 0, 0, 0} },
        { "2*3",
          {2, 0, 0, 0}, {3, 0, 0, 0} },
        { "38*1",
          {38, 0, 0, 0}, {1, 0, 0, 0} },
        { "(p-1)*2",
          {UINT64_C(0xFFFFFFFFFFFFFFEC), UINT64_C(0xFFFFFFFFFFFFFFFF),
           UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x7FFFFFFFFFFFFFFF)},
          {2, 0, 0, 0} },
        { "big*big",
          {UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0xFFFFFFFFFFFFFFFF),
           UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x3FFFFFFFFFFFFFFF)},
          {UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0xFFFFFFFFFFFFFFFF),
           UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x3FFFFFFFFFFFFFFF)} },
    };
    int nvecs = sizeof(vecs) / sizeof(vecs[0]);
    for (int i = 0; i < nvecs; i++) {
        uint64_t ref[4], nat[4], ucd[4];
        fe_mul_reference(vecs[i].a, vecs[i].b, ref);
        fe_mul_native(vecs[i].a, vecs[i].b, nat);
        fe_mul_ucode(vecs[i].a, vecs[i].b, ucd);
        int ok = !memcmp(ref, nat, 32) && !memcmp(ref, ucd, 32);
        if (!ok) {
            printf("  FAIL [%s]\n", vecs[i].name);
            for (int j = 0; j < 4; j++)
                printf("    [%d] ref=%016"PRIx64" nat=%016"PRIx64" ucd=%016"PRIx64"%s%s\n",
                    j, ref[j], nat[j], ucd[j],
                    ref[j] != nat[j] ? " nat***" : "",
                    ref[j] != ucd[j] ? " ucd***" : "");
        } else printf("  PASS [%s]\n", vecs[i].name);
        if (ok) pass++; else fail++;
    }

    printf("\n--- Random (10000) ---\n");
    uint64_t rng = 0xDEADBEEFCAFE1234ULL;
    int rp = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t a[4], b[4], ref[4], nat[4], ucd[4];
        rand_mod_p(a, &rng);
        rand_mod_p(b, &rng);
        fe_mul_reference(a, b, ref); fe_mul_native(a, b, nat); fe_mul_ucode(a, b, ucd);
        if (!memcmp(ref, nat, 32) && !memcmp(ref, ucd, 32)) rp++;
        else {
            printf("  FAIL #%d\n", i);
            printf("    a={%016"PRIx64",%016"PRIx64",%016"PRIx64",%016"PRIx64"}\n",
                   a[0], a[1], a[2], a[3]);
            printf("    b={%016"PRIx64",%016"PRIx64",%016"PRIx64",%016"PRIx64"}\n",
                   b[0], b[1], b[2], b[3]);
            for (int j = 0; j < 4; j++)
                printf("    [%d] ref=%016"PRIx64" nat=%016"PRIx64" ucd=%016"PRIx64"%s%s\n",
                    j, ref[j], nat[j], ucd[j],
                    ref[j] != nat[j] ? " nat***" : "",
                    ref[j] != ucd[j] ? " ucd***" : "");
            break;
        }
    }
    printf("  %d / 10000 PASS\n", rp);
    pass += rp; if (rp < 10000) fail += (10000 - rp);

    printf("\n--- Chain (1000 iterated mul-self = sq) ---\n");
    uint64_t ri[4] = {9, 0, 0, 0};
    uint64_t ni[4], ui[4];
    memcpy(ni, ri, 32); memcpy(ui, ri, 32);
    for (int i = 0; i < 1000; i++) {
        uint64_t tmp[4];
        memcpy(tmp, ri, 32); fe_mul_reference(tmp, tmp, ri);
        memcpy(tmp, ni, 32); fe_mul_native(tmp, tmp, ni);
        memcpy(tmp, ui, 32); fe_mul_ucode(tmp, tmp, ui);
    }
    int rok = !memcmp(ri, ni, 32) && !memcmp(ri, ui, 32);
    printf("  %s\n", rok ? "PASS" : "FAIL");
    if (!rok) {
        printf("  ref:"); for (int j=0;j<4;j++) printf(" %016"PRIx64, ri[j]); printf("\n");
        printf("  nat:"); for (int j=0;j<4;j++) printf(" %016"PRIx64, ni[j]); printf("\n");
        printf("  ucd:"); for (int j=0;j<4;j++) printf(" %016"PRIx64, ui[j]); printf("\n");
    }
    if (rok) pass++; else fail++;

    printf("\n--- Cross-check: mul(a,b) * commutativity ---\n");
    rng = 0x1234567890ABCDEFULL;
    int cp = 0;
    for (int i = 0; i < 1000; i++) {
        uint64_t a[4], b[4], ab[4], ba[4];
        rand_mod_p(a, &rng);
        rand_mod_p(b, &rng);
        fe_mul_ucode(a, b, ab);
        fe_mul_ucode(b, a, ba);
        if (!memcmp(ab, ba, 32)) cp++;
        else {
            printf("  FAIL commutativity #%d\n", i);
            break;
        }
    }
    printf("  %d / 1000 commutative PASS\n", cp);
    pass += cp; if (cp < 1000) fail += (1000 - cp);

    printf("\n=== %d passed, %d failed ===\n\n", pass, fail);
    return fail;
}

/* ── timing ───────────────────────────────────────────────────── */

static inline uint64_t rdtsc_start(void) {
    uint32_t lo, hi;
    asm volatile("cpuid\n\trdtsc" : "=a"(lo), "=d"(hi) :: "rbx", "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t rdtsc_end(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx", "memory");
    asm volatile("cpuid" ::: "rax", "rbx", "rcx", "rdx", "memory");
    return ((uint64_t)hi << 32) | lo;
}

#define BATCH 10000
#define REPS  200

int main(void) {
    printf("=== curve25519 Solinas multiplication: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_solinas_mul_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED, skipping benchmark.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    uint64_t state_a[4] = {9, 0, 0, 0};
    uint64_t state_b[4] = {7, 0, 0, 0};
    uint64_t tmp_a[4], tmp_b[4], t0, t1, min, sum;

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, 32);
        memcpy(tmp_b, state_b, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_native(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Naive -O3:   min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* fiat-crypto (the real GCC baseline CryptOpt compares against) */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, 32);
        memcpy(tmp_b, state_b, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_fiat(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Fiat-crypto: min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, 32);
        memcpy(tmp_b, state_b, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_ucode(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode:   min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    init_match_and_patch(); do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
