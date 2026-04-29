/*
 * asm_op_p384_sq.c — Montgomery squaring for P-384 via microcode
 *
 * 1-iteration-per-vmwrite patch, called 6x via vmwrite.
 * P-384: p = 2^384 - 2^128 - 2^96 + 2^32 - 1
 *   p[0] = 0x00000000FFFFFFFF
 *   p[1] = 0xFFFFFFFF00000000
 *   p[2] = 0xFFFFFFFFFFFFFFFE
 *   p[3] = p[4] = p[5] = 0xFFFFFFFFFFFFFFFF
 *   mu = -p[0]^{-1} mod 2^64 = 0x100000001
 *
 * 7-word accumulator: acc[0..4] in arch regs, acc[5] and acc[6] passed
 * via RDX and RCX at vmwrite entry.
 *
 * Build:  make PROG=asm_op_p384_sq
 * Run:    sudo taskset -c 0 ./asm_op_p384_sq_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

static const uint64_t P384_P[6] = {
    UINT64_C(0x00000000FFFFFFFF), UINT64_C(0xFFFFFFFF00000000),
    UINT64_C(0xFFFFFFFFFFFFFFFE), UINT64_C(0xFFFFFFFFFFFFFFFF),
    UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0xFFFFFFFFFFFFFFFF)
};

#define P384_MU UINT64_C(0x100000001)

/* ── fiat-crypto reference (the REAL GCC baseline CryptOpt compares against) ── */
#include "../curvesC/p384_square.c"

static void fe_sq_fiat(const uint64_t *a, uint64_t *out) {
    fiat_p384_square(out, a);
}

/* ── fe_sq native C ──────────────────────────────────────────── */

static inline void mont_iteration(uint64_t acc[7], uint64_t a_i,
                                   const uint64_t *b) {
    __uint128_t t;
    uint64_t c;

    /* Phase A: schoolbook a_i * b[0..5] → product t0..t5, t6 */
    t = (__uint128_t)a_i * b[0];
    uint64_t t0 = (uint64_t)t, t0h = (uint64_t)(t >> 64);
    t = (__uint128_t)a_i * b[1] + t0h;
    uint64_t t1 = (uint64_t)t, t1h = (uint64_t)(t >> 64);
    t = (__uint128_t)a_i * b[2] + t1h;
    uint64_t t2 = (uint64_t)t, t2h = (uint64_t)(t >> 64);
    t = (__uint128_t)a_i * b[3] + t2h;
    uint64_t t3 = (uint64_t)t, t3h = (uint64_t)(t >> 64);
    t = (__uint128_t)a_i * b[4] + t3h;
    uint64_t t4 = (uint64_t)t, t4h = (uint64_t)(t >> 64);
    t = (__uint128_t)a_i * b[5] + t4h;
    uint64_t t5 = (uint64_t)t, t6 = (uint64_t)(t >> 64);

    /* Phase A': accumulate into acc[0..6] */
    t = (__uint128_t)acc[0] + t0; acc[0] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[1] + t1 + c; acc[1] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[2] + t2 + c; acc[2] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[3] + t3 + c; acc[3] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[4] + t4 + c; acc[4] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[5] + t5 + c; acc[5] = (uint64_t)t; c = (uint64_t)(t >> 64);
    __uint128_t acc6_full = (__uint128_t)acc[6] + t6 + c;
    uint64_t acc6 = (uint64_t)acc6_full;
    uint64_t acc6_hi = (uint64_t)(acc6_full >> 64);

    /* Phase B: m = acc[0] * mu, then m * p[0..5] → red[0..6] */
    uint64_t m = acc[0] * P384_MU;

    t = (__uint128_t)m * P384_P[0];  /* m * 0xFFFFFFFF */
    uint64_t mp0_lo = (uint64_t)t, mp0_hi = (uint64_t)(t >> 64);
    t = (__uint128_t)m * P384_P[1];  /* m * 0xFFFFFFFF00000000 */
    uint64_t mp1_lo = (uint64_t)t, mp1_hi = (uint64_t)(t >> 64);
    t = (__uint128_t)m * P384_P[2];  /* m * 0xFFFFFFFFFFFFFFFE */
    uint64_t mp2_lo = (uint64_t)t, mp2_hi = (uint64_t)(t >> 64);
    t = (__uint128_t)m * UINT64_C(0xFFFFFFFFFFFFFFFF);  /* m * R8 for p[3..5] */
    uint64_t mpR8_lo = (uint64_t)t, mpR8_hi = (uint64_t)(t >> 64);

    /* Chain: red[0..6] from m * p products */
    uint64_t red0 = mp0_lo;
    t = (__uint128_t)mp0_hi + mp1_lo;
    uint64_t red1 = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)mp1_hi + mp2_lo + c;
    uint64_t red2 = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)mp2_hi + mpR8_lo + c;
    uint64_t red3 = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)mpR8_hi + mpR8_lo + c;
    uint64_t red4 = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)mpR8_hi + mpR8_lo + c;
    uint64_t red5 = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)mpR8_hi + c;
    uint64_t red6 = (uint64_t)t;
    uint64_t red6_hi = (uint64_t)(t >> 64);

    /* Phase C: acc += red, shift down (discard word 0) */
    t = (__uint128_t)acc[0] + red0;  c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[1] + red1 + c; acc[0] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[2] + red2 + c; acc[1] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[3] + red3 + c; acc[2] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[4] + red4 + c; acc[3] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[5] + red5 + c; acc[4] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc6 + red6 + c; acc[5] = (uint64_t)t;
    acc[6] = (uint64_t)(t >> 64) + acc6_hi + red6_hi;
}

