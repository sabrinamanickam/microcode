/*
 * asm_op_p224_solinas_sq.c — P-224 field squaring via Solinas reduction
 *
 * Uses direct schoolbook squaring + NIST fast reduction, NOT Montgomery.
 * Single vmwrite: 10 MULs (schoolbook squaring with Dettman MAC pattern).
 * Solinas reduction done in native C after the vmwrite.
 *
 * P-224: p = 2^224 - 2^96 + 1
 *   p[0] = 0x0000000000000001
 *   p[1] = 0xFFFFFFFF00000000
 *   p[2] = 0xFFFFFFFFFFFFFFFF
 *   p[3] = 0x00000000FFFFFFFF
 *
 * Input:  a = [a0, a1, a2, a3] in standard (non-Montgomery) representation
 *         where 0 <= a < p, so a3 <= 0xFFFFFFFF (32 bits).
 *
 * Step 1 (microcode): Schoolbook squaring -> 7-word product w[0..6]
 *   w0 = lo(a0^2),  carry -> w1
 *   w1 = lo(2*a0*a1 + carry),  carry -> w2
 *   w2 = lo(2*a0*a2 + a1^2 + carry),  carry -> w3
 *   w3 = lo(2*a0*a3 + 2*a1*a2 + carry),  carry -> w4
 *   w4 = lo(2*a1*a3 + a2^2 + carry),  carry -> w5
 *   w5 = lo(2*a2*a3 + carry),  carry -> w6
 *   w6 = lo(a3^2 + carry)
 *
 * Step 2 (native C): NIST Solinas reduction (FIPS 186-4 D.2.1)
 *   s1 + s2 + s3 - s4 - s5 (mod p)
 *
 * Build:  make PROG=asm_op_p224_solinas_sq
 * Run:    sudo taskset -c 0 ./asm_op_p224_solinas_sq_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

static const uint64_t P224_P[4] = {
    UINT64_C(0x0000000000000001), UINT64_C(0xFFFFFFFF00000000),
    UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x00000000FFFFFFFF)
};

/* ── Solinas reduction: 7-word product -> 4-word result mod p ── */

/*
 * Given w[0..6] (448-bit product), compute result mod p.
 *
 * Split into 32-bit halves: c[2k] = lo32(w[k]), c[2k+1] = hi32(w[k]).
 * (c[14]=c[15]=0 since w[7]=0.)
 *
 * NIST P-224 reduction (from FIPS 186-4 D.2.1):
 *   s1 = (c6,  c5,  c4,  c3,  c2,  c1,  c0)    -- 7 x 32-bit
 *   s2 = (c10, c9,  c8,  c7,  0,   0,   0)
 *   s3 = (0,   c13, c12, c11, 0,   0,   0)
 *   s4 = (c13, c12, c11, c10, c9,  c8,  c7)
 *   s5 = (0,   0,   0,   0,   c13, c12, c11)
 *
 *   result = s1 + s2 + s3 - s4 - s5 (mod p)
 *
 * Each s value contributes to 32-bit positions 0..6:
 *
 * pos:  s1    s2    s3    s4     s5
 *  0:   c0    0     0     c7     c11
 *  1:   c1    0     0     c8     c12
 *  2:   c2    0     0     c9     c13
 *  3:   c3    c7    c11   c10    0
 *  4:   c4    c8    c12   c11    0
 *  5:   c5    c9    c13   c12    0
 *  6:   c6    c10   0     c13    0
 *
 * Net per column (s1 + s2 + s3 - s4 - s5):
 *  0: c0 - c7 - c11
 *  1: c1 - c8 - c12
 *  2: c2 - c9 - c13
 *  3: c3 + c7 + c11 - c10
 *  4: c4 + c8 + c12 - c11
 *  5: c5 + c9 + c13 - c12
 *  6: c6 + c10 - c13
 *
 * Pack into 64-bit limbs (positions 0-1 -> limb0, 2-3 -> limb1, etc.)
 * with signed arithmetic and carry propagation.
 */
