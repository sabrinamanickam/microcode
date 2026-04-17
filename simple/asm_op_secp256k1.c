/*
 * asm_op_secp256k1.c — fe_sq for secp256k1 Dettman (2^256 - 4294968273) via microcode
 *
 * Field: GF(2^256 - 0x1000003d1)
 * Dettman representation: 5 limbs, widths 52/52/52/52/48
 *   eval = z[0] + z[1]*2^52 + z[2]*2^104 + z[3]*2^156 + z[4]*2^208
 *
 * Reduction constants:
 *   R1     = 0x1000003d1      (33 bits)
 *   R10    = 0x1000003d10     (37 bits) = R1 << 4
 *   R10000 = 0x1000003d10000  (49 bits) = R1 << 16
 *
 * Computation order (from Fiat/Dettman):
 *   B: d0*a3 + d1*a2 + lo(a4²)*R10          → carry@52 → c3_saved
 *   C: carry + d0*a4 + d1*a3 + a2² + hi(a4²)*R10000 → carry@52 → split 48/4
 *   D: carry + d1*a4 + d2*a3                 → carry@52 → x16
 *   E: a0² + (x17 + x16<<4)*R1              → carry@52 → out[0]
 *   F: x15 + d2*a4 + a3²                    → carry@52 → x24
 *   G: x20 + d0*a1 + x24*R10                → carry@52 → out[1]
 *   H: x23 + d3*a4                          → 64-bit split → x29, x30
 *   I: x26 + d0*a2 + a1² + x30*R10         → carry@52 → out[2]
 *   J: carry + c3_saved + x29*R10000        → carry@52 → out[3]
 *   K: carry + c4_out                        → out[4]
 *
 * Register convention (caller → microcode):
 *   RDI=a0  RSI=a1  R12=a2  R11=a3  R14=a4
 *   R15=2*a0  R13=2*a1
 *   R9=lo(a4²)  R10=hi(a4²)    [computed in native before vmwrite]
 *   RBX=0x1000003d1
 *   RAX=0  R8=0  RCX=free  RDX=free(scratch/srcB copy)
 *
 * TMP register plan:
 *   TMP0/TMP2 = acc_lo (alternating)     TMP3 = SETCC carry
 *   TMP4-TMP7 = hi product saves         TMP8 = lo carry, TMP9 = shift scratch
 *   TMP1 = carry alignment scratch
 *   TMP10 = R10_const (persists)          TMP11 = R10000_const (persists)
 *   TMP12 = x17 (c4 overflow 4 bits)     TMP13 = x15 (carry D→F)
 *   TMP14 = d3=2*a3 (for block H)        TMP15 = carry scratch
 *
 * Output: RDI=h0  R9=h1  R10 or RBX=h2  R11=h3  RAX=h4
 *         (exact mapping TBD based on register availability at output time)
 *
 * Build:  make PROG=asm_op_secp256k1
 * Run:    sudo taskset -c 0 ./asm_op_secp256k1_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MASK52 0xFFFFFFFFFFFFFULL
#define MASK48 0xFFFFFFFFFFFFULL

/* ── fe_sq native C (matches Fiat secp256k1_dettman_square) ──── */

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    uint64_t a0=a[0], a1=a[1], a2=a[2], a3=a[3], a4=a[4];
    uint64_t d0=2*a0, d1=2*a1, d2=2*a2, d3=2*a3;

    __uint128_t t = (__uint128_t)a4 * a4;
    uint64_t a4hi = (uint64_t)(t >> 64);
    uint64_t a4lo = (uint64_t)t;

    /* Block B: c3_raw */
    t = (__uint128_t)d0*a3 + (__uint128_t)d1*a2
      + (__uint128_t)a4lo * 0x1000003d10ULL;
    uint64_t carry = (uint64_t)(t >> 52);
    uint64_t c3_saved = (uint64_t)t & MASK52;

    /* Block C: c4_raw → split */
    t = carry
      + (__uint128_t)d0*a4 + (__uint128_t)d1*a3 + (__uint128_t)a2*a2
      + (__uint128_t)a4hi * 0x1000003d10000ULL;
    carry = (uint64_t)(t >> 52);
    uint64_t c4_raw = (uint64_t)t & MASK52;
    uint64_t x17 = c4_raw >> 48;
    uint64_t c4_out = c4_raw & MASK48;

    /* Block D */
    t = carry + (__uint128_t)d1*a4 + (__uint128_t)d2*a3;
    uint64_t x15 = (uint64_t)(t >> 52);
    uint64_t x16 = (uint64_t)t & MASK52;

    /* Block E: out[0] */
    t = (__uint128_t)a0*a0
      + (__uint128_t)(x17 + (x16 << 4)) * 0x1000003d1ULL;
    uint64_t x20 = (uint64_t)(t >> 52);
    out[0] = (uint64_t)t & MASK52;

    /* Block F */
    t = x15 + (__uint128_t)d2*a4 + (__uint128_t)a3*a3;
    uint64_t x23 = (uint64_t)(t >> 52);
    uint64_t x24 = (uint64_t)t & MASK52;

    /* Block G: out[1] */
    t = x20 + (__uint128_t)d0*a1
      + (__uint128_t)x24 * 0x1000003d10ULL;
    uint64_t x26 = (uint64_t)(t >> 52);
    out[1] = (uint64_t)t & MASK52;

    /* Block H: 64-bit split */
    t = x23 + (__uint128_t)d3*a4;
    uint64_t x29 = (uint64_t)(t >> 64);
    uint64_t x30 = (uint64_t)t;

    /* Block I: out[2] */
    t = x26 + (__uint128_t)d0*a2 + (__uint128_t)a1*a1
      + (__uint128_t)x30 * 0x1000003d10ULL;
    carry = (uint64_t)(t >> 52);
    out[2] = (uint64_t)t & MASK52;

    /* Block J: out[3] */
    t = carry + c3_saved
      + (__uint128_t)x29 * 0x1000003d10000ULL;
    uint64_t cj = (uint64_t)(t >> 52);
    out[3] = (uint64_t)t & MASK52;

    /* Block K: out[4] */
    out[4] = cj + c4_out;
}