static inline void cond_subtract(const uint64_t acc[7], uint64_t *out) {
    uint64_t diff[6];
    __uint128_t b128;
    b128 = (__uint128_t)acc[0] - UINT64_C(0x00000000FFFFFFFF);
    diff[0] = (uint64_t)b128;
    b128 = (__uint128_t)acc[1] - UINT64_C(0xFFFFFFFF00000000) - ((uint64_t)(b128 >> 64) & 1);
    diff[1] = (uint64_t)b128;
    b128 = (__uint128_t)acc[2] - UINT64_C(0xFFFFFFFFFFFFFFFE) - ((uint64_t)(b128 >> 64) & 1);
    diff[2] = (uint64_t)b128;
    b128 = (__uint128_t)acc[3] - UINT64_C(0xFFFFFFFFFFFFFFFF) - ((uint64_t)(b128 >> 64) & 1);
    diff[3] = (uint64_t)b128;
    b128 = (__uint128_t)acc[4] - UINT64_C(0xFFFFFFFFFFFFFFFF) - ((uint64_t)(b128 >> 64) & 1);
    diff[4] = (uint64_t)b128;
    b128 = (__uint128_t)acc[5] - UINT64_C(0xFFFFFFFFFFFFFFFF) - ((uint64_t)(b128 >> 64) & 1);
    diff[5] = (uint64_t)b128;
    b128 = (__uint128_t)acc[6] - 0 - ((uint64_t)(b128 >> 64) & 1);
    uint64_t mask = (uint64_t)0 - ((uint64_t)(b128 >> 64) & 1);
    out[0] = (acc[0] & mask) | (diff[0] & ~mask);
    out[1] = (acc[1] & mask) | (diff[1] & ~mask);
    out[2] = (acc[2] & mask) | (diff[2] & ~mask);
    out[3] = (acc[3] & mask) | (diff[3] & ~mask);
    out[4] = (acc[4] & mask) | (diff[4] & ~mask);
    out[5] = (acc[5] & mask) | (diff[5] & ~mask);
}

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    uint64_t acc[7] = {0};
    mont_iteration(acc, a[0], a);
    mont_iteration(acc, a[1], a);
    mont_iteration(acc, a[2], a);
    mont_iteration(acc, a[3], a);
    mont_iteration(acc, a[4], a);
    mont_iteration(acc, a[5], a);
    cond_subtract(acc, out);
}

/* ── reference ───────────────────────────────────────────────── */

static void fe_sq_reference(const uint64_t *a, uint64_t *out) {
    uint64_t acc[7] = {0};
    for (int i = 0; i < 6; i++) {
        __uint128_t c = 0;
        for (int j = 0; j < 6; j++) {
            c += (__uint128_t)a[i] * a[j] + acc[j];
            acc[j] = (uint64_t)c; c >>= 64;
        }
        __uint128_t word6 = (__uint128_t)acc[6] + (uint64_t)c;
        uint64_t m = acc[0] * P384_MU;
        c = 0;
        for (int j = 0; j < 6; j++) {
            c += (__uint128_t)m * P384_P[j] + acc[j];
            acc[j] = (uint64_t)c; c >>= 64;
        }
        word6 += (uint64_t)c;
        acc[0]=acc[1]; acc[1]=acc[2]; acc[2]=acc[3];
        acc[3]=acc[4]; acc[4]=acc[5];
        acc[5]=(uint64_t)word6; acc[6]=(uint64_t)(word6>>64);
    }
    cond_subtract(acc, out);
}

/* ── microcode ───────────────────────────────────────────────── */

