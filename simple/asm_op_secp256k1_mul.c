/*
 * asm_op_secp256k1_mul.c — fe_mul for secp256k1 Dettman (2^256 - 4294968273) via microcode
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
 * Computation order (from Fiat/Dettman, adapted for multiplication):
 *   B: a0*b3 + a1*b2 + a2*b1 + a3*b0 + lo(a4*b4)*R10          → carry@52 → c3_saved
 *   C: carry + a0*b4 + a1*b3 + a2*b2 + a3*b1 + a4*b0
 *           + hi(a4*b4)*R10000                                   → carry@52 → split 48/4
 *   D: carry + a1*b4 + a2*b3 + a3*b2 + a4*b1                   → carry@52 → x16
 *   E: a0*b0 + (x17 + x16<<4)*R1                               → carry@52 → out[0]
 *   F: x15 + a2*b4 + a3*b3 + a4*b2                             → carry@52 → x24
 *   G: x20 + a0*b1 + a1*b0 + x24*R10                           → carry@52 → out[1]
 *   H: x23 + a3*b4 + a4*b3                                     → 64-bit split → x29, x30
 *   I: x26 + a0*b2 + a1*b1 + a2*b0 + x30*R10                  → carry@52 → out[2]
 *   J: carry + c3_saved + x29*R10000                            → carry@52 → out[3]
 *   K: carry + c4_out                                           → out[4]
 *
 * Register convention (caller → microcode):
 *   RDI=a0  RSI=a1  R12=a2  R11=a3  R14=a4
 *   R13=b0  RAX=b1  RCX=b2  RDX=b3  R15=b4
 *   R9=lo(a4*b4)  R10=hi(a4*b4)   [computed in native before vmwrite]
 *   RBX=0x1000003d1
 *   R8=0
 *
 * Microcode PREP saves b[0..4] → TMP10..TMP14, zeros RAX,
 * and computes R15=R10_const, R13=R10000_const.
 *
 * Output: RDI=h0  RBX=h1  R12=h2  R11=h3  RAX=h4
 *
 * Build:  make PROG=asm_op_secp256k1_mul
 * Run:    sudo taskset -c 0 ./asm_op_secp256k1_mul_static
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

/* ── fe_mul native C (matches Fiat secp256k1_dettman_mul) ──── */

static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t a0=a[0], a1=a[1], a2=a[2], a3=a[3], a4=a[4];
    uint64_t b0=b[0], b1=b[1], b2=b[2], b3=b[3], b4=b[4];

    __uint128_t t = (__uint128_t)a4 * b4;
    uint64_t prodhi = (uint64_t)(t >> 64);
    uint64_t prodlo = (uint64_t)t;

    /* Block B: c3_raw */
    t = (__uint128_t)a0*b3 + (__uint128_t)a1*b2
      + (__uint128_t)a2*b1 + (__uint128_t)a3*b0
      + (__uint128_t)prodlo * 0x1000003d10ULL;
    uint64_t carry = (uint64_t)(t >> 52);
    uint64_t c3_saved = (uint64_t)t & MASK52;

    /* Block C: c4_raw → split */
    t = carry
      + (__uint128_t)a0*b4 + (__uint128_t)a1*b3
      + (__uint128_t)a2*b2 + (__uint128_t)a3*b1 + (__uint128_t)a4*b0
      + (__uint128_t)prodhi * 0x1000003d10000ULL;
    carry = (uint64_t)(t >> 52);
    uint64_t c4_raw = (uint64_t)t & MASK52;
    uint64_t x17 = c4_raw >> 48;
    uint64_t c4_out = c4_raw & MASK48;

    /* Block D */
    t = carry + (__uint128_t)a1*b4 + (__uint128_t)a2*b3
      + (__uint128_t)a3*b2 + (__uint128_t)a4*b1;
    uint64_t x15 = (uint64_t)(t >> 52);
    uint64_t x16 = (uint64_t)t & MASK52;

    /* Block E: out[0] */
    t = (__uint128_t)a0*b0
      + (__uint128_t)(x17 + (x16 << 4)) * 0x1000003d1ULL;
    uint64_t x20 = (uint64_t)(t >> 52);
    out[0] = (uint64_t)t & MASK52;

    /* Block F */
    t = x15 + (__uint128_t)a2*b4 + (__uint128_t)a3*b3 + (__uint128_t)a4*b2;
    uint64_t x23 = (uint64_t)(t >> 52);
    uint64_t x24 = (uint64_t)t & MASK52;

    /* Block G: out[1] */
    t = x20 + (__uint128_t)a0*b1 + (__uint128_t)a1*b0
      + (__uint128_t)x24 * 0x1000003d10ULL;
    uint64_t x26 = (uint64_t)(t >> 52);
    out[1] = (uint64_t)t & MASK52;

    /* Block H: 64-bit split */
    t = x23 + (__uint128_t)a3*b4 + (__uint128_t)a4*b3;
    uint64_t x29 = (uint64_t)(t >> 64);
    uint64_t x30 = (uint64_t)t;

    /* Block I: out[2] */
    t = x26 + (__uint128_t)a0*b2 + (__uint128_t)a1*b1 + (__uint128_t)a2*b0
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