/* ── independent reference (big-integer square mod p) ────────── */

static void fe_sq_reference(const uint64_t *a, uint64_t *out) {
    /*
     * 1. Convert Dettman limbs → flat 4×64 integer (256 bits)
     * 2. Schoolbook square → 8×64 (512 bits)
     * 3. Reduce mod p = 2^256 - 0x1000003d1
     * 4. Convert back to Dettman limbs
     */

    /* 1. Flat: val = a0 + a1*2^52 + a2*2^104 + a3*2^156 + a4*2^208 */
    __uint128_t acc = 0;
    uint64_t v[5] = {0};
    acc = (__uint128_t)a[0];
    acc += (__uint128_t)a[1] << 52;
    v[0] = (uint64_t)acc; acc >>= 64;
    acc += (__uint128_t)a[2] << (104 - 64);   /* 40 */
    v[1] = (uint64_t)acc; acc >>= 64;
    acc += (__uint128_t)a[3] << (156 - 128);   /* 28 */
    v[2] = (uint64_t)acc; acc >>= 64;
    acc += (__uint128_t)a[4] << (208 - 192);   /* 16 */
    v[3] = (uint64_t)acc;
    /* v[4] unused: val < 2^256 */

    /* 2. Schoolbook square v[0..3] → r[0..7] */
    __uint128_t rr[8] = {0};
    for (int i = 0; i < 4; i++) {
        __uint128_t carry = 0;
        for (int j = 0; j < 4; j++) {
            __uint128_t prod = (__uint128_t)v[i] * v[j]
                             + (uint64_t)rr[i+j] + carry;
            rr[i+j] = (uint64_t)prod;
            carry = prod >> 64;
        }
        rr[i+4] += carry;
    }
    uint64_t r[8];
    for (int k = 0; k < 8; k++) r[k] = (uint64_t)rr[k];

    /* 3. Reduce mod p = 2^256 - c where c = 0x1000003d1 */
    /* r = hi * 2^256 + lo → lo + c*hi (mod p). Two passes. */
    const uint64_t C = 0x1000003d1ULL;
    for (int pass = 0; pass < 3; pass++) {
        uint64_t lo[4] = {r[0], r[1], r[2], r[3]};
        uint64_t hi[4] = {r[4], r[5], r[6], r[7]};
        __uint128_t s = 0;
        for (int k = 0; k < 4; k++) {
            s += (__uint128_t)lo[k] + (__uint128_t)hi[k] * C;
            r[k] = (uint64_t)s;
            s >>= 64;
        }
        r[4] = (uint64_t)s;
        r[5] = r[6] = r[7] = 0;
    }

    /* 4. Flat → Dettman limbs (52/52/52/52/48) */
    acc = (__uint128_t)r[0] | ((__uint128_t)r[1] << 64);
    out[0] = (uint64_t)acc & MASK52;  acc >>= 52;
    out[1] = (uint64_t)acc & MASK52;  acc >>= 52;
    acc |= (__uint128_t)r[2] << (128 - 104);  /* 24 */
    out[2] = (uint64_t)acc & MASK52;  acc >>= 52;
    acc |= (__uint128_t)r[3] << (192 - 156);  /* 36 */
    out[3] = (uint64_t)acc & MASK52;  acc >>= 52;
    out[4] = (uint64_t)acc & MASK48;

    /* Final carry wrap: if carry overflows limb 4, reduce */
    uint64_t top = (uint64_t)(acc >> 48);
    if (top) {
        __uint128_t s = (__uint128_t)out[0] + (__uint128_t)top * C;
        out[0] = (uint64_t)s & MASK52;
        uint64_t c = (uint64_t)(s >> 52);
        out[1] += c;
    }
}

/* ── microcode patch ──────────────────────────────────────────── */