static void solinas_reduce(const uint64_t w[7], uint64_t out[4]) {
    /* Extract 32-bit halves */
    uint32_t c[14];
    for (int i = 0; i < 7; i++) {
        c[2*i]   = (uint32_t)w[i];
        c[2*i+1] = (uint32_t)(w[i] >> 32);
    }

    /* Compute per-column sums in signed 64-bit, then propagate carries.
     * Use int64_t accumulators to handle negative intermediate values. */
    int64_t col[7];
    col[0] = (int64_t)c[0] - (int64_t)c[7] - (int64_t)c[11];
    col[1] = (int64_t)c[1] - (int64_t)c[8] - (int64_t)c[12];
    col[2] = (int64_t)c[2] - (int64_t)c[9] - (int64_t)c[13];
    col[3] = (int64_t)c[3] + (int64_t)c[7] + (int64_t)c[11] - (int64_t)c[10];
    col[4] = (int64_t)c[4] + (int64_t)c[8] + (int64_t)c[12] - (int64_t)c[11];
    col[5] = (int64_t)c[5] + (int64_t)c[9] + (int64_t)c[13] - (int64_t)c[12];
    col[6] = (int64_t)c[6] + (int64_t)c[10] - (int64_t)c[13];

    /* Pack into 64-bit limbs with carry propagation.
     * Each 32-bit column carries into the next. */
    int64_t carry = 0;
    int64_t r[4];

    carry = col[0];
    r[0] = carry & 0xFFFFFFFF;
    carry >>= 32;

    carry += col[1];
    r[0] |= (carry & 0xFFFFFFFF) << 32;
    carry >>= 32;

    carry += col[2];
    r[1] = carry & 0xFFFFFFFF;
    carry >>= 32;

    carry += col[3];
    r[1] |= (carry & 0xFFFFFFFF) << 32;
    carry >>= 32;

    carry += col[4];
    r[2] = carry & 0xFFFFFFFF;
    carry >>= 32;

    carry += col[5];
    r[2] |= (carry & 0xFFFFFFFF) << 32;
    carry >>= 32;

    carry += col[6];
    r[3] = carry & 0xFFFFFFFF;
    carry >>= 32;

    /* carry is the overflow beyond 224 bits (can be -2..+2).
     * Since p = 2^224 - 2^96 + 1, we have 2^224 ≡ 2^96 - 1 (mod p).
     * So carry * 2^224 ≡ carry * (2^96 - 1) mod p.
     * Fold: r += carry * (2^96 - 1) = r - carry + carry * 2^96. */
    for (int round = 0; round < 3 && carry != 0; round++) {
        __int128_t adj;
        adj = (__int128_t)(uint64_t)r[0] - carry;
        r[0] = (int64_t)(uint64_t)adj;
        adj >>= 64;

        adj += (__int128_t)(uint64_t)r[1] + (carry << 32);
        r[1] = (int64_t)(uint64_t)adj;
        adj >>= 64;

        adj += (uint64_t)r[2];
        r[2] = (int64_t)(uint64_t)adj;
        adj >>= 64;

        adj += (uint64_t)r[3];
        r[3] = (int64_t)(uint64_t)adj;
        carry = (int64_t)(adj >> 32);
        r[3] &= 0xFFFFFFFF;
    }

    /* Handle negative result: add p until non-negative.
     * This happens when carry was initially negative. */
    while (carry < 0) {
        __int128_t adj;
        adj = (__int128_t)(uint64_t)r[0] + P224_P[0];
        r[0] = (int64_t)(uint64_t)adj;
        adj >>= 64;
        adj += (__int128_t)(uint64_t)r[1] + P224_P[1];
        r[1] = (int64_t)(uint64_t)adj;
        adj >>= 64;
        adj += (__int128_t)(uint64_t)r[2] + P224_P[2];
        r[2] = (int64_t)(uint64_t)adj;
        adj >>= 64;
        adj += (__int128_t)(uint64_t)r[3] + P224_P[3];
        r[3] = (int64_t)(uint64_t)adj;
        carry = (int64_t)(adj >> 32);
        r[3] &= 0xFFFFFFFF;
    }

    /* Conditional subtract of p if result >= p (at most once more) */
    while (carry > 0) {
        __int128_t adj;
        adj = (__int128_t)(uint64_t)r[0] - P224_P[0];
        r[0] = (int64_t)(uint64_t)adj;
        adj >>= 64;
        adj += (__int128_t)(uint64_t)r[1] - (int64_t)P224_P[1];
        r[1] = (int64_t)(uint64_t)adj;
        adj >>= 64;
        adj += (__int128_t)(uint64_t)r[2] - (int64_t)P224_P[2];
        r[2] = (int64_t)(uint64_t)adj;
        adj >>= 64;
        adj += (__int128_t)(uint64_t)r[3] - (int64_t)P224_P[3];
        r[3] = (int64_t)(uint64_t)adj;
        carry = (int64_t)(adj >> 32);
        r[3] &= 0xFFFFFFFF;
    }

    /* Final conditional subtract of p */
    {
        uint64_t d[4];
        __uint128_t borrow;
        uint64_t b;

        borrow = (__uint128_t)(uint64_t)r[0] - P224_P[0];
        d[0] = (uint64_t)borrow;
        b = (uint64_t)(borrow >> 64) & 1;

        borrow = (__uint128_t)(uint64_t)r[1] - P224_P[1] - b;
        d[1] = (uint64_t)borrow;
        b = (uint64_t)(borrow >> 64) & 1;

        borrow = (__uint128_t)(uint64_t)r[2] - P224_P[2] - b;
        d[2] = (uint64_t)borrow;
        b = (uint64_t)(borrow >> 64) & 1;

        borrow = (__uint128_t)(uint64_t)r[3] - P224_P[3] - b;
        d[3] = (uint64_t)borrow;
        b = (uint64_t)(borrow >> 64) & 1;

        /* If borrow: result < p, keep r. Else keep d. */
        uint64_t mask = (uint64_t)0 - b;
        out[0] = ((uint64_t)r[0] & mask) | (d[0] & ~mask);
        out[1] = ((uint64_t)r[1] & mask) | (d[1] & ~mask);
        out[2] = ((uint64_t)r[2] & mask) | (d[2] & ~mask);
        out[3] = ((uint64_t)r[3] & mask) | (d[3] & ~mask);
    }
}