/*
 * P-384 Montgomery: 1-iteration-per-vmwrite, 6 vmwrites total.
 *
 * Register allocation at vmwrite entry:
 *   R15=acc[0]  R9=acc[1]  R10=acc[2]  R13=acc[3]  RAX=acc[4]
 *   RDX=acc[5]  RCX=acc[6]  (passed via vmwrite operands)
 *   RSI=b[0]  R12=b[1]  R11=b[2]  R14=b[3]  RBX=b[4]  RBP=b[5]
 *   R8=0xFFFFFFFFFFFFFFFF  (p[3]=p[4]=p[5])
 *   RDI=a[i]
 *
 * PREP (4 triads):
 *   T0: TMP10←RSI(b0), TMP11←R12(b1), TMP12←R11(b2)
 *   T1: TMP13←R14(b3), TMP14←RBX(b4), TMP15←RBP(b5)
 *   T2: RSI←RDX(acc5), R12←RCX(acc6)  — b regs now free for acc use
 *   T3: (reserved for additional setup if needed)
 *
 * After PREP: b in TMP10-15, acc[5] in RSI, acc[6] in R12.
 * R11/R14/RBX/RBP free for Phase B constants and scratch.
 *
 * Phase A: 6 MULs schoolbook (RDI × TMP10-15) → w0-w6
 * Phase A': 7-word accumulate into R15/R9/R10/R13/RAX/RSI/R12
 * Phase B: mu MUL + 4 p-limb MULs (p0,p1,p2,R8) + chain → red[0..6]
 * Phase C: 7-word add + shift
 *
 * At end: acc[0..4] in R15/R9/R10/R13/RAX, acc[5] in RSI, acc[6] in R12.
 * Inline asm reads RSI/R12, stores to stack, reloads b, repeats.
 */