static void install_secp256k1_sq_patch(void) {
    ucode_t patch[] = {

    /*
     * Register state at entry:
     *   RDI=a0  RSI=a1  R12=a2  R11=a3  R14=a4
     *   R15=2*a0  R13=2*a1
     *   R9=lo(a4²)  R10=hi(a4²)
     *   RBX=0x1000003d1  RAX=0  R8=0
     *   RCX=free  RDX=free
     *
     * TMP10=R10c  TMP11=R10000c  TMP12=x17  TMP13=x15
     * TMP14=d3    TMP15=scratch
     */

    /* ═══ PREP: compute reduction constants ═══ */
    /* T0 */ { SHL_DSZ64_DRI(TMP10, RBX, 4),    /* TMP10 = R10_const = R1<<4 */
               SHL_DSZ64_DRI(TMP11, RBX, 16),   /* TMP11 = R10000_const = R1<<16 */
               NOP, NOP_SEQWORD },

    /* ═══ BLOCK B: d0*a3 + d1*a2 + a4sq_lo*R10  [3 MAC, carry@52] ═══ */
    /* Products: R15*a3, R13*a2, TMP10*R9 */
    /* Save a3→RDX for first MUL srcB (a3 needed later) */

    /* T1: init acc, start MUL d0*a3 (copy a3 to RDX first) */
    /* T1 */ { ZEROEXT_DSZ64_DR(TMP0, RAX),       /* TMP0 = 0 (acc init) */
               ZEROEXT_DSZ64_DR(RDX, R11),         /* RDX = a3 copy */
               NOP, NOP_SEQWORD },
    /* T2 */ { MUL_DSZ64_DRR(RCX, R15, RDX),      /* d0*a3: RCX=hi, RDX=lo */
               NOP, NOP, NOP_SEQWORD },
    /* T3 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),    /* acc = 0 + lo(d0*a3) */
               SETCC_CONDB_DR(TMP3, TMP2),
               ZEROEXT_DSZ64_DR(RDX, R12),         /* copy a2 → RDX for next MUL */
               NOP_SEQWORD },

    /* T4: save hi[0], MUL d1*a2 */
    /* T4 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R13, RDX),      /* d1*a2: R13 preserved, RDX=lo */
               NOP, NOP_SEQWORD },
    /* T5 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               NOP, NOP_SEQWORD },

    /* T6: save hi[1], MUL a4sq_lo * R10_const */
    /* T6 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, TMP10, R9),     /* R10c * a4sq_lo: TMP10 preserved, R9=lo */
               NOP, NOP_SEQWORD },
    /* T7 */ { ADD_DSZ64_DRR(TMP2, TMP0, R9),
               SETCC_CONDB_DR(TMP3, TMP2),
               NOP, NOP_SEQWORD },

    /* Carry extraction @52 (S=12), output c3_saved → R9 */
    /* T8 */ { SHR_DSZ64_DRI(TMP8, TMP2, 52),
               ADD_DSZ64_DRR(TMP6, RCX, TMP3),
               NOP, NOP_SEQWORD },
    /* T9 */ { SHL_DSZ64_DRI(TMP9, TMP2, 12),
               ADD_DSZ64_DRR(R8, R8, TMP4),
               ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
               NOP_SEQWORD },
    /* T10 */ { SHR_DSZ64_DRI(R9, TMP9, 12),      /* R9 = c3_saved (x10) */
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD },

    /* ═══ B→C TRANSITION [S=12] ═══ */
    /* Need: carry from B, start d0*a4 for C, copy a4→RDX */
    /* T11 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                ZEROEXT_DSZ64_DR(RDX, R14),        /* copy a4 → RDX */
                NOP_SEQWORD },
    /* T12 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),   /* TMP0 = carry from B */
                MUL_DSZ64_DRR(RCX, R15, RDX),     /* d0*a4: R15 preserved, RDX=lo */
                NOP, NOP_SEQWORD },

    /* ═══ BLOCK C: carry + d0*a4 + d1*a3 + a2² + a4sq_hi*R10000  [4 MAC, carry@52 + 48-split] ═══ */

    /* T13: acc = carry + lo(d0*a4) */
    /* T13 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                ZEROEXT_DSZ64_DR(RDX, R11),        /* copy a3 → RDX */
                NOP_SEQWORD },

    /* T14: hi[0], MUL d1*a3 */
    /* T14 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R13, RDX),     /* d1*a3 */
                NOP, NOP_SEQWORD },
    /* T15 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                SETCC_CONDB_DR(TMP3, TMP0),
                ZEROEXT_DSZ64_DR(RDX, R12),        /* copy a2 → RDX (for a2²) */
                NOP_SEQWORD },

    /* T16: hi[1], MUL a2*a2 */
    /* T16 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R12, RDX),     /* a2*a2: R12 preserved, RDX=lo */
                NOP, NOP_SEQWORD },
    /* T17 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                NOP, NOP_SEQWORD },

    /* T18: hi[2], MUL a4sq_hi * R10000 (last use of R10) */
    /* T18 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, TMP11, R10),   /* R10000c * a4sq_hi: TMP11 preserved, R10=lo */
                NOP, NOP_SEQWORD },
    /* T19 */ { ADD_DSZ64_DRR(TMP0, TMP2, R10),
                SETCC_CONDB_DR(TMP3, TMP0),
                NOP, NOP_SEQWORD },

    /* Carry extraction @52, output → RDX (temp) */
    /* T20 */ { SHR_DSZ64_DRI(TMP8, TMP0, 52),
                ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                NOP, NOP_SEQWORD },
    /* T21 */ { SHL_DSZ64_DRI(TMP9, TMP0, 12),
                ADD_DSZ64_DRR(R8, R8, TMP4),
                ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
                NOP_SEQWORD },
    /* T22 */ { ADD_DSZ64_DRR(TMP0, TMP0, TMP7),
                NOP, NOP, NOP_SEQWORD },
    /* T23 */ { SHR_DSZ64_DRI(RDX, TMP9, 12),     /* RDX = c4_raw (x13, 52 bits) */
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD },

    /* Split c4_raw into x17 (top 4 bits) and c4_out (bottom 48) */
    /* T24 */ { SHR_DSZ64_DRI(TMP12, RDX, 48),    /* TMP12 = x17 */
                SHL_DSZ64_DRI(TMP9, RDX, 16),
                NOP, NOP_SEQWORD },
    /* T25 */ { SHR_DSZ64_DRI(R10, TMP9, 16),     /* R10 = c4_out (x18) — persists until K */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ C→D TRANSITION [S=12] ═══ */
    /* T26 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                ZEROEXT_DSZ64_DR(RDX, R14),        /* copy a4 → RDX */
                NOP_SEQWORD },
    /* T27 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                MUL_DSZ64_DRR(RCX, R13, RDX),     /* d1*a4: R13 preserved, RDX=lo */
                NOP, NOP_SEQWORD },

    /* ═══ BLOCK D: carry + d1*a4 + d2*a3  [2 MAC, carry@52] ═══ */
    /* After D: R13 (d1=2*a1) no longer needed — can reuse */

    /* T28 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                ZEROEXT_DSZ64_DR(RDX, R11),        /* copy a3 → RDX */
                NOP_SEQWORD },

    /* T29: hi[0], compute d2=2*a2→R13, MUL d2*a3 */
    /* T29 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                ADD_DSZ64_DRR(R13, R12, R12),      /* R13 = d2 = 2*a2 (reuse R13) */
                NOP, NOP_SEQWORD },
    /* T30 */ { MUL_DSZ64_DRR(RCX, R13, RDX),     /* d2*a3: R13(d2) preserved, RDX=lo */
                NOP, NOP, NOP_SEQWORD },
    /* T31 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                SETCC_CONDB_DR(TMP3, TMP0),
                NOP, NOP_SEQWORD },

    /* Carry extraction @52, output x16 → RDX */
    /* T32 */ { SHR_DSZ64_DRI(TMP8, TMP0, 52),
                ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                NOP, NOP_SEQWORD },
    /* T33 */ { SHL_DSZ64_DRI(TMP9, TMP0, 12),
                ADD_DSZ64_DRR(TMP0, TMP4, TMP5),
                NOP, NOP_SEQWORD },
    /* T34 */ { SHR_DSZ64_DRI(RDX, TMP9, 12),     /* RDX = x16 */
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD },

    /* ═══ D→E: save x15, compute combined, prep d3 ═══ */
    /* Save D's carry → TMP13 (x15 for block F) */
    /* T35 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                ADD_DSZ64_DRR(TMP14, R11, R11),   /* TMP14 = d3 = 2*a3 (before a3 destroyed) */
                NOP_SEQWORD },
    /* T36 */ { OR_DSZ64_DRR(TMP13, TMP8, TMP1),  /* TMP13 = x15 */
                NOP, NOP, NOP_SEQWORD },

    /* Compute combined = x17 + (x16 << 4) */
    /* T37 */ { SHL_DSZ64_DRI(TMP0, RDX, 4),      /* TMP0 = x16 << 4 */
                NOP, NOP, NOP_SEQWORD },
    /* T38 */ { ADD_DSZ64_DRR(TMP0, TMP0, TMP12), /* TMP0 = combined = (x16<<4) + x17 */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ BLOCK E: a0² + combined*R1  [2 MAC, fresh start, carry@52] → out[0] ═══ */
    /* T39: start a0² */
    /* T39 */ { ZEROEXT_DSZ64_DR(RDX, RDI),        /* copy a0 → RDX */
                NOP, NOP, NOP_SEQWORD },
    /* T40 */ { MUL_DSZ64_DRR(RCX, RDI, RDX),     /* a0²: RDI preserved, RDX=lo */
                ZEROEXT_DSZ64_DR(TMP2, RAX),       /* reset acc = 0... wait, RAX might not be 0 anymore */
                NOP, NOP_SEQWORD },

    /* Hmm, RAX was 0 at entry but hasn't been written. Actually checking: RAX is in the
     * clobber list but not explicitly written by any microcode op. ZEROEXT(TMP2, RAX) should
     * still give 0 if RAX was not written. But to be safe, use NOTAND to zero TMP2. */
    /* Actually, RAX = 0 and is never modified in microcode (MUL writes RCX, not RAX).
     * MUL_DSZ64_DRR(hi, srcA, srcB): hi=RCX, srcA is read-only, srcB gets lo product.
     * RAX is not involved. So RAX is still 0 here. Let me restructure: */

    /* T39 (revised): init acc=0, copy a0, start MUL */
    /* T39 */ { ZEROEXT_DSZ64_DR(TMP15, RAX),      /* TMP15 = 0 (acc init for E) */
                ZEROEXT_DSZ64_DR(RDX, RDI),
                NOP, NOP_SEQWORD },
    /* T40 */ { MUL_DSZ64_DRR(RCX, RDI, RDX),     /* a0²: RDI pres, RDX=lo */
                NOP, NOP, NOP_SEQWORD },
    /* T41 */ { ADD_DSZ64_DRR(TMP2, TMP15, RDX),  /* acc = 0 + lo(a0²) */
                SETCC_CONDB_DR(TMP3, TMP2),
                NOP, NOP_SEQWORD },

    /* T42: hi[0], MUL combined*R1 (TMP0=combined from T38) */
    /* T42 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, RBX, TMP0),    /* R1 * combined: RBX preserved, TMP0=lo */
                NOP, NOP_SEQWORD },
    /* T43 */ { ADD_DSZ64_DRR(TMP0, TMP2, TMP0),
                SETCC_CONDB_DR(TMP3, TMP0),
                NOP, NOP_SEQWORD },

    /* Carry extraction @52, output out[0] → RDI */
    /* T44 */ { SHR_DSZ64_DRI(TMP8, TMP0, 52),
                ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                NOP, NOP_SEQWORD },
    /* T45 */ { SHL_DSZ64_DRI(TMP9, TMP0, 12),
                ADD_DSZ64_DRR(TMP0, TMP4, TMP5),
                NOP, NOP_SEQWORD },
    /* T46 */ { SHR_DSZ64_DRI(RDI, TMP9, 12),     /* RDI = out[0] (x21) */
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD },

    /* Save E's carry → RBX (RBX=R1 no longer needed after T42) */
    /* T47 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                NOP, NOP_SEQWORD },
    /* T48 */ { OR_DSZ64_DRR(RBX, TMP8, TMP1),    /* RBX = x20 (carry E→G) */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ BLOCK F: x15 + d2*a4 + a3²  [2 MAC, start from x15, carry@52] ═══ */
    /* d2 is in R13 (from T29). a4 in R14. a3 in R11. */

    /* T49: set acc = x15 (from TMP13), copy a4→RDX, start MUL d2*a4 */
    /* T49 */ { ZEROEXT_DSZ64_DR(TMP0, TMP13),     /* TMP0 = x15 */
                ZEROEXT_DSZ64_DR(RDX, R14),        /* copy a4 → RDX */
                NOP, NOP_SEQWORD },
    /* T50 */ { MUL_DSZ64_DRR(RCX, R13, RDX),     /* d2*a4: R13(d2) preserved, RDX=lo */
                NOP, NOP, NOP_SEQWORD },
    /* T51 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),   /* acc = x15 + lo(d2*a4) */
                SETCC_CONDB_DR(TMP3, TMP2),
                ZEROEXT_DSZ64_DR(RDX, R11),        /* copy a3 → RDX (last use of a3) */
                NOP_SEQWORD },

    /* T52: hi[0], MUL a3² */
    /* T52 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R11, RDX),     /* a3*a3: R11 pres, RDX=lo */
                NOP, NOP_SEQWORD },
    /* T53 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                SETCC_CONDB_DR(TMP3, TMP0),
                NOP, NOP_SEQWORD },

    /* Carry extraction @52, output x24 → R11 (R11=a3 no longer needed) */
    /* T54 */ { SHR_DSZ64_DRI(TMP8, TMP0, 52),
                ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                NOP, NOP_SEQWORD },
    /* T55 */ { SHL_DSZ64_DRI(TMP9, TMP0, 12),
                ADD_DSZ64_DRR(TMP0, TMP4, TMP5),
                NOP, NOP_SEQWORD },
    /* T56 */ { SHR_DSZ64_DRI(R11, TMP9, 12),     /* R11 = x24 */
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD },

    /* Save F's carry → TMP15 (x23, for block H) */
    /* T57 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                NOP, NOP_SEQWORD },
    /* T58 */ { OR_DSZ64_DRR(TMP15, TMP8, TMP1),  /* TMP15 = x23 */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ BLOCK G: x20 + d0*a1 + x24*R10  [2 MAC, start from x20, carry@52] → out[1] ═══ */
    /* x20 in RBX. d0=R15. a1=RSI. x24=R11. R10c=TMP10. */

    /* T59: set acc = x20, copy a1→RDX, start MUL d0*a1 */
    /* T59 */ { ZEROEXT_DSZ64_DR(TMP0, RBX),       /* TMP0 = x20 */
                ZEROEXT_DSZ64_DR(RDX, RSI),        /* copy a1 → RDX */
                NOP, NOP_SEQWORD },
    /* T60 */ { MUL_DSZ64_DRR(RCX, R15, RDX),     /* d0*a1: R15 pres, RDX=lo */
                NOP, NOP, NOP_SEQWORD },
    /* T61 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                NOP, NOP_SEQWORD },

    /* T62: hi[0], MUL x24*R10 */
    /* T62 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, TMP10, R11),   /* R10c * x24: TMP10 pres, R11=lo */
                NOP, NOP_SEQWORD },
    /* T63 */ { ADD_DSZ64_DRR(TMP0, TMP2, R11),
                SETCC_CONDB_DR(TMP3, TMP0),
                NOP, NOP_SEQWORD },

    /* Carry extraction @52, output out[1] → R11 */
    /* T64 */ { SHR_DSZ64_DRI(TMP8, TMP0, 52),
                ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                NOP, NOP_SEQWORD },
    /* T65 */ { SHL_DSZ64_DRI(TMP9, TMP0, 12),
                ADD_DSZ64_DRR(TMP0, TMP4, TMP5),
                NOP, NOP_SEQWORD },
    /* T66 */ { SHR_DSZ64_DRI(RBX, TMP9, 12),     /* RBX = out[1] (x27) — reuse RBX */
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD },

    /* Save G's carry → R11 (x26, for block I) */
    /* T67 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                NOP, NOP_SEQWORD },
    /* T68 */ { OR_DSZ64_DRR(R11, TMP8, TMP1),    /* R11 = x26 */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ BLOCK H: x23 + d3*a4  [1 MAC, 64-bit split] ═══ */
    /* x23 in TMP15. d3=TMP14. a4=R14 (last use). */
    /* 64-bit split: x30 = lo64, x29 = hi. No 52-bit masking. */

    /* T69: set acc = x23, MUL d3*a4 */
    /* T69 */ { ZEROEXT_DSZ64_DR(TMP0, TMP15),     /* TMP0 = x23 */
                MUL_DSZ64_DRR(RCX, TMP14, R14),   /* d3*a4: TMP14(d3) pres, R14=lo */
                NOP, NOP_SEQWORD },
    /* T70 */ { ADD_DSZ64_DRR(TMP2, TMP0, R14),   /* acc_lo = x23 + lo(d3*a4) */
                SETCC_CONDB_DR(TMP3, TMP2),
                NOP, NOP_SEQWORD },
    /* 64-bit split: x30 = TMP2 (all 64 bits), x29 = RCX + TMP3 */
    /* T71 */ { ADD_DSZ64_DRR(R14, RCX, TMP3),    /* R14 = x29 = hi + carry */
                NOP, NOP, NOP_SEQWORD },
    /* x30 stays in TMP2. x29 in R14. */

    /* ═══ BLOCK I: x26 + d0*a2 + a1² + x30*R10  [3 MAC, carry@52] → out[2] ═══ */
    /* x26=R11, d0=R15, a2=R12, a1=RSI, x30=TMP2, R10c=TMP10 */

    /* T72: set acc = x26, copy a2→RDX, start MUL d0*a2 */
    /* T72 */ { ZEROEXT_DSZ64_DR(TMP0, R11),       /* TMP0 = x26 */
                ZEROEXT_DSZ64_DR(RDX, R12),        /* copy a2 → RDX */
                NOP, NOP_SEQWORD },
    /* T73 */ { MUL_DSZ64_DRR(RCX, R15, RDX),     /* d0*a2: R15 pres, RDX=lo */
                NOP, NOP, NOP_SEQWORD },
    /* T74 */ { ADD_DSZ64_DRR(TMP15, TMP0, RDX),  /* acc = x26 + lo(d0*a2) — use TMP15 as acc */
                SETCC_CONDB_DR(TMP3, TMP15),
                ZEROEXT_DSZ64_DR(RDX, RSI),        /* copy a1 → RDX */
                NOP_SEQWORD },

    /* T75: hi[0], MUL a1² */
    /* T75 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, RSI, RDX),     /* a1²: RSI pres, RDX=lo */
                NOP, NOP_SEQWORD },
    /* T76 */ { ADD_DSZ64_DRR(TMP0, TMP15, RDX),  /* TMP0 = acc + lo(a1²) */
                SETCC_CONDB_DR(TMP3, TMP0),
                NOP, NOP_SEQWORD },

    /* T77: hi[1], MUL x30*R10. x30 is in TMP2 from block H. */
    /* T77 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, TMP10, TMP2),  /* R10c * x30 */
                NOP, NOP_SEQWORD },
    /* T78 */ { ADD_DSZ64_DRR(TMP2, TMP0, TMP2),  /* acc += lo(x30*R10) — TMP2 overwritten */
                SETCC_CONDB_DR(TMP3, TMP2),
                NOP, NOP_SEQWORD },

    /* Carry extraction @52, output out[2] → R13 (d2 no longer needed) */
    /* T79 */ { SHR_DSZ64_DRI(TMP8, TMP2, 52),
                ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                NOP, NOP_SEQWORD },
    /* T80 */ { SHL_DSZ64_DRI(TMP9, TMP2, 12),
                ADD_DSZ64_DRR(R8, R8, TMP4),
                ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
                NOP_SEQWORD },
    /* T81 */ { SHR_DSZ64_DRI(R13, TMP9, 12),     /* R13 = out[2] (x33) */
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD },

    /* ═══ I→J TRANSITION ═══ */
    /* T82 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                NOP, NOP_SEQWORD },
    /* T83 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),   /* TMP0 = carry from I */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ BLOCK J: carry + c3_saved + x29*R10000  → out[3] ═══ */
    /* c3_saved=R9, x29=R14, R10000c=TMP11. carry in TMP0. */

    /* T84: acc = carry + c3_saved */
    /* T84 */ { ADD_DSZ64_DRR(TMP0, TMP0, R9),    /* TMP0 = carry + c3_saved */
                NOP, NOP, NOP_SEQWORD },
    /* T85: MUL x29*R10000 */
    /* T85 */ { MUL_DSZ64_DRR(RCX, TMP11, R14),   /* R10000c * x29: TMP11 pres, R14=lo */
                ZEROEXT_DSZ64_DR(TMP15, RAX),      /* TMP15 = 0 (for fresh acc) */
                NOP, NOP_SEQWORD },
    /* Accumulate: acc = (carry + c3_saved) + x29*R10000 */
    /* Use TMP0 as the running acc_lo with the partial sum */
    /* T86 */ { ADD_DSZ64_DRR(TMP2, TMP0, R14),   /* TMP2 = partial + lo(x29*R10000) */
                SETCC_CONDB_DR(TMP3, TMP2),
                NOP, NOP_SEQWORD },
    /* T87 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),   /* TMP4 = hi + carry — this IS the full hi */
                NOP, NOP, NOP_SEQWORD },

    /* Carry extraction @52, output out[3] → R14 */
    /* T88 */ { SHR_DSZ64_DRI(TMP8, TMP2, 52),
                SHL_DSZ64_DRI(TMP1, TMP4, 12),    /* align hi for carry combine */
                NOP, NOP_SEQWORD },
    /* T89 */ { SHL_DSZ64_DRI(TMP9, TMP2, 12),
                OR_DSZ64_DRR(TMP0, TMP8, TMP1),   /* carry = lo_carry | (hi << 12) */
                NOP, NOP_SEQWORD },
    /* T90 */ { SHR_DSZ64_DRI(R14, TMP9, 12),     /* R14 = out[3] (x36) */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ BLOCK K: carry + c4_out → out[4] ═══ */
    /* carry = TMP0 (from T89). c4_out = R10. */
    /* T91 */ { ADD_DSZ64_DRR(RAX, TMP0, R10),    /* RAX = out[4] (x37) */
                NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("secp256k1_sq patch installed: %d triads at U7c00\n",
           (int)ARRAY_SZ(patch));
}

/* ── fe_sq via microcode ─────────────────────────────────────── */

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    register uint64_t *_in  asm("rcx") = (uint64_t *)a;
    register uint64_t *_out asm("r15") = out;

    asm volatile(
        "push r15\n\t"

        /* load 5 limbs */
        "mov rdi, [rcx]\n\t"
        "mov rsi, [rcx + 8]\n\t"
        "mov r12, [rcx + 16]\n\t"
        "mov r11, [rcx + 24]\n\t"
        "mov r14, [rcx + 32]\n\t"

        /* precompute doubled values */
        "lea r15, [rdi + rdi]\n\t"     /* 2*a0 */
        "lea r13, [rsi + rsi]\n\t"     /* 2*a1 */

        /* compute a4² in native code (avoids clobbering R14) */
        "mov rax, r14\n\t"
        "mul r14\n\t"                   /* RDX:RAX = a4² (R14 preserved) */
        "mov r9, rax\n\t"              /* R9 = a4sq_lo */
        "mov r10, rdx\n\t"             /* R10 = a4sq_hi */

        /* load reduction constant */
        "mov rbx, 0x1000003d1\n\t"

        /* clear accumulators */
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* fire microcode */
        "vmwrite rcx, rdx\n\t"

        /* recover output pointer, store 5 result limbs */
        /* Output mapping: RDI=out[0], RBX=out[1], R13=out[2], R14=out[3], RAX=out[4] */
        "pop rcx\n\t"
        "mov [rcx],      rdi\n\t"
        "mov [rcx + 8],  rbx\n\t"
        "mov [rcx + 16], r13\n\t"
        "mov [rcx + 24], r14\n\t"
        "mov [rcx + 32], rax\n\t"

        : "+r"(_in), "+r"(_out)
        :
        : "rax", "rbx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14",
          "memory", "cc"
    );
}

