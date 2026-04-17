/*
 * asm_op_p256.c — Montgomery mul/sq for P-256 via microcode (two patches)
 *
 * Field: GF(p) where p = 2^256 - 2^224 + 2^192 + 2^96 - 1  (NIST P-256)
 * Representation: 4 saturated 64-bit limbs in Montgomery domain
 *   eval z = z[0] + z[1]*2^64 + z[2]*2^128 + z[3]*2^192
 *
 * Prime limbs:
 *   p[0] = 0xFFFFFFFFFFFFFFFF
 *   p[1] = 0x00000000FFFFFFFF
 *   p[2] = 0x0000000000000000
 *   p[3] = 0xFFFFFFFF00000001
 *
 * Montgomery constant: mu = -p^{-1} mod 2^64 = 1  (since p ≡ -1 mod 2^64)
 *
 * Algorithm: word-by-word Montgomery multiplication, 4 iterations.
 * Each iteration: multiply a[i]×b, add to accumulator, reduce mod p.
 * Split into two microcode patches (128-triad limit):
 *   Patch 1 (vmwrite, match 0): iterations 0-1
 *   Patch 2 (vmread,  match 1): iterations 2-3 + conditional subtract
 *
 * Build:  make PROG=asm_op_p256
 * Run:    sudo taskset -c 0 ./asm_op_p256_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* P-256 prime */
static const uint64_t P256_P[4] = {
    UINT64_C(0xFFFFFFFFFFFFFFFF),
    UINT64_C(0x00000000FFFFFFFF),
    UINT64_C(0x0000000000000000),
    UINT64_C(0xFFFFFFFF00000001)
};

/* ── fe_mul native C (word-by-word Montgomery) ──────────────── */

/*
 * Fully unrolled Montgomery multiplication exploiting P-256 structure:
 *   p[0] = 0xFFFFFFFFFFFFFFFF   p[1] = 0x00000000FFFFFFFF
 *   p[2] = 0x0000000000000000   p[3] = 0xFFFFFFFF00000001
 *   mu = 1  (so m = acc[0], no extra multiply)
 *   p[2] = 0  (skip one multiply per iteration)
 *
 * One iteration inlined:
 *   1. Schoolbook: t = a_i * b[0..3], chain partial products
 *   2. Add t to accumulator
 *   3. m = acc[0]  (mu=1)
 *   4. Reduce: acc += m * p (skip p[2]=0), shift right
 */