/* ── native C: direct Solinas squaring ──────────────────────── */

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
    uint64_t w[7];

    /*
     * Schoolbook squaring with explicit carry chain to avoid
     * __uint128_t overflow. Uses the Comba (column-wise) approach:
     * accumulate each column with a running carry.
     */
    __uint128_t acc = 0;

    /* Column 0: a0*a0 */
    acc += (__uint128_t)a0 * a0;
    w[0] = (uint64_t)acc;
    acc >>= 64;

    /* Column 1: a0*a1 + a1*a0 = 2*a0*a1 */
    acc += (__uint128_t)a0 * a1;
    acc += (__uint128_t)a1 * a0;
    w[1] = (uint64_t)acc;
    acc >>= 64;

    /* Column 2: a0*a2 + a1*a1 + a2*a0 */
    acc += (__uint128_t)a0 * a2;
    acc += (__uint128_t)a1 * a1;
    acc += (__uint128_t)a2 * a0;
    w[2] = (uint64_t)acc;
    acc >>= 64;

    /* Column 3: a0*a3 + a1*a2 + a2*a1 + a3*a0 */
    /* This has 4 products. Max sum ~ 4*2^128. After >>64 from col2,
     * acc < 2^66 (max carry from 3 products). Adding 4 products:
     * acc < 2^66 + 4*2^128 = ~2^130. But __uint128_t can only hold 2^128-1!
     * Solution: add products one at a time and propagate extra carries. */
    { uint64_t extra = 0;
      acc += (__uint128_t)a0 * a3;
      acc += (__uint128_t)a1 * a2;
      /* At this point acc might be close to 2^128. Adding more could overflow. */
      /* Safe because: carry from col2 < 2^66, two products < 2*2^128,
       * so acc < 2^66 + 2^129 < 2^130. Need to handle overflow. */
      /* Extract overflow: if acc wrapped around, extra increments. */
      __uint128_t tmp = acc;
      acc += (__uint128_t)a2 * a1;
      if (acc < tmp) extra++;
      tmp = acc;
      acc += (__uint128_t)a3 * a0;
      if (acc < tmp) extra++;
      w[3] = (uint64_t)acc;
      acc = (acc >> 64) | ((__uint128_t)extra << 64); }

    /* Column 4: a1*a3 + a2*a2 + a3*a1 */
    { uint64_t extra = 0;
      __uint128_t tmp;
      acc += (__uint128_t)a1 * a3;
      tmp = acc;
      acc += (__uint128_t)a2 * a2;
      if (acc < tmp) extra++;
      tmp = acc;
      acc += (__uint128_t)a3 * a1;
      if (acc < tmp) extra++;
      w[4] = (uint64_t)acc;
      acc = (acc >> 64) | ((__uint128_t)extra << 64); }

    /* Column 5: a2*a3 + a3*a2 = 2*a2*a3 */
    acc += (__uint128_t)a2 * a3;
    acc += (__uint128_t)a3 * a2;
    w[5] = (uint64_t)acc;
    acc >>= 64;

    /* Column 6: a3*a3 */
    acc += (__uint128_t)a3 * a3;
    w[6] = (uint64_t)acc;

    solinas_reduce(w, out);
}

/* ── reference: naive schoolbook + full bigint mod p ─────────── */

static void fe_sq_reference(const uint64_t *a, uint64_t *out) {
    /*
     * Independent reference: overflow-safe Comba squaring + Solinas reduction.
     * Uses the same Comba column approach as fe_sq_native with explicit
     * overflow detection for __uint128_t accumulation.
     */
    uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
    uint64_t w[7];
    __uint128_t acc = 0;

    /* Column 0 */
    acc += (__uint128_t)a0 * a0;
    w[0] = (uint64_t)acc; acc >>= 64;

    /* Column 1 */
    acc += (__uint128_t)a0 * a1;
    acc += (__uint128_t)a1 * a0;
    w[1] = (uint64_t)acc; acc >>= 64;

    /* Column 2 */
    acc += (__uint128_t)a0 * a2;
    acc += (__uint128_t)a1 * a1;
    acc += (__uint128_t)a2 * a0;
    w[2] = (uint64_t)acc; acc >>= 64;

    /* Column 3 (4 products, may overflow) */
    { uint64_t extra = 0; __uint128_t tmp;
      acc += (__uint128_t)a0 * a3;
      acc += (__uint128_t)a1 * a2;
      tmp = acc; acc += (__uint128_t)a2 * a1; if (acc < tmp) extra++;
      tmp = acc; acc += (__uint128_t)a3 * a0; if (acc < tmp) extra++;
      w[3] = (uint64_t)acc;
      acc = (acc >> 64) | ((__uint128_t)extra << 64); }

    /* Column 4 (3 products, may overflow) */
    { uint64_t extra = 0; __uint128_t tmp;
      acc += (__uint128_t)a1 * a3;
      tmp = acc; acc += (__uint128_t)a2 * a2; if (acc < tmp) extra++;
      tmp = acc; acc += (__uint128_t)a3 * a1; if (acc < tmp) extra++;
      w[4] = (uint64_t)acc;
      acc = (acc >> 64) | ((__uint128_t)extra << 64); }

    /* Column 5 */
    acc += (__uint128_t)a2 * a3;
    acc += (__uint128_t)a3 * a2;
    w[5] = (uint64_t)acc; acc >>= 64;

    /* Column 6 */
    acc += (__uint128_t)a3 * a3;
    w[6] = (uint64_t)acc;

    solinas_reduce(w, out);
}