/* ── verification ────────────────────────────────────────────── */

static void fe_carry(uint64_t h[5]) {
    uint64_t c;
    c = h[0] >> 52; h[0] &= MASK52;
    h[1] += c;
    c = h[1] >> 52; h[1] &= MASK52;
    h[2] += c;
    c = h[2] >> 52; h[2] &= MASK52;
    h[3] += c;
    c = h[3] >> 52; h[3] &= MASK52;
    h[4] += c;
    c = h[4] >> 48; h[4] &= MASK48;
    /* reduce: c * (2^256 mod p) = c * 0x1000003d1 */
    __uint128_t t = (__uint128_t)h[0] + (__uint128_t)c * 0x1000003d1ULL;
    h[0] = (uint64_t)t & MASK52;
    c = (uint64_t)(t >> 52);
    h[1] += c;
}

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

typedef struct {
    const char *label;
    uint64_t    input[5];
    uint64_t    expected[5];
    int         has_expected;
} test_vec_t;

/*
 * Hand-verified expected outputs:
 *   0² = 0
 *   1² = 1
 *   9² = 81
 *   (2^52)² = 2^104 → {0, 0, 1, 0, 0}
 *   (2^208)² = 2^416 = 2^256 * 2^160 ≡ R1 * 2^160
 *     2^160 in Dettman: limb3 = 2^(160-156) = 2^4 = 16
 *     so 2^416 ≡ R1 * 16 at limb3 position, but R1*16 = R10 = 0x1000003d10
 *     which is > 52 bits, so it overflows into limb4.
 *     Actually: R1 * 2^160 = 0x1000003d1 * 2^160. In limbs:
 *     2^160 = 2^(3*52 + 4) = 16 * 2^156 → limb3 += 16, so the contribution is
 *     0x1000003d1 * 16 = 0x1000003d10 spread across limbs. This is complex.
 *     Skip hardcoded expected for this.
 */