static inline void mont_iteration(uint64_t acc[5], uint64_t a_i,
                                   const uint64_t *b) {
    __uint128_t t;
    uint64_t c;

    /* ── schoolbook a_i × b ─────────────────────────────────── */
    t = (__uint128_t)a_i * b[0];
    uint64_t t0 = (uint64_t)t;
    uint64_t t0h = (uint64_t)(t >> 64);

    t = (__uint128_t)a_i * b[1] + t0h;
    uint64_t t1 = (uint64_t)t;
    uint64_t t1h = (uint64_t)(t >> 64);

    t = (__uint128_t)a_i * b[2] + t1h;
    uint64_t t2 = (uint64_t)t;
    uint64_t t2h = (uint64_t)(t >> 64);

    t = (__uint128_t)a_i * b[3] + t2h;
    uint64_t t3 = (uint64_t)t;
    uint64_t t4 = (uint64_t)(t >> 64);

    /* ── add to accumulator ──────────────────────────────────── */
    t = (__uint128_t)acc[0] + t0;
    acc[0] = (uint64_t)t; c = (uint64_t)(t >> 64);

    t = (__uint128_t)acc[1] + t1 + c;
    acc[1] = (uint64_t)t; c = (uint64_t)(t >> 64);

    t = (__uint128_t)acc[2] + t2 + c;
    acc[2] = (uint64_t)t; c = (uint64_t)(t >> 64);

    t = (__uint128_t)acc[3] + t3 + c;
    acc[3] = (uint64_t)t; c = (uint64_t)(t >> 64);

    uint64_t acc4 = acc[4] + t4 + c;

    /* ── Montgomery reduction: m = acc[0] (mu=1) ────────────── */
    uint64_t m = acc[0];

    /* m × p[0] = m × 0xFFFFFFFFFFFFFFFF = m×2^64 − m */
    t = (__uint128_t)m * UINT64_C(0xFFFFFFFFFFFFFFFF);
    uint64_t mp0_lo = (uint64_t)t;
    uint64_t mp0_hi = (uint64_t)(t >> 64);

    /* m × p[1] = m × 0xFFFFFFFF */
    t = (__uint128_t)m * UINT32_C(0xFFFFFFFF);
    uint64_t mp1_lo = (uint64_t)t;
    uint64_t mp1_hi = (uint64_t)(t >> 64);

    /* m × p[2] = 0 (skip!) */

    /* m × p[3] = m × 0xFFFFFFFF00000001 */
    t = (__uint128_t)m * UINT64_C(0xFFFFFFFF00000001);
    uint64_t mp3_lo = (uint64_t)t;
    uint64_t mp3_hi = (uint64_t)(t >> 64);

    /* Chain m*p products:
     * red[0] = mp0_lo
     * red[1] = mp0_hi + mp1_lo
     * red[2] = carry  + mp1_hi   (since p[2]=0)
     * red[3] = mp3_lo
     * red[4] = mp3_hi
     */
    t = (__uint128_t)mp0_hi + mp1_lo;
    uint64_t red1 = (uint64_t)t;
    uint64_t red2 = mp1_hi + (uint64_t)(t >> 64);

    /* ── Add m*p to accumulator + shift ──────────────────────── */
    /* word 0 cancels (acc[0] + mp0_lo = 0 mod 2^64, carry = 1 when m>0) */
    t = (__uint128_t)acc[0] + mp0_lo;
    c = (uint64_t)(t >> 64);

    t = (__uint128_t)acc[1] + red1 + c;
    acc[0] = (uint64_t)t; c = (uint64_t)(t >> 64);  /* shift: [1]→[0] */

    t = (__uint128_t)acc[2] + red2 + c;
    acc[1] = (uint64_t)t; c = (uint64_t)(t >> 64);

    t = (__uint128_t)acc[3] + mp3_lo + c;
    acc[2] = (uint64_t)t; c = (uint64_t)(t >> 64);

    t = (__uint128_t)acc4 + mp3_hi + c;
    acc[3] = (uint64_t)t;
    acc[4] = (uint64_t)(t >> 64);
}

static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t acc[5] = {0};

    mont_iteration(acc, a[0], b);
    mont_iteration(acc, a[1], b);
    mont_iteration(acc, a[2], b);
    mont_iteration(acc, a[3], b);

    /* Conditional subtract: if acc >= p, output acc - p; else output acc.
     * Borrow-chain using __uint128_t: upper 64 bits = 0xFFFF... when underflow. */
    uint64_t diff[4];
    __uint128_t b128;

    b128 = (__uint128_t)acc[0] - UINT64_C(0xFFFFFFFFFFFFFFFF);
    diff[0] = (uint64_t)b128;
    b128 = (__uint128_t)acc[1] - UINT32_C(0xFFFFFFFF) - ((uint64_t)(b128 >> 64) & 1);
    diff[1] = (uint64_t)b128;
    b128 = (__uint128_t)acc[2] - 0 - ((uint64_t)(b128 >> 64) & 1);
    diff[2] = (uint64_t)b128;
    b128 = (__uint128_t)acc[3] - UINT64_C(0xFFFFFFFF00000001) - ((uint64_t)(b128 >> 64) & 1);
    diff[3] = (uint64_t)b128;
    b128 = (__uint128_t)acc[4] - 0 - ((uint64_t)(b128 >> 64) & 1);
    uint64_t underflow = (uint64_t)(b128 >> 64) & 1; /* 1 if acc < p */

    uint64_t mask = (uint64_t)0 - underflow; /* all-ones if acc < p (use original) */
    out[0] = (acc[0] & mask) | (diff[0] & ~mask);
    out[1] = (acc[1] & mask) | (diff[1] & ~mask);
    out[2] = (acc[2] & mask) | (diff[2] & ~mask);
    out[3] = (acc[3] & mask) | (diff[3] & ~mask);
}

/* ── fe_sq native C ─────────────────────────────────────────── */

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    fe_mul_native(a, a, out);
}