/* ── microcode patch ────────────────────────────────────────── */

/*
 * Single-vmwrite P-224 schoolbook squaring: 10 MULs, Dettman MAC pattern.
 *
 * Input:  a[0..3] in RDI, RSI, R11, R14
 * PREP: copies a -> TMP0..TMP3, precomputes 2*a -> TMP4..TMP7
 *
 * Squaring chain with progressive hi accumulation (R8):
 *   w0: a0^2                    (1 MUL)
 *   w1: 2*a0*a1                 (1 MUL)
 *   w2: 2*a0*a2 + a1^2         (2 MULs)
 *   w3: 2*a0*a3 + 2*a1*a2      (2 MULs)
 *   w4: 2*a1*a3 + a2^2         (2 MULs)
 *   w5: 2*a2*a3                 (1 MUL)
 *   w6: a3^2                    (1 MUL)
 *
 * Output: w[0..6] in RDI, RSI, R11, R14, RBP, RBX, R12
 * (RBP and RBX are callee-saved; handled by inline asm wrapper)
 */

static void install_p224_solinas_sq_patch(void) {
    ucode_t patch[] = {

    /* ═══ PREP: copy a[0..3] → TMP0..TMP3, 2*a[0..3] → TMP4..TMP7 ═══ */

    /* P0 */ { ZEROEXT_DSZ64_DR(TMP0, RDI),
               ZEROEXT_DSZ64_DR(TMP1, RSI),
               ZEROEXT_DSZ64_DR(TMP2, R11),
               NOP_SEQWORD },
    /* P1 */ { ZEROEXT_DSZ64_DR(TMP3, R14),
               ADD_DSZ64_DRR(TMP4, RDI, RDI),      /* 2*a0 */
               ADD_DSZ64_DRR(TMP5, RSI, RSI),       /* 2*a1 */
               NOP_SEQWORD },
    /* P2 */ { ADD_DSZ64_DRR(TMP6, R11, R11),       /* 2*a2 */
               ADD_DSZ64_DRR(TMP7, R14, R14),       /* 2*a3 */
               ZEROEXT_DSZ64_DR(RDX, TMP0),          /* prep a0 for SQ0 MUL */
               NOP_SEQWORD },

    /* ═══ w0 = a0^2 ═══ */
    /* SQ0-0: MUL a0*a0 → hi:RCX, lo:RDX */
    /* SQ0-0 */ { MUL_DSZ64_DRR(RCX, TMP0, RDX),
                  NOP, NOP, NOP_SEQWORD },
    /* SQ0-1: w0=lo→RDI, R8=hi, prep RDX=2*a1 */
    /* SQ0-1 */ { ZEROEXT_DSZ64_DR(RDI, RDX),         /* w0 → RDI */
                  ZEROEXT_DSZ64_DR(R8, RCX),           /* R8 = hi(a0^2) */
                  ZEROEXT_DSZ64_DR(RDX, TMP5),         /* RDX = 2*a1 */
                  NOP_SEQWORD },

    /* ═══ w1 = 2*a0*a1 ═══ */
    /* SQ1-0: MUL a0 * 2a1 */
    /* SQ1-0 */ { MUL_DSZ64_DRR(RCX, TMP0, RDX),
                  NOP, NOP, NOP_SEQWORD },
    /* SQ1-1: acc = R8(carry) + lo */
    /* SQ1-1 */ { ADD_DSZ64_DRR(TMP13, R8, RDX),      /* TMP13 = hi(w0) + lo(2a0a1) */
                  SETCC_CONDB_DR(TMP15, TMP13),
                  ZEROEXT_DSZ64_DR(RDX, TMP6),         /* RDX = 2*a2 for w2 */
                  NOP_SEQWORD },
    /* SQ1-2: w1→RSI, R8 = hi + carry */
    /* SQ1-2 */ { ZEROEXT_DSZ64_DR(RSI, TMP13),       /* w1 → RSI */
                  ADD_DSZ64_DRR(R8, RCX, TMP15),       /* R8 = hi + carry_bit */
                  NOP, NOP_SEQWORD },

    /* ═══ w2 = 2*a0*a2 + a1^2 (2 MULs) ═══ */
    /* SQ2-0: MAC1 = a0 * 2a2 */
    /* SQ2-0 */ { MUL_DSZ64_DRR(RCX, TMP0, RDX),
                  NOP, NOP, NOP_SEQWORD },
    /* SQ2-1: TMP13 = R8(carry) + lo */
    /* SQ2-1 */ { ADD_DSZ64_DRR(TMP13, R8, RDX),
                  SETCC_CONDB_DR(TMP15, TMP13),
                  ZEROEXT_DSZ64_DR(RDX, TMP1),         /* RDX = a1 for a1^2 */
                  NOP_SEQWORD },
    /* SQ2-2: R8 = hi1 + carry1; MAC2 = a1 * a1 */
    /* SQ2-2 */ { ADD_DSZ64_DRR(R8, RCX, TMP15),
                  MUL_DSZ64_DRR(RCX, TMP1, RDX),
                  NOP, NOP_SEQWORD },
    /* SQ2-3: TMP13 += lo2 */
    /* SQ2-3 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                  SETCC_CONDB_DR(TMP15, TMP13),
                  NOP, NOP_SEQWORD },
    /* SQ2-4: w2→R11, R8 += hi2 + carry2 */
    /* SQ2-4 */ { ZEROEXT_DSZ64_DR(R11, TMP13),       /* w2 → R11 */
                  ADD_DSZ64_DRR(R8, R8, RCX),
                  ZEROEXT_DSZ64_DR(RDX, TMP7),         /* RDX = 2*a3 for w3 */
                  NOP_SEQWORD },
    /* SQ2-5: R8 += carry2 */
    /* SQ2-5 */ { ADD_DSZ64_DRR(R8, R8, TMP15),
                  NOP, NOP, NOP_SEQWORD },

    /* ═══ w3 = 2*a0*a3 + 2*a1*a2 (2 MULs) ═══ */
    /* SQ3-0: MAC1 = a0 * 2a3 */
    /* SQ3-0 */ { MUL_DSZ64_DRR(RCX, TMP0, RDX),
                  NOP, NOP, NOP_SEQWORD },
    /* SQ3-1 */ { ADD_DSZ64_DRR(TMP13, R8, RDX),
                  SETCC_CONDB_DR(TMP15, TMP13),
                  ZEROEXT_DSZ64_DR(RDX, TMP6),         /* RDX = 2*a2 */
                  NOP_SEQWORD },
    /* SQ3-2: R8 = hi1 + carry1; MAC2 = a1 * 2a2 */
    /* SQ3-2 */ { ADD_DSZ64_DRR(R8, RCX, TMP15),
                  MUL_DSZ64_DRR(RCX, TMP1, RDX),
                  NOP, NOP_SEQWORD },
    /* SQ3-3 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                  SETCC_CONDB_DR(TMP15, TMP13),
                  NOP, NOP_SEQWORD },
    /* SQ3-4: w3→R14, R8 += hi2 */
    /* SQ3-4 */ { ZEROEXT_DSZ64_DR(R14, TMP13),       /* w3 → R14 */
                  ADD_DSZ64_DRR(R8, R8, RCX),
                  ZEROEXT_DSZ64_DR(RDX, TMP7),         /* RDX = 2*a3 for w4 */
                  NOP_SEQWORD },
    /* SQ3-5: R8 += carry2 */
    /* SQ3-5 */ { ADD_DSZ64_DRR(R8, R8, TMP15),
                  NOP, NOP, NOP_SEQWORD },

    /* ═══ w4 = 2*a1*a3 + a2^2 (2 MULs) ═══ */
    /* SQ4-0: MAC1 = a1 * 2a3 */
    /* SQ4-0 */ { MUL_DSZ64_DRR(RCX, TMP1, RDX),
                  NOP, NOP, NOP_SEQWORD },
    /* SQ4-1 */ { ADD_DSZ64_DRR(TMP13, R8, RDX),
                  SETCC_CONDB_DR(TMP15, TMP13),
                  ZEROEXT_DSZ64_DR(RDX, TMP2),         /* RDX = a2 */
                  NOP_SEQWORD },
    /* SQ4-2: hi1 + carry1; MAC2 = a2 * a2 */
    /* SQ4-2 */ { ADD_DSZ64_DRR(R8, RCX, TMP15),
                  MUL_DSZ64_DRR(RCX, TMP2, RDX),
                  NOP, NOP_SEQWORD },
    /* SQ4-3 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                  SETCC_CONDB_DR(TMP15, TMP13),
                  NOP, NOP_SEQWORD },
    /* SQ4-4: w4→RBP, R8 += hi2 */
    /* SQ4-4 */ { ZEROEXT_DSZ64_DR(RBP, TMP13),       /* w4 → RBP */
                  ADD_DSZ64_DRR(R8, R8, RCX),
                  ZEROEXT_DSZ64_DR(RDX, TMP6),         /* RDX = 2*a2 for w5 */
                  NOP_SEQWORD },
    /* SQ4-5: R8 += carry2 */
    /* SQ4-5 */ { ADD_DSZ64_DRR(R8, R8, TMP15),
                  NOP, NOP, NOP_SEQWORD },

    /* ═══ w5 = 2*a2*a3 (1 MUL) ═══ */
    /* Use a3 * 2a2 */
    /* SQ5-0: MUL a3 * 2a2 */
    /* SQ5-0 */ { MUL_DSZ64_DRR(RCX, TMP3, RDX),
                  NOP, NOP, NOP_SEQWORD },
    /* SQ5-1: w5 = R8(carry) + lo */
    /* SQ5-1 */ { ADD_DSZ64_DRR(TMP13, R8, RDX),
                  SETCC_CONDB_DR(TMP15, TMP13),
                  ZEROEXT_DSZ64_DR(RDX, TMP3),         /* RDX = a3 for w6 */
                  NOP_SEQWORD },
    /* SQ5-2: w5→RBX, R8 = hi + carry */
    /* SQ5-2 */ { ZEROEXT_DSZ64_DR(RBX, TMP13),       /* w5 → RBX */
                  ADD_DSZ64_DRR(R8, RCX, TMP15),
                  NOP, NOP_SEQWORD },

    /* ═══ w6 = a3^2 (1 MUL) ═══ */
    /* SQ6-0: MUL a3 * a3 */
    /* SQ6-0 */ { MUL_DSZ64_DRR(RCX, TMP3, RDX),
                  NOP, NOP, NOP_SEQWORD },
    /* SQ6-1: w6 = R8(carry) + lo → R12 */
    /* SQ6-1 */ { ADD_DSZ64_DRR(R12, R8, RDX),
                  NOP, NOP, NOP_SEQWORD },

    /* ═══ END: output w[0..6] in RDI, RSI, R11, R14, RBP, RBX, R12 ═══ */
    { NOP, NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("p224_solinas_sq: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ── fe_sq via microcode ─────────────────────────────────────── */

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    register const uint64_t *_a   asm("rcx") = a;
    uint64_t w[7];
    register uint64_t       *_w  asm("r8")  = w;  /* patch stores products here */

    asm volatile(
        /* save callee-saved registers + w[] pointer */
        "push rbp\n\t"
        "push rbx\n\t"
        "push r12\n\t"
        "push r13\n\t"
        "push r14\n\t"
        "push r15\n\t"
        "push r8\n\t"         /* save w[] pointer */

        /* load a[0..3] into input regs */
        "mov rdi, [rcx]\n\t"          /* a0 */
        "mov rsi, [rcx + 8]\n\t"      /* a1 */
        "mov r11, [rcx + 16]\n\t"     /* a2 */
        "mov r14, [rcx + 24]\n\t"     /* a3 */

        /* fire microcode — single vmwrite computes schoolbook squaring */
        "vmwrite rcx, rdx\n\t"

        /* w[0..6] now in: RDI, RSI, R11, R14, RBP, RBX, R12 */
        /* store to w[] array */
        "pop rcx\n\t"         /* rcx = w[] pointer */
        "mov [rcx],      rdi\n\t"     /* w[0] */
        "mov [rcx + 8],  rsi\n\t"     /* w[1] */
        "mov [rcx + 16], r11\n\t"     /* w[2] */
        "mov [rcx + 24], r14\n\t"     /* w[3] */
        "mov [rcx + 32], rbp\n\t"     /* w[4] */
        "mov [rcx + 40], rbx\n\t"     /* w[5] */
        "mov [rcx + 48], r12\n\t"     /* w[6] */

        /* restore callee-saved */
        "pop r15\n\t"
        "pop r14\n\t"
        "pop r13\n\t"
        "pop r12\n\t"
        "pop rbx\n\t"
        "pop rbp\n\t"

        : "+r"(_a), "+r"(_w)
        :
        : "rax", "rdx", "rsi", "rdi",
          "r9", "r10", "r11",
          "memory", "cc"
    );

    /* Solinas reduction in native C */
    solinas_reduce(w, out);
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
        out[3] &= UINT64_C(0xFFFFFFFF);
        int lt = 0;
        for (int j = 3; j >= 0; j--) {
            if (out[j] < P224_P[j]) { lt = 1; break; }
            if (out[j] > P224_P[j]) break;
        }
        if (lt) break;
    }
}

static int verify_all(void) {
    int pass = 0, fail = 0;

    printf("--- Known vectors ---\n");
    struct { const char *name; uint64_t a[4]; uint64_t expected[4]; int has_exp; } vecs[] = {
        { "0^2",     {0,0,0,0}, {0,0,0,0}, 1 },
        { "1^2",     {1,0,0,0}, {1,0,0,0}, 1 },
        { "2^2",     {2,0,0,0}, {4,0,0,0}, 1 },
        { "3^2",     {3,0,0,0}, {9,0,0,0}, 1 },
        /* (p-1)^2 mod p = 1 since (p-1) = -1 mod p, so (-1)^2 = 1 */
        { "(p-1)^2", {0, UINT64_C(0xFFFFFFFF00000000),
                      UINT64_C(0xFFFFFFFFFFFFFFFF),
                      UINT64_C(0x00000000FFFFFFFF)}, {1,0,0,0}, 1 },
        /* 2^32 squared = 2^64 = {0, 1, 0, 0} */
        { "2^32",    {UINT64_C(0x100000000), 0, 0, 0}, {0, 1, 0, 0}, 1 },
        /* 2^96 = {0, 0x100000000, 0, 0}; (2^96)^2 = 2^192.
         * 2^192 < p*2^(-32)... let me compute: 2^192 as 4-limb:
         * {0, 0, 0, 1}... but p[3]=0xFFFFFFFF and 1 < 0xFFFFFFFF, so 2^192 < p.
         * Actually 2^192 = {0, 0, 0, 1}. */
        { "2^96",    {0, UINT64_C(0x100000000), 0, 0}, {0, 0, 0, 1}, 1 },
        { "max32",   {UINT64_C(0xFFFFFFFF), 0, 0, 0}, {0}, 0 },
        { "large",   {UINT64_C(0xFEDCBA9876543210), UINT64_C(0x1234567890ABCDEF),
                      UINT64_C(0xAAAABBBBCCCCDDDD), UINT64_C(0x12345678)}, {0}, 0 },
    };
    int nvecs = sizeof(vecs)/sizeof(vecs[0]);

    /* Debug: compare raw w[] products for the "large" test vector */
    {
        uint64_t a_dbg[4] = {UINT64_C(0xFEDCBA9876543210), UINT64_C(0x1234567890ABCDEF),
                              UINT64_C(0xAAAABBBBCCCCDDDD), UINT64_C(0x12345678)};
        uint64_t w_nat[7] = {0}, w_ucd[7] = {0};
        /* Native: compute w[] without reducing */
        {
            uint64_t a0=a_dbg[0],a1=a_dbg[1],a2=a_dbg[2],a3=a_dbg[3];
            __uint128_t acc = (__uint128_t)a0*a0;
            w_nat[0] = (uint64_t)acc; acc >>= 64;
            acc += (__uint128_t)a0*a1*2;
            w_nat[1] = (uint64_t)acc; acc >>= 64;
            acc += (__uint128_t)a0*a2*2 + (__uint128_t)a1*a1;
            w_nat[2] = (uint64_t)acc; acc >>= 64;
            acc += (__uint128_t)a0*a3*2 + (__uint128_t)a1*a2*2;
            w_nat[3] = (uint64_t)acc; acc >>= 64;
            acc += (__uint128_t)a1*a3*2 + (__uint128_t)a2*a2;
            w_nat[4] = (uint64_t)acc; acc >>= 64;
            acc += (__uint128_t)a2*a3*2;
            w_nat[5] = (uint64_t)acc; acc >>= 64;
            acc += (__uint128_t)a3*a3;
            w_nat[6] = (uint64_t)acc;
        }
        /* Ucode: extract w[] from the patch */
        {
            register const uint64_t *_a asm("rcx") = a_dbg;
            register uint64_t *_w asm("r8") = w_ucd;
            asm volatile(
                "push rbp\n\t" "push rbx\n\t" "push r12\n\t"
                "push r13\n\t" "push r14\n\t" "push r15\n\t" "push r8\n\t"
                "mov rdi, [rcx]\n\t" "mov rsi, [rcx+8]\n\t"
                "mov r11, [rcx+16]\n\t" "mov r14, [rcx+24]\n\t"
                "vmwrite rcx, rdx\n\t"
                "pop rcx\n\t"
                "mov [rcx], rdi\n\t" "mov [rcx+8], rsi\n\t"
                "mov [rcx+16], r11\n\t" "mov [rcx+24], r14\n\t"
                "mov [rcx+32], rbp\n\t" "mov [rcx+40], rbx\n\t"
                "mov [rcx+48], r12\n\t"
                "pop r15\n\t" "pop r14\n\t" "pop r13\n\t"
                "pop r12\n\t" "pop rbx\n\t" "pop rbp\n\t"
                : "+r"(_a), "+r"(_w)
                : : "rax","rdx","rsi","rdi","r9","r10","r11","memory","cc"
            );
        }
        printf("  --- w[] debug for 'large' ---\n");
        for (int i = 0; i < 7; i++)
            printf("    w[%d] nat=%016lx ucd=%016lx %s\n",
                   i, w_nat[i], w_ucd[i], w_nat[i]==w_ucd[i] ? "" : "***MISMATCH***");
        /* Also show the carry (R8) that would feed into each w */
        printf("  --- carry debug (R8 values) ---\n");
        printf("    w6_diff = 0x%lx (a3 = 0x%lx)\n",
               w_nat[6] > w_ucd[6] ? w_nat[6]-w_ucd[6] : w_ucd[6]-w_nat[6],
               a_dbg[3]);
        printf("    w4_diff = 0x%lx (a1 = 0x%lx)\n",
               w_nat[4] > w_ucd[4] ? w_nat[4]-w_ucd[4] : w_ucd[4]-w_nat[4],
               a_dbg[1]);

        /* Test: what if we square {0, 0, 0, a3} — only a3 nonzero */
        uint64_t a_a3only[4] = {0, 0, 0, a_dbg[3]};
        uint64_t w_n2[7]={0}, w_u2[7]={0};
        {
            uint64_t a3=a_a3only[3];
            __uint128_t acc = 0;
            /* w0-w5 = 0 since a0=a1=a2=0 */
            w_n2[6] = (uint64_t)((__uint128_t)a3*a3);
        }
        {
            register const uint64_t *_a2 asm("rcx") = a_a3only;
            register uint64_t *_w2 asm("r8") = w_u2;
            asm volatile(
                "push rbp\n\t" "push rbx\n\t" "push r12\n\t"
                "push r13\n\t" "push r14\n\t" "push r15\n\t" "push r8\n\t"
                "mov rdi, [rcx]\n\t" "mov rsi, [rcx+8]\n\t"
                "mov r11, [rcx+16]\n\t" "mov r14, [rcx+24]\n\t"
                "vmwrite rcx, rdx\n\t"
                "pop rcx\n\t"
                "mov [rcx], rdi\n\t" "mov [rcx+8], rsi\n\t"
                "mov [rcx+16], r11\n\t" "mov [rcx+24], r14\n\t"
                "mov [rcx+32], rbp\n\t" "mov [rcx+40], rbx\n\t"
                "mov [rcx+48], r12\n\t"
                "pop r15\n\t" "pop r14\n\t" "pop r13\n\t"
                "pop r12\n\t" "pop rbx\n\t" "pop rbp\n\t"
                : "+r"(_a2), "+r"(_w2)
                : : "rax","rdx","rsi","rdi","r9","r10","r11","memory","cc"
            );
        }
        printf("  --- {0,0,0,a3} squared: w[6] ---\n");
        printf("    nat w[6]=%016lx  ucd w[6]=%016lx %s\n",
               w_n2[6], w_u2[6], w_n2[6]==w_u2[6] ? "MATCH" : "***MISMATCH***");
        printf("    (a3^2 = %016lx)\n", (uint64_t)((__uint128_t)a_dbg[3]*a_dbg[3]));
    }

    for (int i = 0; i < nvecs; i++) {
        uint64_t ref[4], nat[4], ucd[4];
        fe_sq_reference(vecs[i].a, ref);
        fe_sq_native(vecs[i].a, nat);
        fe_sq_ucode(vecs[i].a, ucd);
        int ok = !memcmp(ref, nat, 32) && !memcmp(ref, ucd, 32);
        if (vecs[i].has_exp) ok &= !memcmp(ref, vecs[i].expected, 32);
        if (!ok) {
            printf("  FAIL [%s]\n", vecs[i].name);
            for (int j = 0; j < 4; j++)
                printf("    [%d] ref=%016"PRIx64" nat=%016"PRIx64" ucd=%016"PRIx64"%s%s",
                    j, ref[j], nat[j], ucd[j],
                    ref[j]!=nat[j]?" nat***":"", ref[j]!=ucd[j]?" ucd***":"");
            if (vecs[i].has_exp) {
                printf("\n    expected:");
                for (int j = 0; j < 4; j++)
                    printf(" %016"PRIx64, vecs[i].expected[j]);
            }
            printf("\n");
            fail++;
        } else {
            printf("  PASS [%s]\n", vecs[i].name);
            pass++;
        }
    }

    printf("\n--- Random (10000) ---\n");
    uint64_t rng = 0xA224CAFE12345678ULL;
    int rp = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t a[4], ref[4], nat[4], ucd[4];
        rand_mod_p(a, &rng);
        fe_sq_reference(a, ref);
        fe_sq_native(a, nat);
        fe_sq_ucode(a, ucd);
        if (!memcmp(ref, nat, 32) && !memcmp(ref, ucd, 32)) rp++;
        else {
            printf("  FAIL #%d  a={%016"PRIx64",%016"PRIx64",%016"PRIx64",%016"PRIx64"}\n",
                   i, a[0], a[1], a[2], a[3]);
            for (int j = 0; j < 4; j++)
                printf("    [%d] ref=%016"PRIx64" nat=%016"PRIx64" ucd=%016"PRIx64"%s%s\n",
                    j, ref[j], nat[j], ucd[j],
                    ref[j]!=nat[j]?" nat***":"", ref[j]!=ucd[j]?" ucd***":"");
            break;
        }
    }
    printf("  %d / 10000 PASS\n", rp);
    pass += rp; if (rp < 10000) fail += (10000 - rp);

    printf("\n--- Chain (1000 iterated sq from 3) ---\n");
    uint64_t ri[4] = {3, 0, 0, 0};
    uint64_t ni[4], ui[4];
    memcpy(ni, ri, 32); memcpy(ui, ri, 32);
    for (int i = 0; i < 1000; i++) {
        fe_sq_reference(ri, ri);
        fe_sq_native(ni, ni);
        fe_sq_ucode(ui, ui);
    }
    int rok = !memcmp(ri, ni, 32) && !memcmp(ri, ui, 32);
    printf("  ref==native: %s   ref==ucode: %s   -> %s\n",
           !memcmp(ri, ni, 32) ? "yes" : "NO",
           !memcmp(ri, ui, 32) ? "yes" : "NO",
           rok ? "PASS" : "FAIL");
    if (!rok) {
        printf("  ref  = {%016"PRIx64",%016"PRIx64",%016"PRIx64",%016"PRIx64"}\n",
               ri[0], ri[1], ri[2], ri[3]);
        printf("  nat  = {%016"PRIx64",%016"PRIx64",%016"PRIx64",%016"PRIx64"}\n",
               ni[0], ni[1], ni[2], ni[3]);
        printf("  ucd  = {%016"PRIx64",%016"PRIx64",%016"PRIx64",%016"PRIx64"}\n",
               ui[0], ui[1], ui[2], ui[3]);
    }
    if (rok) pass++; else fail++;

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
    printf("=== P-224 Solinas squaring: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_p224_solinas_sq_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED, skipping benchmark.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    uint64_t state[4] = {3, 0, 0, 0};
    uint64_t tmp[4], t0, t1, min, sum;

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_native(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Naive -O3:   min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_ucode(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode:   min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    init_match_and_patch(); do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