/* ── independent reference (big-integer multiply mod p) ────────── */

static void fe_mul_reference(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    /*
     * 1. Convert Dettman limbs → flat 4×64 integer (256 bits)
     * 2. Schoolbook multiply → 8×64 (512 bits)
     * 3. Reduce mod p = 2^256 - 0x1000003d1
     * 4. Convert back to Dettman limbs
     */

    /* 1. Flat: val = a0 + a1*2^52 + a2*2^104 + a3*2^156 + a4*2^208 */
    __uint128_t acc;
    uint64_t va[4] = {0}, vb[4] = {0};

    acc = (__uint128_t)a[0];
    acc += (__uint128_t)a[1] << 52;
    va[0] = (uint64_t)acc; acc >>= 64;
    acc += (__uint128_t)a[2] << (104 - 64);
    va[1] = (uint64_t)acc; acc >>= 64;
    acc += (__uint128_t)a[3] << (156 - 128);
    va[2] = (uint64_t)acc; acc >>= 64;
    acc += (__uint128_t)a[4] << (208 - 192);
    va[3] = (uint64_t)acc;

    acc = (__uint128_t)b[0];
    acc += (__uint128_t)b[1] << 52;
    vb[0] = (uint64_t)acc; acc >>= 64;
    acc += (__uint128_t)b[2] << (104 - 64);
    vb[1] = (uint64_t)acc; acc >>= 64;
    acc += (__uint128_t)b[3] << (156 - 128);
    vb[2] = (uint64_t)acc; acc >>= 64;
    acc += (__uint128_t)b[4] << (208 - 192);
    vb[3] = (uint64_t)acc;

    /* 2. Schoolbook multiply va[0..3] × vb[0..3] → r[0..7] */
    __uint128_t rr[8] = {0};
    for (int i = 0; i < 4; i++) {
        __uint128_t carry = 0;
        for (int j = 0; j < 4; j++) {
            __uint128_t prod = (__uint128_t)va[i] * vb[j]
                             + (uint64_t)rr[i+j] + carry;
            rr[i+j] = (uint64_t)prod;
            carry = prod >> 64;
        }
        rr[i+4] += carry;
    }
    uint64_t r[8];
    for (int k = 0; k < 8; k++) r[k] = (uint64_t)rr[k];

    /* 3. Reduce mod p = 2^256 - c where c = 0x1000003d1 */
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
    acc |= (__uint128_t)r[2] << (128 - 104);
    out[2] = (uint64_t)acc & MASK52;  acc >>= 52;
    acc |= (__uint128_t)r[3] << (192 - 156);
    out[3] = (uint64_t)acc & MASK52;  acc >>= 52;
    out[4] = (uint64_t)acc & MASK48;

    uint64_t top = (uint64_t)(acc >> 48);
    if (top) {
        __uint128_t s = (__uint128_t)out[0] + (__uint128_t)top * C;
        out[0] = (uint64_t)s & MASK52;
        uint64_t c = (uint64_t)(s >> 52);
        out[1] += c;
    }
}

/* ── microcode patch ──────────────────────────────────────────── */