/* ── independent reference (big-integer multiply mod p) ──────── */

static void fe_mul_reference(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    /*
     * Compute (a * b * R^{-1}) mod p in Montgomery domain.
     * Same algorithm as native but with explicit __uint128_t to avoid
     * any optimization that could hide bugs.
     */
    __uint128_t acc[5] = {0};

    for (int i = 0; i < 4; i++) {
        __uint128_t c = 0;
        for (int j = 0; j < 4; j++) {
            c += (__uint128_t)a[i] * b[j] + (uint64_t)acc[j];
            acc[j] = (uint64_t)c;
            c >>= 64;
        }
        acc[4] = (uint64_t)((uint64_t)acc[4] + (uint64_t)c);

        uint64_t m = (uint64_t)acc[0]; /* mu = 1 */
        c = 0;
        for (int j = 0; j < 4; j++) {
            c += (__uint128_t)m * P256_P[j] + (uint64_t)acc[j];
            acc[j] = (uint64_t)c;
            c >>= 64;
        }
        acc[4] = (uint64_t)((uint64_t)acc[4] + (uint64_t)c);

        acc[0] = acc[1]; acc[1] = acc[2]; acc[2] = acc[3]; acc[3] = acc[4]; acc[4] = 0;
    }

    __uint128_t borrow = 0;
    uint64_t diff[4];
    for (int j = 0; j < 4; j++) {
        borrow = (__uint128_t)(uint64_t)acc[j] - P256_P[j] - (uint64_t)borrow;
        diff[j] = (uint64_t)borrow;
        borrow = (borrow >> 64) & 1;
    }
    uint64_t mask = (uint64_t)0 - (uint64_t)borrow;
    for (int j = 0; j < 4; j++)
        out[j] = ((uint64_t)acc[j] & mask) | (diff[j] & ~mask);
}

static void fe_sq_reference(const uint64_t *a, uint64_t *out) {
    fe_mul_reference(a, a, out);
}

/* ── microcode patches ───────────────────────────────────────── */

/*
 * Two patches for P-256 Montgomery multiplication:
 *
 * Register convention at vmwrite entry:
 *   RDI=a0  RSI=a1  R12=a2  R11=a3
 *   R15=b0  R13=b1  R9=b2   R10=b3
 *   R8=p0(all 1s)  R14=p1(0xFFFFFFFF)  RBX=p3(0xFFFFFFFF00000001)
 *   RAX=0  RCX=free  RDX=free
 *
 * Accumulator between iterations: R15=acc[0] R9=acc[1] R10=acc[2] R13=acc[3] TMP15=acc[4]
 * b values saved in TMP10-TMP13 (persist across both patches)
 *
 * Each Montgomery iteration:
 *   Phase A:  schoolbook a_i × b[0..3] → 5-word product (in TMP scratch)
 *   Phase A': (iter 1+) add product to accumulator (5-word carry chain)
 *   Phase B:  m = acc[0], compute m×p[0,1,3] (skip p[2]=0), chain
 *   Phase C:  add m×p to accumulator, shift right (discard word 0)
 *
 * MUL convention: MUL(hi_dest, srcA, srcB) → srcA preserved, srcB = lo product
 * addcarryx with cin: 2 triads (ADD+SETCC, ADD+SETCC) + combine in next triad
 */

/*
 * ── PHASE A MACRO: schoolbook a_i × b[0..3] → 5-word product ──
 * Input:  a_i in arch reg, b[0..3] in TMP10-13
 * Output: TMP0=w0, TMP2=w1, TMP4=w2, TMP5=w3, TMP6=w4
 * Clobbers: TMP0-TMP7, RCX, RDX
 * 15 triads.
 */
#define PHASE_A(a_reg) \
    { ZEROEXT_DSZ64_DR(RDX, a_reg), NOP, NOP, NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP10, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP0, RDX), ZEROEXT_DSZ64_DR(TMP1, RCX), \
      ZEROEXT_DSZ64_DR(RDX, a_reg), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP11, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP2, TMP1, RDX), SETCC_CONDB_DR(TMP3, TMP2), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(RDX, a_reg), NOP, NOP, NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP12, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP4, TMP1, RDX), SETCC_CONDB_DR(TMP5, TMP4), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP3), SETCC_CONDB_DR(TMP6, TMP4), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP5, TMP6), ZEROEXT_DSZ64_DR(RDX, a_reg), \
      NOP, NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP13, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP5, TMP1, RDX), SETCC_CONDB_DR(TMP6, TMP5), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP5, TMP5, TMP3), SETCC_CONDB_DR(TMP7, TMP5), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP6, TMP7), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP6, RCX, TMP3), NOP, NOP, NOP_SEQWORD }