#define MONT_ITER \
    /* ── PHASE A: schoolbook a_i(RDI) × b(TMP10-15) — 22 triads ── */ \
    /* Precompute p[0..2] in free slots for Phase B */ \
    /* T0: setup RDX=a_i, p[0]=SHR(R8,32)→R11, p[1]=SHL(R8,32)→R14 */ \
    { ZEROEXT_DSZ64_DR(RDX, RDI), SHR_DSZ64_DRI(R11, R8, 32), \
      SHL_DSZ64_DRI(R14, R8, 32), NOP_SEQWORD }, \
    /* T1: MUL b0*ai */ \
    { MUL_DSZ64_DRR(RCX, TMP10, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* T2: save w0_lo=TMP0, w0_hi=TMP1, reload RDX=ai */ \
    { ZEROEXT_DSZ64_DR(TMP0, RDX), ZEROEXT_DSZ64_DR(TMP1, RCX), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    /* T3: MUL b1*ai + Phase A' w0 merge (ADD+SETCC in slots 1-2) */ \
    { MUL_DSZ64_DRR(RCX, TMP11, RDX), ADD_DSZ64_DRR(TMP0, R15, TMP0), \
      SETCC_CONDB_DR(TMP3, TMP0), NOP_SEQWORD }, \
    /* T4: w1 chain — carry in TMP8 (not TMP3, which holds Phase A' w0 carry) */ \
    { ADD_DSZ64_DRR(TMP2, TMP1, RDX), SETCC_CONDB_DR(TMP8, TMP2), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    /* T5: reload RDX=ai, precompute p[2]=R8+R8→RBX */ \
    { ZEROEXT_DSZ64_DR(RDX, RDI), ADD_DSZ64_DRR(RBX, R8, R8), \
      NOP, NOP_SEQWORD }, \
    /* T6: MUL b2*ai */ \
    { MUL_DSZ64_DRR(RCX, TMP12, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* T7: w2_base = hi(b1*ai) + lo(b2*ai) */ \
    { ADD_DSZ64_DRR(TMP4, TMP1, RDX), SETCC_CONDB_DR(TMP5, TMP4), \
      NOP, NOP_SEQWORD }, \
    /* T8: w2 = w2_base + carry(TMP8) */ \
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP8), SETCC_CONDB_DR(TMP6, TMP4), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    /* T9: combine w2 carries, reload RDX */ \
    { ADD_DSZ64_DRR(TMP8, TMP5, TMP6), ZEROEXT_DSZ64_DR(RDX, RDI), \
      NOP, NOP_SEQWORD }, \
    /* T10: MUL b3*ai */ \
    { MUL_DSZ64_DRR(RCX, TMP13, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* T11: w3_base = hi(b2*ai) + lo(b3*ai) */ \
    { ADD_DSZ64_DRR(TMP5, TMP1, RDX), SETCC_CONDB_DR(TMP6, TMP5), \
      NOP, NOP_SEQWORD }, \
    /* T12: w3 = w3_base + carry(TMP8) */ \
    { ADD_DSZ64_DRR(TMP5, TMP5, TMP8), SETCC_CONDB_DR(TMP7, TMP5), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    /* T13: combine w3 carries, reload RDX */ \
    { ADD_DSZ64_DRR(TMP8, TMP6, TMP7), ZEROEXT_DSZ64_DR(RDX, RDI), \
      NOP, NOP_SEQWORD }, \
    /* T14: MUL b4*ai */ \
    { MUL_DSZ64_DRR(RCX, TMP14, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* T15: w4_base = hi(b3*ai) + lo(b4*ai) */ \
    { ADD_DSZ64_DRR(TMP6, TMP1, RDX), SETCC_CONDB_DR(TMP7, TMP6), \
      NOP, NOP_SEQWORD }, \
    /* T16: w4 = w4_base + carry(TMP8) */ \
    { ADD_DSZ64_DRR(TMP6, TMP6, TMP8), SETCC_CONDB_DR(TMP9, TMP6), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    /* T17: combine w4 carries, reload RDX */ \
    { ADD_DSZ64_DRR(TMP8, TMP7, TMP9), ZEROEXT_DSZ64_DR(RDX, RDI), \
      NOP, NOP_SEQWORD }, \
    /* T18: MUL b5*ai */ \
    { MUL_DSZ64_DRR(RCX, TMP15, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* T19: w5_base = hi(b4*ai) + lo(b5*ai) */ \
    { ADD_DSZ64_DRR(TMP7, TMP1, RDX), SETCC_CONDB_DR(TMP9, TMP7), \
      NOP, NOP_SEQWORD }, \
    /* T20: w5 = w5_base + carry(TMP8) */ \
    { ADD_DSZ64_DRR(TMP7, TMP7, TMP8), SETCC_CONDB_DR(TMP1, TMP7), \
      NOP, NOP_SEQWORD }, \
    /* T21: slot-0→1 RAW merge: combine carries + w6 */ \
    { ADD_DSZ64_DRR(TMP8, TMP9, TMP1), ADD_DSZ64_DRR(TMP9, RCX, TMP8), \
      NOP, NOP_SEQWORD }, \
    /* Product: TMP0=acc[0]+w0(carry TMP3), TMP2=w1, TMP4=w2, TMP5=w3, */ \
    /*          TMP6=w4, TMP7=w5, TMP9=w6 */ \
    \
    /* ── PHASE A': accumulate with triple-pack — 13 triads ── */ \
    /* w0 already done in T3. Copy w0 result + start w1 */ \
    { ZEROEXT_DSZ64_DR(R15, TMP0), ADD_DSZ64_DRR(TMP0, R9, TMP2), \
      SETCC_CONDB_DR(TMP1, TMP0), NOP_SEQWORD }, \
    /* w1 +cin (TMP3 = carry from w0) */ \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0), \
      NOP, NOP_SEQWORD }, \
    /* combine w1 + copy w1 + start w2 */ \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R9, TMP0), \
      ADD_DSZ64_DRR(TMP0, R10, TMP4), NOP_SEQWORD }, \
    /* TRIPLE w2: SETCC(prev) + ADD(+cin) + SETCC(new) */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* combine w2 + copy w2 + start w3 */ \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R10, TMP0), \
      ADD_DSZ64_DRR(TMP0, R13, TMP5), NOP_SEQWORD }, \
    /* TRIPLE w3 */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* combine w3 + copy w3 + start w4 */ \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R13, TMP0), \
      ADD_DSZ64_DRR(TMP0, RAX, TMP6), NOP_SEQWORD }, \
    /* TRIPLE w4 */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* combine w4 + copy w4 + start w5 */ \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(RAX, TMP0), \
      ADD_DSZ64_DRR(TMP0, RSI, TMP7), NOP_SEQWORD }, \
    /* TRIPLE w5 */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* combine w5 + copy w5 + start w6 */ \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(RSI, TMP0), \
      ADD_DSZ64_DRR(TMP0, R12, TMP9), NOP_SEQWORD }, \
    /* TRIPLE w6 */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* combine w6 + copy w6 + Phase A'/B merge: setup RDX=R15 for Phase B */ \
    { ADD_DSZ64_DRR(TMP14, TMP1, TMP8), ZEROEXT_DSZ64_DR(R12, TMP0), \
      ZEROEXT_DSZ64_DR(RDX, R15), NOP_SEQWORD }, \
    /* TMP14 = extra carry from Phase A' (acc[6] overflow) */ \
    \
    /* ── PHASE B: m = acc[0]*mu, m*p[0..2], m*R8, chain — 24 triads ── */ \
    /* Compute mu = 0x100000001: slot-0→1 RAW merge SHR+ADD_DRI */ \
    { SHR_DSZ64_DRI(TMP9, R8, 32), ADD_DSZ64_DRI(TMP9, TMP9, 2), \
      NOP, NOP_SEQWORD }, \
    /* MUL: m = acc[0] * mu */ \
    { MUL_DSZ64_DRR(RCX, TMP9, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* save m to TMP9 (RDX = m = lo result, unchanged by ZEROEXT) */ \
    { ZEROEXT_DSZ64_DR(TMP9, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* MUL: m * p[0] (R11 = 0xFFFFFFFF, precomputed in Phase A T0) */ \
    { MUL_DSZ64_DRR(RCX, R11, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* save mp0_lo, mp0_hi, reload m */ \
    { ZEROEXT_DSZ64_DR(TMP0, RDX), ZEROEXT_DSZ64_DR(TMP1, RCX), \
      ZEROEXT_DSZ64_DR(RDX, TMP9), NOP_SEQWORD }, \
    /* MUL: m * p[1] (R14 = 0xFFFFFFFF00000000, precomputed in Phase A T0) */ \
    { MUL_DSZ64_DRR(RCX, R14, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* save mp1_lo, mp1_hi, reload m */ \
    { ZEROEXT_DSZ64_DR(TMP2, RDX), ZEROEXT_DSZ64_DR(TMP3, RCX), \
      ZEROEXT_DSZ64_DR(RDX, TMP9), NOP_SEQWORD }, \
    /* MUL: m * p[2] (RBX = 0xFFFFFFFFFFFFFFFE, precomputed in Phase A T5) */ \
    { MUL_DSZ64_DRR(RCX, RBX, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* save mp2_lo, mp2_hi, reload m */ \
    { ZEROEXT_DSZ64_DR(TMP4, RDX), ZEROEXT_DSZ64_DR(TMP5, RCX), \
      ZEROEXT_DSZ64_DR(RDX, TMP9), NOP_SEQWORD }, \
    /* MUL: m * R8 (= m * p[3] = m * p[4] = m * p[5]) */ \
    { MUL_DSZ64_DRR(RCX, R8, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* save mR8_lo=TMP6, mR8_hi=TMP7 */ \
    { ZEROEXT_DSZ64_DR(TMP6, RDX), ZEROEXT_DSZ64_DR(TMP7, RCX), \
      NOP, NOP_SEQWORD }, \
    \
    /* ── Phase B chain: build red[0..6] ── */ \
    /* red[0] = mp0_lo (TMP0), free */ \
    /* red[1] = mp0_hi + mp1_lo */ \
    { ADD_DSZ64_DRR(TMP1, TMP1, TMP2), SETCC_CONDB_DR(TMP2, TMP1), \
      NOP, NOP_SEQWORD }, \
    /* red[2] = mp1_hi + mp2_lo + carry */ \
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP4), SETCC_CONDB_DR(TMP4, TMP3), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP2), SETCC_CONDB_DR(TMP8, TMP3), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP2, TMP4, TMP8), NOP, NOP, NOP_SEQWORD }, \
    /* red[3] = mp2_hi + mR8_lo + carry */ \
    { ADD_DSZ64_DRR(TMP4, TMP5, TMP6), SETCC_CONDB_DR(TMP5, TMP4), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP2), SETCC_CONDB_DR(TMP8, TMP4), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP2, TMP5, TMP8), NOP, NOP, NOP_SEQWORD }, \
    /* red[4] = mR8_hi + mR8_lo + carry */ \
    { ADD_DSZ64_DRR(TMP5, TMP7, TMP6), SETCC_CONDB_DR(TMP8, TMP5), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP5, TMP5, TMP2), SETCC_CONDB_DR(TMP9, TMP5), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP2, TMP8, TMP9), NOP, NOP, NOP_SEQWORD }, \
    /* red[5] = mR8_hi + mR8_lo + carry */ \
    { ADD_DSZ64_DRR(TMP8, TMP7, TMP6), SETCC_CONDB_DR(TMP9, TMP8), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP8, TMP8, TMP2), SETCC_CONDB_DR(TMP6, TMP8), \
      NOP, NOP_SEQWORD }, \
    /* merge: red[5] combine + red[6] (slot-0→1 RAW) + red[6]_hi (slot-1→2 SETCC) */ \
    { ADD_DSZ64_DRR(TMP2, TMP9, TMP6), ADD_DSZ64_DRR(TMP9, TMP7, TMP2), \
      SETCC_CONDB_DR(TMP6, TMP9), NOP_SEQWORD }, \
    /* After chain: TMP0=red[0], TMP1=red[1], TMP3=red[2], TMP4=red[3], */ \
    /*              TMP5=red[4], TMP8=red[5], TMP9=red[6], TMP6=red[6]_hi */ \
    \
    /* ── PHASE C: acc += red, shift — 15 triads ── */ \
    /* word 0 (discard) + carry→TMP7, save red[5](TMP8)→RDI before clobber */ \
    { ADD_DSZ64_DRR(TMP2, R15, TMP0), SETCC_CONDB_DR(TMP7, TMP2), \
      ZEROEXT_DSZ64_DR(RDI, TMP8), NOP_SEQWORD }, \
    /* word 1 start: acc[1] + red[1] */ \
    { ADD_DSZ64_DRR(TMP0, R9, TMP1), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP, NOP_SEQWORD }, \
    /* word 1 +cin */ \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP7), SETCC_CONDB_DR(TMP8, TMP0), \
      NOP, NOP_SEQWORD }, \
    /* combine w1 + copy w1 + start w2 */ \
    { ADD_DSZ64_DRR(TMP7, TMP1, TMP8), ZEROEXT_DSZ64_DR(R15, TMP0), \
      ADD_DSZ64_DRR(TMP0, R10, TMP3), NOP_SEQWORD }, \
    /* TRIPLE w2 */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP7), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* combine w2 + copy w2 + start w3 */ \
    { ADD_DSZ64_DRR(TMP7, TMP1, TMP8), ZEROEXT_DSZ64_DR(R9, TMP0), \
      ADD_DSZ64_DRR(TMP0, R13, TMP4), NOP_SEQWORD }, \
    /* TRIPLE w3 */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP7), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* combine w3 + copy w3 + start w4 */ \
    { ADD_DSZ64_DRR(TMP7, TMP1, TMP8), ZEROEXT_DSZ64_DR(R10, TMP0), \
      ADD_DSZ64_DRR(TMP0, RAX, TMP5), NOP_SEQWORD }, \
    /* TRIPLE w4 */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP7), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* combine w4 + copy w4 + start w5 (red[5] saved in RDI) */ \
    { ADD_DSZ64_DRR(TMP7, TMP1, TMP8), ZEROEXT_DSZ64_DR(R13, TMP0), \
      ADD_DSZ64_DRR(TMP0, RSI, RDI), NOP_SEQWORD }, \
    /* TRIPLE w5 */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP7), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* combine w5 + copy w5 + start w6 */ \
    { ADD_DSZ64_DRR(TMP7, TMP1, TMP8), ZEROEXT_DSZ64_DR(RAX, TMP0), \
      ADD_DSZ64_DRR(TMP0, R12, TMP9), NOP_SEQWORD }, \
    /* TRIPLE w6 */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP7), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* copy w6 + combine w6 carries */ \
    { ZEROEXT_DSZ64_DR(RSI, TMP0), ADD_DSZ64_DRR(TMP0, TMP1, TMP8), \
      NOP, NOP_SEQWORD }, \
    /* acc[6] = carry + TMP14 + red[6]_hi: slot-0→1 RAW merge */ \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP14), ADD_DSZ64_DRR(R12, TMP0, TMP6), \
      NOP, NOP_SEQWORD }
    /* After: R15=acc[0], R9=acc[1], R10=acc[2], R13=acc[3], RAX=acc[4], */
    /*        RSI=acc[5], R12=acc[6] */