static void install_secp256k1_mul_patch(void) {
    ucode_t patch[] = {
    /*
     * Entry: RDI=a0 RSI=a1 R12=a2 R11=a3 R14=a4
     *        R13=b0 RAX=b1 RCX=b2 RDX=b3 R15=b4
     *        R9=(a4*b4)_lo  R10=(a4*b4)_hi  RBX=R1  R8=0
     * After PREP: TMP10-14=b0-b4, RAX=0, R15=R10c, R13=R10000c
     */

    /* ═══ PREP ═══ */
    /* P0 */ { ZEROEXT_DSZ64_DR(TMP10, R13),       /* TMP10 = b0 */
               ZEROEXT_DSZ64_DR(TMP11, RAX),       /* TMP11 = b1 */
               ZEROEXT_DSZ64_DR(TMP12, RCX),       /* TMP12 = b2 */
               NOP_SEQWORD },
    /* P1 */ { ZEROEXT_DSZ64_DR(TMP13, RDX),       /* TMP13 = b3 */
               ZEROEXT_DSZ64_DR(TMP14, R15),       /* TMP14 = b4 */
               NOTAND_DSZ64_DRR(RAX, RAX, RAX),    /* RAX = 0 */
               NOP_SEQWORD },
    /* P2 */ { SHL_DSZ64_DRI(R15, RBX, 4),         /* R15 = R10 = R1<<4 */
               SHL_DSZ64_DRI(R13, RBX, 16),        /* R13 = R10000 = R1<<16 */
               NOP, NOP_SEQWORD },

    /* ═══ BLOCK B: a0*b3 + a1*b2 + a2*b1 + a3*b0 + prodlo*R10  [5 MAC, carry@52] ═══ */

    /* init acc=0, copy b3→RDX */
    /* B0 */ { ZEROEXT_DSZ64_DR(TMP0, RAX),
               ZEROEXT_DSZ64_DR(RDX, TMP13),
               NOP, NOP_SEQWORD },
    /* Product 1: a0 × b3 */
    /* B1 */ { MUL_DSZ64_DRR(RCX, RDI, RDX),
               NOP, NOP, NOP_SEQWORD },
    /* B2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               ZEROEXT_DSZ64_DR(RDX, TMP12),       /* prep b2 */
               NOP_SEQWORD },
    /* Product 2: a1 × b2 */
    /* B3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, RSI, RDX),
               NOP, NOP_SEQWORD },
    /* B4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               ZEROEXT_DSZ64_DR(RDX, TMP11),       /* prep b1 */
               NOP_SEQWORD },
    /* Product 3: a2 × b1 */
    /* B5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R12, RDX),
               NOP, NOP_SEQWORD },
    /* B6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               ZEROEXT_DSZ64_DR(RDX, TMP10),       /* prep b0 */
               NOP_SEQWORD },
    /* Product 4: a3 × b0 */
    /* B7 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R11, RDX),
               NOP, NOP_SEQWORD },
    /* B8 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               NOP, NOP_SEQWORD },
    /* Product 5: prodlo(R9) × R10(R15) */
    /* B9 */ { ADD_DSZ64_DRR(TMP7, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R15, R9),        /* R15(R10) preserved, R9=lo */
               NOP, NOP_SEQWORD },
    /* B10 */ { ADD_DSZ64_DRR(TMP2, TMP0, R9),
                SETCC_CONDB_DR(TMP3, TMP2),
                NOP, NOP_SEQWORD },

    /* MAC_TAIL_5 → R9 = c3_saved */
    /* B11 */ { SHR_DSZ64_DRI(TMP8, TMP2, 52),
                ADD_DSZ64_DRR(TMP0, RCX, TMP3),
                NOP, NOP_SEQWORD },
    /* B12 */ { SHL_DSZ64_DRI(TMP9, TMP2, 12),
                ADD_DSZ64_DRR(TMP1, TMP4, TMP5),
                ADD_DSZ64_DRR(TMP0, TMP6, TMP0),
                NOP_SEQWORD },
    /* B13 */ { ADD_DSZ64_DRR(R8, R8, TMP7),
                ADD_DSZ64_DRR(TMP0, TMP1, TMP0),
                NOP, NOP_SEQWORD },
    /* B14 */ { SHR_DSZ64_DRI(R9, TMP9, 12),       /* R9 = c3_saved */
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD },

    /* ═══ B→C TRANSITION ═══ */
    /* BC0 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                ZEROEXT_DSZ64_DR(RDX, TMP14),      /* prep b4 */
                NOP_SEQWORD },
    /* BC1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                MUL_DSZ64_DRR(RCX, RDI, RDX),     /* a0 × b4 */
                NOP, NOP_SEQWORD },

    /* ═══ BLOCK C: carry + a0*b4 + a1*b3 + a2*b2 + a3*b1 + a4*b0 + prodhi*R10000  [6 MAC] ═══ */

    /* C0 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               ZEROEXT_DSZ64_DR(RDX, TMP13),       /* prep b3 */
               NOP_SEQWORD },
    /* Product 2: a1 × b3 */
    /* C1 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, RSI, RDX),
               NOP, NOP_SEQWORD },
    /* C2 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               ZEROEXT_DSZ64_DR(RDX, TMP12),       /* prep b2 */
               NOP_SEQWORD },
    /* Product 3: a2 × b2 */
    /* C3 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R12, RDX),
               NOP, NOP_SEQWORD },
    /* C4 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               ZEROEXT_DSZ64_DR(RDX, TMP11),       /* prep b1 */
               NOP_SEQWORD },
    /* Product 4: a3 × b1 */
    /* C5 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R11, RDX),
               NOP, NOP_SEQWORD },
    /* C6 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               ZEROEXT_DSZ64_DR(RDX, TMP10),       /* prep b0 */
               NOP_SEQWORD },
    /* Product 5: a4 × b0 */
    /* C7 */ { ADD_DSZ64_DRR(TMP7, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R14, RDX),
               NOP, NOP_SEQWORD },
    /* C8 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               NOP, NOP_SEQWORD },
    /* Product 6: prodhi(R10) × R10000(R13) — hi save → TMP1 */
    /* C9 */ { ADD_DSZ64_DRR(TMP1, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R13, R10),      /* R13(R10000) preserved, R10=lo */
               NOP, NOP_SEQWORD },
    /* C10 */ { ADD_DSZ64_DRR(TMP0, TMP2, R10),
                SETCC_CONDB_DR(TMP3, TMP0),
                NOP, NOP_SEQWORD },

    /* Custom MAC_TAIL_6: acc=TMP0, 6 hi saves in TMP4,TMP5,TMP6,TMP7,TMP1,(RCX+TMP3) */
    /* CT0 */ { SHR_DSZ64_DRI(TMP8, TMP0, 52),
                ADD_DSZ64_DRR(TMP2, RCX, TMP3),    /* TMP2 = hi[5] */
                NOP, NOP_SEQWORD },
    /* CT1 */ { SHL_DSZ64_DRI(TMP9, TMP0, 12),
                ADD_DSZ64_DRR(TMP2, TMP2, TMP1),   /* TMP2 = hi[4]+hi[5] */
                NOP, NOP_SEQWORD },
    /* CT2 */ { ADD_DSZ64_DRR(R8, R8, TMP4),
                ADD_DSZ64_DRR(TMP1, TMP5, TMP6),
                NOP, NOP_SEQWORD },
    /* CT3 */ { ADD_DSZ64_DRR(R8, R8, TMP7),
                ADD_DSZ64_DRR(TMP1, TMP1, TMP2),
                NOP, NOP_SEQWORD },
    /* CT4 */ { SHR_DSZ64_DRI(RDX, TMP9, 12),      /* RDX = c4_raw */
                ADD_DSZ64_DRR(R8, R8, TMP1),
                NOP, NOP_SEQWORD },

    /* Split c4_raw into x17 (top 4 bits) and c4_out (bottom 48) */
    /* CS0 */ { SHR_DSZ64_DRI(TMP15, RDX, 48),     /* TMP15 = x17 */
                SHL_DSZ64_DRI(TMP9, RDX, 16),
                NOP, NOP_SEQWORD },
    /* CS1 */ { SHR_DSZ64_DRI(R10, TMP9, 16),      /* R10 = c4_out */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ C→D TRANSITION ═══ */
    /* CD0 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                ZEROEXT_DSZ64_DR(RDX, TMP14),      /* prep b4 */
                NOP_SEQWORD },
    /* CD1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                MUL_DSZ64_DRR(RCX, RSI, RDX),     /* a1 × b4 */
                NOP, NOP_SEQWORD },

    /* ═══ BLOCK D: carry + a1*b4 + a2*b3 + a3*b2 + a4*b1  [4 MAC] ═══ */

    /* D0 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               ZEROEXT_DSZ64_DR(RDX, TMP13),       /* prep b3 */
               NOP_SEQWORD },
    /* Product 2: a2 × b3 */
    /* D1 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R12, RDX),
               NOP, NOP_SEQWORD },
    /* D2 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               ZEROEXT_DSZ64_DR(RDX, TMP12),       /* prep b2 */
               NOP_SEQWORD },
    /* Product 3: a3 × b2 */
    /* D3 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R11, RDX),
               NOP, NOP_SEQWORD },
    /* D4 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               ZEROEXT_DSZ64_DR(RDX, TMP11),       /* prep b1 */
               NOP_SEQWORD },
    /* Product 4: a4 × b1 */
    /* D5 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R14, RDX),
               NOP, NOP_SEQWORD },
    /* D6 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               NOP, NOP_SEQWORD },

    /* MAC_TAIL_4 → RDX = x16 */
    /* DT0 */ { SHR_DSZ64_DRI(TMP8, TMP0, 52),
                ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                NOP, NOP_SEQWORD },
    /* DT1 */ { SHL_DSZ64_DRI(TMP9, TMP0, 12),
                ADD_DSZ64_DRR(TMP0, TMP4, TMP5),
                ADD_DSZ64_DRR(TMP1, TMP6, TMP7),
                NOP_SEQWORD },
    /* DT2 */ { ADD_DSZ64_DRR(TMP0, TMP0, TMP1),
                NOP, NOP, NOP_SEQWORD },
    /* DT3 */ { SHR_DSZ64_DRI(RDX, TMP9, 12),     /* RDX = x16 */
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD },

    /* ═══ D→E: save x15, compute combined ═══ */
    /* Save D carry → R13 (overwriting R10000, recompute before J) */
    /* DE0 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                NOP, NOP_SEQWORD },
    /* DE1 */ { OR_DSZ64_DRR(R13, TMP8, TMP1),    /* R13 = x15 */
                NOP, NOP, NOP_SEQWORD },

    /* Compute combined = x17 + (x16 << 4) */
    /* DE2 */ { SHL_DSZ64_DRI(TMP0, RDX, 4),
                NOP, NOP, NOP_SEQWORD },
    /* DE3 */ { ADD_DSZ64_DRR(TMP0, TMP0, TMP15), /* TMP0 = (x16<<4) + x17 */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ BLOCK E: a0*b0 + combined*R1  [2 MAC, fresh start] → out[0] ═══ */

    /* E0 */ { ZEROEXT_DSZ64_DR(TMP15, RAX),       /* TMP15 = 0 (acc init) */
               ZEROEXT_DSZ64_DR(RDX, TMP10),       /* copy b0 → RDX */
               NOP, NOP_SEQWORD },
    /* E1 */ { MUL_DSZ64_DRR(RCX, RDI, RDX),      /* a0 × b0 */
               NOP, NOP, NOP_SEQWORD },
    /* E2 */ { ADD_DSZ64_DRR(TMP2, TMP15, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               NOP, NOP_SEQWORD },
    /* Product 2: combined(TMP0) × R1(RBX) */
    /* E3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, RBX, TMP0),    /* RBX(R1) preserved, TMP0=lo */
               NOP, NOP_SEQWORD },
    /* E4 */ { ADD_DSZ64_DRR(TMP0, TMP2, TMP0),
               SETCC_CONDB_DR(TMP3, TMP0),
               NOP, NOP_SEQWORD },

    /* MAC_TAIL_2 → TMP15 = out[0] (save in TMP15) */
    /* ET0 */ { SHR_DSZ64_DRI(TMP8, TMP0, 52),
                ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                NOP, NOP_SEQWORD },
    /* ET1 */ { SHL_DSZ64_DRI(TMP9, TMP0, 12),
                ADD_DSZ64_DRR(TMP0, TMP4, TMP5),
                NOP, NOP_SEQWORD },
    /* ET2 */ { SHR_DSZ64_DRI(TMP15, TMP9, 12),   /* TMP15 = out[0] */
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD },

    /* Save E carry → RBX (R1 no longer needed) */
    /* EX0 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                NOP, NOP_SEQWORD },
    /* EX1 */ { OR_DSZ64_DRR(RBX, TMP8, TMP1),    /* RBX = x20 */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ BLOCK F: x15 + a2*b4 + a3*b3 + a4*b2  [3 MAC from x15] ═══ */

    /* F0 */ { ZEROEXT_DSZ64_DR(TMP0, R13),        /* TMP0 = x15 */
               ZEROEXT_DSZ64_DR(RDX, TMP14),       /* copy b4 → RDX */
               NOP, NOP_SEQWORD },
    /* Product 1: a2 × b4 */
    /* F1 */ { MUL_DSZ64_DRR(RCX, R12, RDX),
               NOP, NOP, NOP_SEQWORD },
    /* F2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               ZEROEXT_DSZ64_DR(RDX, TMP13),       /* prep b3 */
               NOP_SEQWORD },
    /* Product 2: a3 × b3 */
    /* F3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R11, RDX),
               NOP, NOP_SEQWORD },
    /* F4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               ZEROEXT_DSZ64_DR(RDX, TMP12),       /* prep b2 */
               NOP_SEQWORD },
    /* Product 3: a4 × b2 */
    /* F5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R14, RDX),
               NOP, NOP_SEQWORD },
    /* F6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               NOP, NOP_SEQWORD },

    /* MAC_TAIL_3 → RDX = x24, carry → R13 = x23 */
    /* FT0 */ { SHR_DSZ64_DRI(TMP8, TMP2, 52),
                ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                NOP, NOP_SEQWORD },
    /* FT1 */ { SHL_DSZ64_DRI(TMP9, TMP2, 12),
                ADD_DSZ64_DRR(R8, R8, TMP4),
                ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
                NOP_SEQWORD },
    /* FT2 */ { SHR_DSZ64_DRI(RDX, TMP9, 12),     /* RDX = x24 */
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD },

    /* Save F carry → R13 = x23 (overwriting x15, consumed) */
    /* FX0 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                NOP, NOP_SEQWORD },
    /* FX1 */ { OR_DSZ64_DRR(R13, TMP8, TMP1),    /* R13 = x23 */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ BLOCK G: x20 + x24*R10 + a0*b1 + a1*b0  [3 MAC from x20] → out[1] ═══ */
    /* x24 is in RDX (from block F tail). x20 is in RBX. */

    /* G0: acc=x20, fire x24(RDX) × R10(R15) */
    /* G0 */ { ZEROEXT_DSZ64_DR(TMP0, RBX),
               MUL_DSZ64_DRR(RCX, R15, RDX),      /* R10_const × x24 */
               NOP, NOP_SEQWORD },
    /* G1 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               ZEROEXT_DSZ64_DR(RDX, TMP11),       /* prep b1 */
               NOP_SEQWORD },
    /* Product 2: a0 × b1 */
    /* G2 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, RDI, RDX),
               NOP, NOP_SEQWORD },
    /* G3 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               ZEROEXT_DSZ64_DR(RDX, TMP10),       /* prep b0 */
               NOP_SEQWORD },
    /* Product 3: a1 × b0 */
    /* G4 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, RSI, RDX),
               NOP, NOP_SEQWORD },
    /* G5 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               NOP, NOP_SEQWORD },

    /* MAC_TAIL_3 → RBX = out[1] */
    /* GT0 */ { SHR_DSZ64_DRI(TMP8, TMP2, 52),
                ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                NOP, NOP_SEQWORD },
    /* GT1 */ { SHL_DSZ64_DRI(TMP9, TMP2, 12),
                ADD_DSZ64_DRR(R8, R8, TMP4),
                ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
                NOP_SEQWORD },
    /* GT2 */ { SHR_DSZ64_DRI(RBX, TMP9, 12),     /* RBX = out[1] */
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD },

    /* Save G carry → RDX = x26 */
    /* GX0 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                NOP, NOP_SEQWORD },
    /* GX1 */ { OR_DSZ64_DRR(RDX, TMP8, TMP1),    /* RDX = x26 */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ BLOCK H: x23 + a3*b4 + a4*b3  [2 MAC, 64-bit split] ═══ */
    /* x23=R13, x26=RDX (save before clobbering) */

    /* H0: save x26, set acc=x23, prep b4→RDX */
    /* H0 */ { ZEROEXT_DSZ64_DR(TMP0, R13),        /* TMP0 = x23 */
               ZEROEXT_DSZ64_DR(TMP1, RDX),        /* TMP1 = x26 (save) */
               ZEROEXT_DSZ64_DR(RDX, TMP14),       /* prep b4 */
               NOP_SEQWORD },
    /* Product 1: a3 × b4 */
    /* H1 */ { MUL_DSZ64_DRR(RCX, R11, RDX),
               NOP, NOP, NOP_SEQWORD },
    /* H2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               ZEROEXT_DSZ64_DR(RDX, TMP13),       /* prep b3 */
               NOP_SEQWORD },
    /* Product 2: a4 × b3 */
    /* H3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R14, RDX),
               NOP, NOP_SEQWORD },
    /* H4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               NOP, NOP_SEQWORD },

    /* 64-bit split: x30 = TMP0 (lo 64), x29 = TMP4 + RCX + TMP3 (total hi) */
    /* H5 */ { ADD_DSZ64_DRR(R14, RCX, TMP3),     /* R14 = hi[1] + carry */
               NOP, NOP, NOP_SEQWORD },
    /* H6 */ { ADD_DSZ64_DRR(R14, R14, TMP4),     /* R14 = x29 = hi[0]+hi[1]+carry */
               NOP, NOP, NOP_SEQWORD },
    /* x30 = TMP0, x26 = TMP1 */

    /* ═══ BLOCK I: x26 + a0*b2 + a1*b1 + a2*b0 + x30*R10  [4 MAC] → out[2] ═══ */

    /* I0: acc = x26 (from TMP1), fire x30(TMP0) × R10(R15) */
    /* We need x30 in a srcB position. Copy TMP0→RDX, set acc from TMP1. */
    /* I0 */ { ZEROEXT_DSZ64_DR(RDX, TMP0),        /* RDX = x30 */
               ZEROEXT_DSZ64_DR(TMP0, TMP1),       /* TMP0 = x26 (overwrite x30, saved in RDX) */
               NOP, NOP_SEQWORD },
    /* I1 */ { MUL_DSZ64_DRR(RCX, R15, RDX),      /* R10_const × x30 */
               NOP, NOP, NOP_SEQWORD },
    /* I2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               ZEROEXT_DSZ64_DR(RDX, TMP12),       /* prep b2 */
               NOP_SEQWORD },
    /* Product 2: a0 × b2 */
    /* I3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, RDI, RDX),
               NOP, NOP_SEQWORD },
    /* I4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               ZEROEXT_DSZ64_DR(RDX, TMP11),       /* prep b1 */
               NOP_SEQWORD },
    /* Product 3: a1 × b1 */
    /* I5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, RSI, RDX),
               NOP, NOP_SEQWORD },
    /* I6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               ZEROEXT_DSZ64_DR(RDX, TMP10),       /* prep b0 */
               NOP_SEQWORD },
    /* Product 4: a2 × b0 */
    /* I7 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R12, RDX),
               NOP, NOP_SEQWORD },
    /* I8 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               NOP, NOP_SEQWORD },

    /* MAC_TAIL_4 → R12 = out[2] */
    /* IT0 */ { SHR_DSZ64_DRI(TMP8, TMP0, 52),
                ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                NOP, NOP_SEQWORD },
    /* IT1 */ { SHL_DSZ64_DRI(TMP9, TMP0, 12),
                ADD_DSZ64_DRR(TMP0, TMP4, TMP5),
                ADD_DSZ64_DRR(TMP1, TMP6, TMP7),
                NOP_SEQWORD },
    /* IT2 */ { ADD_DSZ64_DRR(TMP0, TMP0, TMP1),
                NOP, NOP, NOP_SEQWORD },
    /* IT3 */ { SHR_DSZ64_DRI(R12, TMP9, 12),     /* R12 = out[2] */
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD },

    /* ═══ I→J TRANSITION ═══ */
    /* IJ0 */ { SHL_DSZ64_DRI(TMP1, R8, 12),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                NOP, NOP_SEQWORD },
    /* IJ1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),   /* TMP0 = carry from I */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ BLOCK J: carry + c3_saved + x29*R10000 → out[3] ═══ */
    /* c3_saved=R9, x29=R14. R10000 must be recomputed (was in R13, overwritten). */

    /* Recompute R10000 = R10(R15) << 12 → TMP2 */
    /* J0 */ { ADD_DSZ64_DRR(TMP0, TMP0, R9),     /* TMP0 = carry + c3_saved */
               SHL_DSZ64_DRI(TMP2, R15, 12),      /* TMP2 = R10000 */
               NOP, NOP_SEQWORD },

    /* MUL x29 × R10000 */
    /* J1 */ { MUL_DSZ64_DRR(RCX, TMP2, R14),     /* TMP2(R10000) preserved, R14=lo */
               NOP, NOP, NOP_SEQWORD },
    /* J2 */ { ADD_DSZ64_DRR(TMP2, TMP0, R14),    /* TMP2 = partial + lo(x29*R10000) */
               SETCC_CONDB_DR(TMP3, TMP2),
               NOP, NOP_SEQWORD },
    /* J3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),    /* TMP4 = hi + carry */
               NOP, NOP, NOP_SEQWORD },

    /* Carry extraction @52 → R11 = out[3] */
    /* JT0 */ { SHR_DSZ64_DRI(TMP8, TMP2, 52),
                SHL_DSZ64_DRI(TMP1, TMP4, 12),
                NOP, NOP_SEQWORD },
    /* JT1 */ { SHL_DSZ64_DRI(TMP9, TMP2, 12),
                OR_DSZ64_DRR(TMP0, TMP8, TMP1),   /* carry = lo_carry | (hi<<12) */
                NOP, NOP_SEQWORD },
    /* JT2 */ { SHR_DSZ64_DRI(R11, TMP9, 12),     /* R11 = out[3] */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ BLOCK K: carry + c4_out → out[4] ═══ */
    /* K0 */ { ADD_DSZ64_DRR(RAX, TMP0, R10),     /* RAX = out[4] */
               NOP, NOP, NOP_SEQWORD },

    /* Move out[0] from TMP15 to RDI */
    /* M0 */ { ZEROEXT_DSZ64_DR(RDI, TMP15),
               NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("secp256k1_mul patch installed: %d triads at U7c00\n",
           (int)ARRAY_SZ(patch));
}

/* ── fe_mul via microcode ─────────────────────────────────────── */

static void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_b   asm("rbx") = (uint64_t *)b;
    register uint64_t *_out asm("r15") = out;

    asm volatile(
        "push r15\n\t"

        /* load a[0..4] from rcx */
        "mov rdi, [rcx]\n\t"
        "mov rsi, [rcx + 8]\n\t"
        "mov r12, [rcx + 16]\n\t"
        "mov r11, [rcx + 24]\n\t"
        "mov r14, [rcx + 32]\n\t"

        /* compute a4 * b4 in native */
        "mov rax, [rbx + 32]\n\t"  /* rax = b4 */
        "mov r15, rax\n\t"         /* save b4 */
        "mul r14\n\t"              /* rdx:rax = a4 * b4 */
        "mov r9, rax\n\t"          /* R9 = (a4*b4)_lo */
        "mov r10, rdx\n\t"         /* R10 = (a4*b4)_hi */

        /* load b[0..3] into registers, b4 already in r15 */
        "mov r13, [rbx]\n\t"       /* b0 → R13 */
        "mov rax, [rbx + 8]\n\t"   /* b1 → RAX */
        "mov rcx, [rbx + 16]\n\t"  /* b2 → RCX */
        "mov rdx, [rbx + 24]\n\t"  /* b3 → RDX */

        /* load R1 constant */
        "mov rbx, 0x1000003d1\n\t"

        /* clear R8 */
        "xor r8d, r8d\n\t"

        /* fire microcode */
        "vmwrite rcx, rdx\n\t"

        /* recover output pointer, store results */
        "pop rcx\n\t"
        "mov [rcx],      rdi\n\t"  /* out[0] */
        "mov [rcx + 8],  rbx\n\t"  /* out[1] */
        "mov [rcx + 16], r12\n\t"  /* out[2] */
        "mov [rcx + 24], r11\n\t"  /* out[3] */
        "mov [rcx + 32], rax\n\t"  /* out[4] */

        : "+r"(_a), "+r"(_b), "+r"(_out)
        :
        : "rax", "rdx", "rsi", "rdi",
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
    c = h[3] >> 52; h[3] &= MASK48;
    h[4] += c;
    c = h[4] >> 48; h[4] &= MASK48;
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
    uint64_t    a[5];
    uint64_t    b[5];
    uint64_t    expected[5];
    int         has_expected;
} test_vec_t;

static const test_vec_t test_vectors[] = {
    { "0*0",
      {0,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0}, 1 },
    { "1*1",
      {1,0,0,0,0}, {1,0,0,0,0}, {1,0,0,0,0}, 1 },
    { "0*1",
      {0,0,0,0,0}, {1,0,0,0,0}, {0,0,0,0,0}, 1 },
    { "1*0",
      {1,0,0,0,0}, {0,0,0,0,0}, {0,0,0,0,0}, 1 },
    { "2*3",
      {2,0,0,0,0}, {3,0,0,0,0}, {6,0,0,0,0}, 1 },
    { "9*9",
      {9,0,0,0,0}, {9,0,0,0,0}, {81,0,0,0,0}, 1 },
    { "2^52*2^52",
      {0,1,0,0,0}, {0,1,0,0,0}, {0,0,1,0,0}, 1 },
    { "2^104*2^104",
      {0,0,1,0,0}, {0,0,1,0,0}, {0,0,0,0,1}, 1 },
    { "1*gen",
      {1,0,0,0,0},
      {0x59F2815B16F81798ULL & MASK52, 0x029BFCDB2DCE28D9ULL & MASK52,
       0x55A06295CE870B07ULL & MASK52, 0xF9DCBBAC55A06295ULL & MASK52,
       0x79BE667EF9DCULL & MASK48},
      {0}, 0 },
    { "gen*gen",
      {0x59F2815B16F81798ULL & MASK52, 0x029BFCDB2DCE28D9ULL & MASK52,
       0x55A06295CE870B07ULL & MASK52, 0xF9DCBBAC55A06295ULL & MASK52,
       0x79BE667EF9DCULL & MASK48},
      {0x59F2815B16F81798ULL & MASK52, 0x029BFCDB2DCE28D9ULL & MASK52,
       0x55A06295CE870B07ULL & MASK52, 0xF9DCBBAC55A06295ULL & MASK52,
       0x79BE667EF9DCULL & MASK48},
      {0}, 0 },
    { "all_max",
      {MASK52,MASK52,MASK52,MASK52,MASK48},
      {MASK52,MASK52,MASK52,MASK52,MASK48},
      {0}, 0 },
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[5], nat[5], ucd[5];
    fe_mul_reference(t->a, t->b, ref);
    fe_mul_native(t->a, t->b, nat);
    fe_mul_ucode(t->a, t->b, ucd);
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

static int verify_random_quiet(const uint64_t a[5], const uint64_t b[5]) {
    uint64_t ref[5], nat[5], ucd[5];
    fe_mul_reference(a, b, ref);
    fe_mul_native(a, b, nat);
    fe_mul_ucode(a, b, ucd);
    fe_carry(ref); fe_carry(nat); fe_carry(ucd);
    if (memcmp(ref, nat, 40) != 0 || memcmp(ref, ucd, 40) != 0) {
        printf("  FAIL random: a={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 ",%016" PRIx64 "}\n",
               a[0], a[1], a[2], a[3], a[4]);
        printf("           b={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 ",%016" PRIx64 "}\n",
               b[0], b[1], b[2], b[3], b[4]);
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
        uint64_t a[5], b[5];
        for (int j = 0; j < 4; j++)
            a[j] = splitmix64(&rng) & MASK52;
        a[4] = splitmix64(&rng) & MASK48;
        for (int j = 0; j < 4; j++)
            b[j] = splitmix64(&rng) & MASK52;
        b[4] = splitmix64(&rng) & MASK48;
        if (verify_random_quiet(a, b)) rpass++;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    printf("\n--- Iterated chain (%d mul-self) ---\n", CHAIN_ITERS);
    uint64_t bp[5] = { 0x59F2815B16F81ULL, 0x29BFCDB2DCE28ULL,
                        0x55A06295CE870ULL, 0xF9DCBBAC55A06ULL,
                        0x79BE667EF9DCULL };
    uint64_t ri[5], ni[5], ui[5];
    memcpy(ri, bp, 40); memcpy(ni, bp, 40); memcpy(ui, bp, 40);
    for (int i = 0; i < CHAIN_ITERS; i++) {
        uint64_t tmp[5];
        memcpy(tmp, ri, 40); fe_mul_reference(tmp, tmp, ri);
        memcpy(tmp, ni, 40); fe_mul_native(tmp, tmp, ni);
        memcpy(tmp, ui, 40); fe_mul_ucode(tmp, tmp, ui);
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

    printf("=== fe_mul secp256k1: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_secp256k1_mul_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    uint64_t state_a[5] = { 0x59F2815B16F81ULL, 0x29BFCDB2DCE28ULL,
                              0x55A06295CE870ULL, 0xF9DCBBAC55A06ULL,
                              0x79BE667EF9DCULL };
    uint64_t state_b[5] = { 0x3B17D1F2E12C4ULL, 0xCF2546785FD89ULL,
                              0x68B2F9BCCDC68ULL, 0xE8AFBFC23A862ULL,
                              0x4FE13A0540EAULL };

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t tmp_a[5], tmp_b[5];

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, sizeof(tmp_a));
        memcpy(tmp_b, state_b, sizeof(tmp_b));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_native(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Native -O3:  min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, sizeof(tmp_a));
        memcpy(tmp_b, state_b, sizeof(tmp_b));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_ucode(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