/*
 * ── PHASE A': add 5-word product to accumulator ──
 * Accumulator: R15=acc[0] R9=acc[1] R10=acc[2] R13=acc[3] TMP15=acc[4]
 * Product: TMP0=w0 TMP2=w1 TMP4=w2 TMP5=w3 TMP6=w4
 * Output: updated accumulator, TMP14=extra_carry
 * 10 triads.
 */
#define PHASE_A_PRIME \
    { ADD_DSZ64_DRR(R15, R15, TMP0), SETCC_CONDB_DR(TMP3, R15), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, R9, TMP2), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R9, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R9), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), \
      ADD_DSZ64_DRR(TMP0, R10, TMP4), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R10, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R10), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), \
      ADD_DSZ64_DRR(TMP0, R13, TMP5), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R13, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R13), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), \
      ADD_DSZ64_DRR(TMP0, TMP15, TMP6), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP15, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP15), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP14, TMP1, TMP8), NOP, NOP, NOP_SEQWORD }

/*
 * ── PHASE B: Montgomery reduction m×p ──
 * m = R15 (= acc[0], since mu=1).  Computes m×p[0], m×p[1], m×p[3], chains.
 * Output: TMP8=red0, TMP7=red1, TMP9=red2, RDX=red3, RCX=red4
 * 8 triads.
 */
#define PHASE_B \
    { ZEROEXT_DSZ64_DR(RDX, R15), NOP, NOP, NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, R8, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP7, RCX), ZEROEXT_DSZ64_DR(TMP8, RDX), \
      ZEROEXT_DSZ64_DR(RDX, R15), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, R14, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP9, RCX), ZEROEXT_DSZ64_DR(TMP3, RDX), \
      ZEROEXT_DSZ64_DR(RDX, R15), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, RBX, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP7, TMP7, TMP3), SETCC_CONDB_DR(TMP3, TMP7), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP9, TMP9, TMP3), NOP, NOP, NOP_SEQWORD }

/*
 * ── PHASE C: add m×p to accumulator + shift (iteration 0) ──
 * Accumulator words: TMP0=w0 TMP2=w1 TMP4=w2 TMP5=w3 TMP6=w4
 * Reduction: TMP8=red0 TMP7=red1 TMP9=red2 RDX=red3 RCX=red4
 * Output: R15=acc[0] R9=acc[1] R10=acc[2] R13=acc[3] TMP15=acc[4]
 * 10 triads.
 */
#define PHASE_C_ITER0 \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP8), SETCC_CONDB_DR(TMP3, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP2, TMP7), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R15, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R15), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), \
      ADD_DSZ64_DRR(TMP0, TMP4, TMP9), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R9, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R9), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), \
      ADD_DSZ64_DRR(TMP0, TMP5, RDX), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R10, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R10), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), \
      ADD_DSZ64_DRR(TMP0, TMP6, RCX), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R13, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R13), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP15, TMP1, TMP8), NOP, NOP, NOP_SEQWORD }

/*
 * ── PHASE C: add m×p to accumulator + shift (iterations 1+) ──
 * Same as ITER0 but reads accumulator from R15/R9/R10/R13/TMP15
 * and adds TMP14 (extra carry from Phase A') at the end.
 * 11 triads.
 */
#define PHASE_C_CHAIN \
    { ADD_DSZ64_DRR(TMP0, R15, TMP8), SETCC_CONDB_DR(TMP3, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, R9, TMP7), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R15, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R15), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), \
      ADD_DSZ64_DRR(TMP0, R10, TMP9), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R9, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R9), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), \
      ADD_DSZ64_DRR(TMP0, R13, RDX), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R10, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R10), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), \
      ADD_DSZ64_DRR(TMP0, TMP15, RCX), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(R13, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R13), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP15, TMP1, TMP8), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP15, TMP15, TMP14), NOP, NOP, NOP_SEQWORD }