static void install_p384_sq_patch(void) {
    ucode_t patch[] = {
    /* PREP: copy b[0..5] from arch to TMP10-15, then load acc[5..6] */
    { ZEROEXT_DSZ64_DR(TMP10, RSI), ZEROEXT_DSZ64_DR(TMP11, R12),
      ZEROEXT_DSZ64_DR(TMP12, R11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R14), ZEROEXT_DSZ64_DR(TMP14, RBX),
      ZEROEXT_DSZ64_DR(TMP15, RBP), NOP_SEQWORD },
    /* Now b regs (RSI,R12,R11,R14,RBX,RBP) are free.
     * Load acc[5] from RDX, acc[6] from RCX (vmwrite operands) */
    { ZEROEXT_DSZ64_DR(RSI, RDX), ZEROEXT_DSZ64_DR(R12, RCX),
      NOP, NOP_SEQWORD },
    MONT_ITER,
    { NOP, NOP, NOP, END_SEQWORD }
    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("p384_sq: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ── fe_sq via microcode ─────────────────────────────────────── */

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    uint64_t acc[7];
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_acc asm("r15") = acc;

    asm volatile(
        "push r15\n\t"
        "push rcx\n\t"
        "push rbp\n\t"

        /* b = a for squaring → arch regs */
        "mov rsi, [rcx]\n\t"        /* b[0] = a[0] */
        "mov r12, [rcx + 8]\n\t"    /* b[1] = a[1] */
        "mov r11, [rcx + 16]\n\t"   /* b[2] = a[2] */
        "mov r14, [rcx + 24]\n\t"   /* b[3] = a[3] */
        "mov rbx, [rcx + 32]\n\t"   /* b[4] = a[4] */
        "mov rbp, [rcx + 40]\n\t"   /* b[5] = a[5] */

        /* p constant */
        "mov r8, -1\n\t"            /* p[3]=p[4]=p[5] = all ones */

        /* Zero accumulator: acc[0..4] in regs, acc[5..6] via RDX/RCX */
        "xor r15d, r15d\n\t"        /* acc[0] */
        "xor r9d, r9d\n\t"          /* acc[1] */
        "xor r10d, r10d\n\t"        /* acc[2] */
        "xor r13d, r13d\n\t"        /* acc[3] */
        "xor eax, eax\n\t"          /* acc[4] */

        /* ── Iteration 0 ── */
        "mov rcx, [rsp + 8]\n\t"    /* a pointer (above rbp push) */
        "mov rdi, [rcx]\n\t"        /* a[0] */
        "xor edx, edx\n\t"          /* acc[5] = 0 */
        "xor ecx, ecx\n\t"          /* acc[6] = 0 */
        "vmwrite rcx, rdx\n\t"

        /* After vmwrite: RSI=acc[5], R12=acc[6]. Save and reload b. */
        "push rsi\n\t"              /* save acc[5] */
        "push r12\n\t"              /* save acc[6] */
        "mov rcx, [rsp + 24]\n\t"   /* a pointer (rsp+24: r12,rsi,rbp,rcx) */
        "mov rsi, [rcx]\n\t"
        "mov r12, [rcx + 8]\n\t"
        "mov r11, [rcx + 16]\n\t"
        "mov r14, [rcx + 24]\n\t"
        "mov rbx, [rcx + 32]\n\t"
        "mov rbp, [rcx + 40]\n\t"

        /* ── Iteration 1 ── */
        "mov rdi, [rcx + 8]\n\t"    /* a[1] */
        "pop rcx\n\t"               /* acc[6] → RCX */
        "pop rdx\n\t"               /* acc[5] → RDX */
        "vmwrite rcx, rdx\n\t"

        "push rsi\n\t"
        "push r12\n\t"
        "mov rcx, [rsp + 24]\n\t"
        "mov rsi, [rcx]\n\t"
        "mov r12, [rcx + 8]\n\t"
        "mov r11, [rcx + 16]\n\t"
        "mov r14, [rcx + 24]\n\t"
        "mov rbx, [rcx + 32]\n\t"
        "mov rbp, [rcx + 40]\n\t"

        /* ── Iteration 2 ── */
        "mov rdi, [rcx + 16]\n\t"   /* a[2] */
        "pop rcx\n\t"
        "pop rdx\n\t"
        "vmwrite rcx, rdx\n\t"

        "push rsi\n\t"
        "push r12\n\t"
        "mov rcx, [rsp + 24]\n\t"
        "mov rsi, [rcx]\n\t"
        "mov r12, [rcx + 8]\n\t"
        "mov r11, [rcx + 16]\n\t"
        "mov r14, [rcx + 24]\n\t"
        "mov rbx, [rcx + 32]\n\t"
        "mov rbp, [rcx + 40]\n\t"

        /* ── Iteration 3 ── */
        "mov rdi, [rcx + 24]\n\t"   /* a[3] */
        "pop rcx\n\t"
        "pop rdx\n\t"
        "vmwrite rcx, rdx\n\t"

        "push rsi\n\t"
        "push r12\n\t"
        "mov rcx, [rsp + 24]\n\t"
        "mov rsi, [rcx]\n\t"
        "mov r12, [rcx + 8]\n\t"
        "mov r11, [rcx + 16]\n\t"
        "mov r14, [rcx + 24]\n\t"
        "mov rbx, [rcx + 32]\n\t"
        "mov rbp, [rcx + 40]\n\t"

        /* ── Iteration 4 ── */
        "mov rdi, [rcx + 32]\n\t"   /* a[4] */
        "pop rcx\n\t"
        "pop rdx\n\t"
        "vmwrite rcx, rdx\n\t"

        "push rsi\n\t"
        "push r12\n\t"
        "mov rcx, [rsp + 24]\n\t"
        "mov rsi, [rcx]\n\t"
        "mov r12, [rcx + 8]\n\t"
        "mov r11, [rcx + 16]\n\t"
        "mov r14, [rcx + 24]\n\t"
        "mov rbx, [rcx + 32]\n\t"
        "mov rbp, [rcx + 40]\n\t"

        /* ── Iteration 5 ── */
        "mov rdi, [rcx + 40]\n\t"   /* a[5] */
        "pop rcx\n\t"
        "pop rdx\n\t"
        "vmwrite rcx, rdx\n\t"

        /* Final: RSI=acc[5], R12=acc[6], R15/R9/R10/R13/RAX = acc[0..4] */
        /* Stack: [rbp_save][a_ptr][r15_save(=acc_ptr)]
         * pop rbp → restore rbp
         * pop rcx → discard a_ptr
         * pop rcx → rcx = saved r15 = acc pointer */
        "pop rbp\n\t"
        "pop rcx\n\t"
        "pop rcx\n\t"

        "mov [rcx],      r15\n\t"   /* acc[0] */
        "mov [rcx + 8],  r9\n\t"    /* acc[1] */
        "mov [rcx + 16], r10\n\t"   /* acc[2] */
        "mov [rcx + 24], r13\n\t"   /* acc[3] */
        "mov [rcx + 32], rax\n\t"   /* acc[4] */
        "mov [rcx + 40], rsi\n\t"   /* acc[5] */
        "mov [rcx + 48], r12\n\t"   /* acc[6] */

        : "+r"(_a), "+r"(_acc)
        :
        : "rax", "rbx", "rbp", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14",
          "memory", "cc"
    );

    cond_subtract(acc, out);
}

/* ── verification ────────────────────────────────────────────── */

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void rand_mod_p(uint64_t out[6], uint64_t *rng) {
    for (;;) {
        for (int j = 0; j < 6; j++) out[j] = splitmix64(rng);
        int lt = 0;
        for (int j = 5; j >= 0; j--) {
            if (out[j] < P384_P[j]) { lt = 1; break; }
            if (out[j] > P384_P[j]) break;
        }
        if (lt) break;
    }
}

static int verify_all(void) {
    int pass = 0, fail = 0;

    printf("--- Known vectors ---\n");
    /*
     * Montgomery form of 1 for P-384: R mod p = 2^384 mod p.
     * R - p = 2^128 + 2^96 - 2^32 + 1
     * limb0: (-2^32 + 1) mod 2^64 = 0xFFFFFFFF00000001, borrow 1
     * limb1: 2^32 - 1 (borrow) = 0xFFFFFFFF
     * limb2: 1
     * R mod p = {0xFFFFFFFF00000001, 0xFFFFFFFF, 1, 0, 0, 0}
     */
    struct { const char *name; uint64_t a[6]; } vecs[] = {
        { "0^2", {0,0,0,0,0,0} },
        { "1_mont^2", {UINT64_C(0xFFFFFFFF00000001), UINT64_C(0xFFFFFFFF),
                       1, 0, 0, 0} },
        { "small^2", {3, 0, 0, 0, 0, 0} },
    };
    for (int i = 0; i < 3; i++) {
        uint64_t ref[6], nat[6], ucd[6];
        fe_sq_reference(vecs[i].a, ref);
        fe_sq_native(vecs[i].a, nat);
        fe_sq_ucode(vecs[i].a, ucd);
        int ok = !memcmp(ref, nat, 48) && !memcmp(ref, ucd, 48);
        if (!ok) {
            printf("  FAIL [%s]\n", vecs[i].name);
            for (int j=0;j<6;j++)
                printf("    [%d] ref=%016"PRIx64" nat=%016"PRIx64" ucd=%016"PRIx64"%s%s\n",
                    j, ref[j], nat[j], ucd[j],
                    ref[j]!=nat[j]?" nat***":"", ref[j]!=ucd[j]?" ucd***":"");
        } else printf("  PASS [%s]\n", vecs[i].name);
        if (ok) pass++; else fail++;
    }

    printf("\n--- Random (10000) ---\n");
    uint64_t rng = 0xA384CAFE12345678ULL;
    int rp = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t a[6], ref[6], nat[6], ucd[6];
        rand_mod_p(a, &rng);
        fe_sq_reference(a, ref); fe_sq_native(a, nat); fe_sq_ucode(a, ucd);
        if (!memcmp(ref, nat, 48) && !memcmp(ref, ucd, 48)) rp++;
        else {
            printf("  FAIL #%d  a={%016lx,%016lx,%016lx,%016lx,%016lx,%016lx}\n",
                   i, a[0], a[1], a[2], a[3], a[4], a[5]);
            for (int j=0;j<6;j++)
                printf("    [%d] ref=%016lx nat=%016lx ucd=%016lx%s%s\n",
                    j, ref[j], nat[j], ucd[j],
                    ref[j]!=nat[j]?" nat***":"", ref[j]!=ucd[j]?" ucd***":"");
            break;
        }
    }
    printf("  %d / 10000 PASS\n", rp);
    pass += rp; if (rp < 10000) fail += (10000 - rp);

    printf("\n--- Chain (1000 iterated sq) ---\n");
    uint64_t ri[6]={UINT64_C(0xFFFFFFFF00000001), UINT64_C(0xFFFFFFFF), 1, 0, 0, 0};
    uint64_t ni[6], ui[6];
    memcpy(ni, ri, 48); memcpy(ui, ri, 48);
    for (int i = 0; i < 1000; i++) {
        fe_sq_reference(ri, ri); fe_sq_native(ni, ni); fe_sq_ucode(ui, ui);
    }
    int rok = !memcmp(ri,ni,48) && !memcmp(ri,ui,48);
    printf("  %s\n", rok?"PASS":"FAIL");
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
    printf("=== P-384 Montgomery squaring: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_p384_sq_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED, skipping benchmark.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    uint64_t state[6] = {UINT64_C(0xFFFFFFFF00000001), UINT64_C(0xFFFFFFFF),
                         1, 0, 0, 0};
    uint64_t tmp[6], t0, t1, min, sum;

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, 48);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_native(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Naive -O3:   min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n", min/BATCH, sum/REPS/BATCH);

    /* fiat-crypto (the real GCC baseline CryptOpt compares against) */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, 48);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_fiat(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Fiat-crypto: min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n", min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, 48);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_ucode(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode:   min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n", min/BATCH, sum/REPS/BATCH);

    init_match_and_patch(); do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