static const test_vec_t test_vectors[] = {
    { "zero",   {0,0,0,0,0}, {0,0,0,0,0}, 1 },
    { "one",    {1,0,0,0,0}, {1,0,0,0,0}, 1 },
    { "nine",   {9,0,0,0,0}, {81,0,0,0,0}, 1 },
    { "2^52",   {0,1,0,0,0}, {0,0,1,0,0}, 1 },
    { "2^104",  {0,0,1,0,0}, {0,0,0,0,1}, 1 },
    { "all_1",  {1,1,1,1,1}, {0}, 0 },
    { "max52",  {MASK52,0,0,0,0}, {0}, 0 },
    { "all_max",{MASK52,MASK52,MASK52,MASK52,MASK48}, {0}, 0 },
    { "bitcoin_gen", {0x59F2815B16F81798ULL & MASK52,
                      0x029BFCDB2DCE28D9ULL & MASK52,
                      0x55A06295CE870B07ULL & MASK52,
                      0xF9DCBBAC55A06295ULL & MASK52,
                      0x79BE667EF9DCULL & MASK48}, {0}, 0 },
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[5], nat[5], ucd[5];
    fe_sq_reference(t->input, ref);
    fe_sq_native(t->input, nat);
    fe_sq_ucode(t->input, ucd);
    fe_carry(ref); fe_carry(nat); fe_carry(ucd);

    int ok = 1;
    if (memcmp(ref, nat, 40) != 0) {
        printf("  FAIL [%s] native != reference\n", t->label);
        for (int i = 0; i < 5; i++)
            printf("    [%d] native=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, nat[i], ref[i], nat[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (memcmp(ref, ucd, 40) != 0) {
        printf("  FAIL [%s] ucode != reference\n", t->label);
        for (int i = 0; i < 5; i++)
            printf("    [%d] ucode=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, ucd[i], ref[i], ucd[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (t->has_expected && memcmp(ref, t->expected, 40) != 0) {
        printf("  FAIL [%s] result != expected\n", t->label);
        for (int i = 0; i < 5; i++)
            printf("    [%d] got=%016" PRIx64 " exp=%016" PRIx64 " %s\n",
                   i, ref[i], t->expected[i],
                   ref[i] == t->expected[i] ? "" : "***");
        ok = 0;
    }
    if (ok) printf("  PASS [%s]\n", t->label);
    return ok;
}

static int verify_random_quiet(const uint64_t in[5]) {
    uint64_t ref[5], nat[5], ucd[5];
    fe_sq_reference(in, ref);
    fe_sq_native(in, nat);
    fe_sq_ucode(in, ucd);
    fe_carry(ref); fe_carry(nat); fe_carry(ucd);
    if (memcmp(ref, nat, 40) != 0 || memcmp(ref, ucd, 40) != 0) {
        printf("  FAIL random: in={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 ",%016" PRIx64 "}\n",
               in[0], in[1], in[2], in[3], in[4]);
        if (memcmp(ref, nat, 40) != 0) printf("    native mismatch\n");
        if (memcmp(ref, ucd, 40) != 0) printf("    ucode  mismatch\n");
        return 0;
    }
    return 1;
}

#define RANDOM_TESTS 10000
#define CHAIN_ITERS  1000

static int verify_all(void) {
    int pass = 0, fail = 0;

    printf("--- Known test vectors ---\n");
    for (int i = 0; i < (int)N_VECS; i++) {
        if (verify_one(&test_vectors[i])) pass++; else fail++;
    }

    printf("\n--- Random stress test (%d vectors) ---\n", RANDOM_TESTS);
    uint64_t rng = 0xBEEFCAFE42424242ULL;
    int rpass = 0;
    for (int i = 0; i < RANDOM_TESTS; i++) {
        uint64_t in[5];
        for (int j = 0; j < 4; j++)
            in[j] = splitmix64(&rng) & MASK52;
        in[4] = splitmix64(&rng) & MASK48;
        if (verify_random_quiet(in)) rpass++;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    printf("\n--- Iterated chain (%d sq) ---\n", CHAIN_ITERS);
    uint64_t bp[5] = { 0x59F2815B16F81ULL, 0x29BFCDB2DCE28ULL,
                        0x55A06295CE870ULL, 0xF9DCBBAC55A06ULL,
                        0x79BE667EF9DCULL };
    uint64_t ri[5], ni[5], ui[5];
    memcpy(ri, bp, 40); memcpy(ni, bp, 40); memcpy(ui, bp, 40);
    for (int i = 0; i < CHAIN_ITERS; i++) {
        fe_sq_reference(ri, ri);
        fe_sq_native(ni, ni);
        fe_sq_ucode(ui, ui);
    }
    fe_carry(ri); fe_carry(ni); fe_carry(ui);
    int ref_nat = memcmp(ri, ni, 40) == 0;
    int ref_ucd = memcmp(ri, ui, 40) == 0;
    printf("  ref==native: %s   ref==ucode: %s   -> %s\n",
           ref_nat ? "yes" : "NO", ref_ucd ? "yes" : "NO",
           (ref_nat && ref_ucd) ? "PASS" : "FAIL");
    if (ref_nat && ref_ucd) {
        pass++;
    } else {
        fail++;
        printf("  reference:"); for (int i=0;i<5;i++) printf(" %016" PRIx64, ri[i]); printf("\n");
        printf("  native:   "); for (int i=0;i<5;i++) printf(" %016" PRIx64, ni[i]); printf("\n");
        printf("  ucode:    "); for (int i=0;i<5;i++) printf(" %016" PRIx64, ui[i]); printf("\n");
    }

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

    printf("=== fe_sq secp256k1: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_secp256k1_sq_patch();

    /* ── verification ──────────────────────────────────────────── */
    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    /* ── benchmark ────────────────────────────────────────────── */
    uint64_t state[5] = { 0x59F2815B16F81ULL, 0x29BFCDB2DCE28ULL,
                           0x55A06295CE870ULL, 0xF9DCBBAC55A06ULL,
                           0x79BE667EF9DCULL };

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t tmp[5];

    /* native C -O3 */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_native(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Native -O3:  min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* microcode */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_ucode(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* clean up */
    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