static void install_p256_patches(void) {

    /* ════════ PATCH 1: iterations 0-1  (79 triads) ════════ */
    ucode_t patch1[] = {

    /* PREP: save b values, zero acc[4] */
    { ZEROEXT_DSZ64_DR(TMP10, R15), ZEROEXT_DSZ64_DR(TMP11, R13),
      ZEROEXT_DSZ64_DR(TMP12, R9), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R10), NOTAND_DSZ64_DRR(TMP15, RAX, RAX),
      NOP, NOP_SEQWORD },

    /* ── Iteration 0: a0(RDI) × b ── */
    PHASE_A(RDI),      /* 15 triads → product in TMP0,TMP2,TMP4,TMP5,TMP6 */
    /* word0(TMP0) → R15 so PHASE_B reads correct m */
    { ZEROEXT_DSZ64_DR(R15, TMP0), NOP, NOP, NOP_SEQWORD },
    PHASE_B,            /* 8 triads → reduction in TMP8,TMP7,TMP9,RDX,RCX */
    PHASE_C_ITER0,      /* 10 triads → acc in R15,R9,R10,R13,TMP15 */

    /* ── Iteration 1: a1(RSI) × b ── */
    PHASE_A(RSI),       /* 15 triads */
    PHASE_A_PRIME,      /* 10 triads → acc updated, TMP14=extra carry */
    PHASE_B,            /* 8 triads */

    /* Phase C chain (last triad gets END_SEQWORD) — inline instead of macro */
    { ADD_DSZ64_DRR(TMP0, R15, TMP8), SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, R9, TMP7), SETCC_CONDB_DR(TMP1, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(R15, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R15),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8),
      ADD_DSZ64_DRR(TMP0, R10, TMP9), SETCC_CONDB_DR(TMP1, TMP0),
      NOP_SEQWORD },
    { ADD_DSZ64_DRR(R9, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R9),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8),
      ADD_DSZ64_DRR(TMP0, R13, RDX), SETCC_CONDB_DR(TMP1, TMP0),
      NOP_SEQWORD },
    { ADD_DSZ64_DRR(R10, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R10),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8),
      ADD_DSZ64_DRR(TMP0, TMP15, RCX), SETCC_CONDB_DR(TMP1, TMP0),
      NOP_SEQWORD },
    { ADD_DSZ64_DRR(R13, TMP0, TMP3), SETCC_CONDB_DR(TMP8, R13),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP15, TMP1, TMP8), NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP15, TMP15, TMP14), NOP, NOP, END_SEQWORD }

    };

    /* ════════ PATCH 2: iterations 2-3 + conditional subtract  (105 triads) ════════ */
    ucode_t patch2[] = {

    /* ── Iteration 2: a2(R12) × b ── */
    PHASE_A(R12),
    PHASE_A_PRIME,
    PHASE_B,
    PHASE_C_CHAIN,

    /* ── Iteration 3: a3(R11) × b ── */
    PHASE_A(R11),
    PHASE_A_PRIME,
    PHASE_B,
    PHASE_C_CHAIN,

    /* ── Conditional subtract: if acc >= p, output acc-p ── */
    /* Compute ~p constants and "1" */
    { SHR_DSZ64_DRI(TMP0, R8, 63),         /* TMP0 = 1 */
      NOTAND_DSZ64_DRR(TMP1, R8, R14),     /* TMP1 = ~p[1] = 0xFFFFFFFF00000000 */
      NOTAND_DSZ64_DRR(TMP2, R8, RBX),     /* TMP2 = ~p[3] = 0x00000000FFFFFFFE */
      NOP_SEQWORD },

    /* diff[0] = acc[0] + 0 + 1  (~p[0]=0) */
    { ADD_DSZ64_DRR(RDI, R15, TMP0), SETCC_CONDB_DR(TMP3, RDI),
      NOP, NOP_SEQWORD },

    /* diff[1] = acc[1] + ~p[1] + carry */
    { ADD_DSZ64_DRR(TMP0, R9, TMP1), SETCC_CONDB_DR(TMP4, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(RSI, TMP0, TMP3), SETCC_CONDB_DR(TMP5, RSI),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP4, TMP5),
      ADD_DSZ64_DRR(TMP0, R10, R8), SETCC_CONDB_DR(TMP4, TMP0),
      NOP_SEQWORD },

    /* diff[2] = acc[2] + ~p[2] + carry  (~p[2]=all_ones=R8) */
    { ADD_DSZ64_DRR(R12, TMP0, TMP3), SETCC_CONDB_DR(TMP5, R12),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP4, TMP5),
      ADD_DSZ64_DRR(TMP0, R13, TMP2), SETCC_CONDB_DR(TMP4, TMP0),
      NOP_SEQWORD },

    /* diff[3] = acc[3] + ~p[3] + carry */
    { ADD_DSZ64_DRR(R11, TMP0, TMP3), SETCC_CONDB_DR(TMP5, R11),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP4, TMP5), NOP, NOP, NOP_SEQWORD },

    /* carry_out = carry + acc[4]: nonzero means acc >= p → select diff */
    { OR_DSZ64_DRR(TMP3, TMP3, TMP15), NOP, NOP, NOP_SEQWORD },

    /* mask = TMP3 * all_ones: 0 or 0xFFFF..F */
    { MUL_DSZ64_DRR(RCX, R8, TMP3), NOP, NOP, NOP_SEQWORD },
    /* TMP3 = mask (lo product). Compute ~mask. */
    { NOTAND_DSZ64_DRR(TMP0, R8, TMP3), NOP, NOP, NOP_SEQWORD },

    /* cmov word 0: out = (diff & mask) | (acc & ~mask) */
    { NOTAND_DSZ64_DRR(TMP1, R15, TMP3),    /* acc[0] & ~mask */
      NOTAND_DSZ64_DRR(TMP2, RDI, TMP0),    /* diff[0] & mask */
      NOP, NOP_SEQWORD },
    /* cmov word 1 (pack OR with next NOTANDs) */
    { OR_DSZ64_DRR(R15, TMP1, TMP2),
      NOTAND_DSZ64_DRR(TMP1, R9, TMP3),
      NOTAND_DSZ64_DRR(TMP2, RSI, TMP0),
      NOP_SEQWORD },
    /* cmov word 2 */
    { OR_DSZ64_DRR(R9, TMP1, TMP2),
      NOTAND_DSZ64_DRR(TMP1, R10, TMP3),
      NOTAND_DSZ64_DRR(TMP2, R12, TMP0),
      NOP_SEQWORD },
    /* cmov word 3 */
    { OR_DSZ64_DRR(R10, TMP1, TMP2),
      NOTAND_DSZ64_DRR(TMP1, R13, TMP3),
      NOTAND_DSZ64_DRR(TMP2, R11, TMP0),
      NOP_SEQWORD },
    { OR_DSZ64_DRR(R13, TMP1, TMP2), NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch1, ARRAY_SZ(patch1));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);

    /* Patch2 must go AFTER 0x7df0 (hook_match_and_patch staging area at 0x7de0-0x7df0) */
    patch_ucode(0x7e00, patch2, ARRAY_SZ(patch2));
    hook_match_and_patch(1, 0x0618, 0x7e00);

    printf("p256 patches installed: patch1=%d triads, patch2=%d triads\n",
           (int)ARRAY_SZ(patch1), (int)ARRAY_SZ(patch2));
}

/* ── fe_mul via microcode ─────────────────────────────────────── */

static void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_b   asm("rbx") = (uint64_t *)b;
    register uint64_t *_out asm("r15") = out;

    asm volatile(
        "push r15\n\t"

        /* load a[0..3] from rcx */
        "mov rdi, [rcx]\n\t"
        "mov rsi, [rcx + 8]\n\t"
        "mov r12, [rcx + 16]\n\t"
        "mov r11, [rcx + 24]\n\t"

        /* load b[0..3] from rbx */
        "mov r15, [rbx]\n\t"
        "mov r13, [rbx + 8]\n\t"
        "mov r9,  [rbx + 16]\n\t"
        "mov r10, [rbx + 24]\n\t"

        /* load p constants */
        "mov r8, -1\n\t"                        /* p0 = 0xFFFFFFFFFFFFFFFF */
        "mov r14, 0xffffffff\n\t"               /* p1 */
        "mov rbx, 0xffffffff00000001\n\t"       /* p3 */

        "xor eax, eax\n\t"

        /* Patch 1: iterations 0-1 (vmwrite triggers match reg 0) */
        "vmwrite rcx, rdx\n\t"

        /* Patch 2: iterations 2-3 + subtract (vmread triggers match reg 1) */
        ".byte 0x0f, 0x78, 0xca\n\t"

        /* Results: R15=out[0] R9=out[1] R10=out[2] R13=out[3] */
        "pop rcx\n\t"
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
}

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    fe_mul_ucode(a, a, out);
}

/* ── verification ────────────────────────────────────────────── */

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* Montgomery form of 1: R mod p = 2^256 mod p */
static const uint64_t MONT_ONE[4] = {
    UINT64_C(0x0000000000000001),
    UINT64_C(0xFFFFFFFF00000000),
    UINT64_C(0xFFFFFFFFFFFFFFFF),
    UINT64_C(0x00000000FFFFFFFE)
};

typedef struct {
    const char *label;
    uint64_t    a[4];
    uint64_t    b[4];
    uint64_t    expected[4];
    int         has_expected;
} test_vec_t;

static const test_vec_t test_vectors[] = {
    /* 0 * 0 = 0 in Montgomery domain */
    { "0*0", {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, 1 },

    /* 0 * R = 0 */
    { "0*R",
      {0,0,0,0},
      {0x0000000000000001, 0xFFFFFFFF00000000,
       0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFE},
      {0,0,0,0}, 1 },

    /* R * R = R^2 mod p (= toMontgomery(1) * R, but let reference compute) */
    { "R*R",
      {0x0000000000000001, 0xFFFFFFFF00000000,
       0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFE},
      {0x0000000000000001, 0xFFFFFFFF00000000,
       0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFE},
      {0}, 0 },

    /* Squaring test: a*a where a is a small Montgomery value */
    { "small_sq",
      {0x0000000000000002, 0xFFFFFFFE00000000,
       0xFFFFFFFFFFFFFFFF, 0x00000001FFFFFFFD},
      {0x0000000000000002, 0xFFFFFFFE00000000,
       0xFFFFFFFFFFFFFFFF, 0x00000001FFFFFFFD},
      {0}, 0 },
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[4], nat[4], ucd[4];
    fe_mul_reference(t->a, t->b, ref);
    fe_mul_native(t->a, t->b, nat);
    fe_mul_ucode(t->a, t->b, ucd);

    int ok = 1;
    if (memcmp(ref, nat, 32) != 0) {
        printf("  FAIL [%s] native != reference\n", t->label);
        for (int i = 0; i < 4; i++)
            printf("    [%d] native=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, nat[i], ref[i], nat[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (memcmp(ref, ucd, 32) != 0) {
        printf("  FAIL [%s] ucode != reference\n", t->label);
        for (int i = 0; i < 4; i++)
            printf("    [%d] ucode=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, ucd[i], ref[i], ucd[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (t->has_expected && memcmp(ref, t->expected, 32) != 0) {
        printf("  FAIL [%s] result != expected\n", t->label);
        for (int i = 0; i < 4; i++)
            printf("    [%d] got=%016" PRIx64 " exp=%016" PRIx64 " %s\n",
                   i, ref[i], t->expected[i],
                   ref[i] == t->expected[i] ? "" : "***");
        ok = 0;
    }
    if (ok) printf("  PASS [%s]\n", t->label);
    return ok;
}

static int verify_random_quiet(const uint64_t a[4], const uint64_t b[4]) {
    uint64_t ref[4], nat[4], ucd[4];
    fe_mul_reference(a, b, ref);
    fe_mul_native(a, b, nat);
    fe_mul_ucode(a, b, ucd);
    if (memcmp(ref, nat, 32) != 0 || memcmp(ref, ucd, 32) != 0) {
        printf("  FAIL random\n");
        return 0;
    }
    return 1;
}

/* Generate a random value < p for Montgomery domain */
static void rand_mod_p(uint64_t out[4], uint64_t *rng) {
    for (;;) {
        for (int j = 0; j < 4; j++)
            out[j] = splitmix64(rng);
        /* Reduce to < p: simple rejection */
        int lt = 0, gt = 0;
        for (int j = 3; j >= 0; j--) {
            if (out[j] < P256_P[j]) { lt = 1; break; }
            if (out[j] > P256_P[j]) { gt = 1; break; }
        }
        if (lt || (!gt)) { /* out <= p, but we want < p; for random this is fine */
            if (lt) break;
            /* out == p, extremely unlikely, retry */
        }
    }
}

#define RANDOM_TESTS 10000
#define CHAIN_ITERS  1000

static int verify_all(void) {
    int pass = 0, fail = 0;

    printf("--- Known test vectors ---\n");
    for (int i = 0; i < (int)N_VECS; i++) {
        if (verify_one(&test_vectors[i])) pass++; else fail++;
    }

    printf("\n--- Random mul test (%d vectors) ---\n", RANDOM_TESTS);
    uint64_t rng = 0xA256CAFE12345678ULL;
    int rpass = 0;
    for (int i = 0; i < RANDOM_TESTS; i++) {
        uint64_t a[4], b[4];
        rand_mod_p(a, &rng);
        rand_mod_p(b, &rng);
        if (verify_random_quiet(a, b)) rpass++;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    printf("\n--- Random sq test (%d vectors) ---\n", RANDOM_TESTS);
    rpass = 0;
    for (int i = 0; i < RANDOM_TESTS; i++) {
        uint64_t a[4], ref[4], nat[4], ucd[4];
        rand_mod_p(a, &rng);
        fe_sq_reference(a, ref);
        fe_sq_native(a, nat);
        fe_sq_ucode(a, ucd);
        if (memcmp(ref, nat, 32) == 0 && memcmp(ref, ucd, 32) == 0) rpass++;
        else printf("  FAIL random sq\n");
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    printf("\n--- Iterated chain (%d mul-self) ---\n", CHAIN_ITERS);
    uint64_t ri[4], ni[4], ui[4];
    memcpy(ri, MONT_ONE, 32); memcpy(ni, MONT_ONE, 32); memcpy(ui, MONT_ONE, 32);
    /* Multiply by a fixed value repeatedly */
    uint64_t mult[4] = {0x3, 0x0, 0x0, 0x0};
    for (int i = 0; i < CHAIN_ITERS; i++) {
        uint64_t tmp[4];
        memcpy(tmp, ri, 32); fe_mul_reference(tmp, mult, ri);
        memcpy(tmp, ni, 32); fe_mul_native(tmp, mult, ni);
        memcpy(tmp, ui, 32); fe_mul_ucode(tmp, mult, ui);
    }
    int ref_nat = memcmp(ri, ni, 32) == 0;
    int ref_ucd = memcmp(ri, ui, 32) == 0;
    printf("  ref==native: %s   ref==ucode: %s   -> %s\n",
           ref_nat ? "yes" : "NO", ref_ucd ? "yes" : "NO",
           (ref_nat && ref_ucd) ? "PASS" : "FAIL");
    if (ref_nat && ref_ucd) pass++; else fail++;

    printf("\n=== Verification: %d passed, %d failed ===\n\n", pass, fail);
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

#define BATCH 1000
#define REPS  100

int main(void) {
    uint64_t t0, t1, min, sum;

    printf("=== P-256 Montgomery: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_p256_patches();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    uint64_t sa[4], sb[4];
    memcpy(sa, MONT_ONE, 32);
    sb[0] = 0x6B17D1F2E12C4247ULL; sb[1] = 0xF8BCE6E563A440F2ULL;
    sb[2] = 0x7037D812DEB33A0FULL; sb[3] = 0x4FE342E2FE1A7F9BULL;

    printf("--- mul: %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t ta[4], tb[4];

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, 32); memcpy(tb, sb, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_native(ta, tb, ta);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Native mul:  min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, 32); memcpy(tb, sb, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_ucode(ta, tb, ta);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Ucode mul:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    printf("\n--- sq: %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_native(ta, ta);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Native sq:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_ucode(ta, ta);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Ucode sq:    min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
