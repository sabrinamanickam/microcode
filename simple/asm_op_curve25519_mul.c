/*
 * asm_op_curve25519_mul.c — fe_mul via microcode (vmwrite) vs native C (-O3)
 *
 * Field: GF(2^255 - 19),  unsaturated radix-2^51,  5 limbs.
 *
 * Multiplication formula (25 cross-products + carry + reduction):
 *   c0 = a0*b0 + a1*(19*b4) + a2*(19*b3) + a3*(19*b2) + a4*(19*b1)
 *   c1 = a0*b1 + a1*b0      + a2*(19*b4) + a3*(19*b3) + a4*(19*b2)
 *   c2 = a0*b2 + a1*b1      + a2*b0      + a3*(19*b4) + a4*(19*b3)
 *   c3 = a0*b3 + a1*b2      + a2*b1      + a3*b0      + a4*(19*b4)
 *   c4 = a0*b4 + a1*b3      + a2*b2      + a3*b1      + a4*b0
 *
 * Patch: 88 triads at U7c00, hooked on vmwrite (0x0cd8).
 * All 25 MACs + carry propagation + reduction in microcode.
 *
 * Register convention (caller → microcode):
 *   RDI=a0  RSI=a1  R12=a2  R11=a3  R14=a4
 *   R15=b0  R13=b1  R9=b2   R10=b3  RBX=b4
 *   RAX=0  R8=0
 *
 * Microcode PREP saves b[0..4] → TMP10..TMP14, then computes:
 *   R13=19*b1  R9=19*b2  R10=19*b3  RBX=19*b4
 *
 * Output: R15=h0  R13=h1  R9=h2  R10=h3  RAX=h4
 *
 * Build:  make PROG=asm_op_curve25519_mul
 * Run:    sudo taskset -c 0 ./asm_op_curve25519_mul_static
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

/* ── fiat-crypto reference (the REAL GCC baseline CryptOpt compares against) ── */
#include "../curvesC/curve25519_mul.c"

static void fe_mul_fiat(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    fiat_curve25519_carry_mul(out, a, b);
}

/* ── microcode patch ──────────────────────────────────────────── */

static void install_fe_mul_patch(void) {
    ucode_t patch[] = {

    /*
     * Register state at entry:
     *   RDI=a0  RSI=a1  R12=a2  R11=a3  R14=a4
     *   R15=b0  R13=b1  R9=b2   R10=b3  RBX=b4
     *   RAX=0   R8=0    RCX=free  RDX=free
     *
     * After PREP:
     *   TMP10=b0  TMP11=b1  TMP12=b2  TMP13=b3  TMP14=b4
     *   R13=19*b1  R9=19*b2  R10=19*b3  RBX=19*b4
     *   R15=b0 (unchanged)  RAX=0  R8=0
     */

    /* ═══ PREP: save b values, compute 19*bi ═══ */
    /* P0 */ { ZEROEXT_DSZ64_DR(TMP10, R15),
               ZEROEXT_DSZ64_DR(TMP11, R13),
               ZEROEXT_DSZ64_DR(TMP12, R9),
               NOP_SEQWORD },
    /* P1 */ { ZEROEXT_DSZ64_DR(TMP13, R10),
               ZEROEXT_DSZ64_DR(TMP14, RBX),
               NOP, NOP_SEQWORD },
    /* P2 */ { MUL_DSZ64_DIR(RCX, 19, R13),       /* R13 = 19*b1 */
               NOP, NOP, NOP_SEQWORD },
    /* P3 */ { MUL_DSZ64_DIR(RCX, 19, R9),        /* R9  = 19*b2 */
               NOP, NOP, NOP_SEQWORD },
    /* P4 */ { MUL_DSZ64_DIR(RCX, 19, R10),       /* R10 = 19*b3 */
               NOP, NOP, NOP_SEQWORD },
    /* P5 */ { MUL_DSZ64_DIR(RCX, 19, RBX),       /* RBX = 19*b4 */
               NOP, NOP, NOP_SEQWORD },

    /* ═══ LIMB c0 = a0*b0 + a1*19b4 + a2*19b3 + a3*19b2 + a4*19b1 ═══ */

    /* init acc=0, copy b0→RDX */
    /* C0-0 */ { ZEROEXT_DSZ64_DR(TMP0, RAX),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),
                 NOP, NOP_SEQWORD },
    /* Product 1: a0 × b0 */
    /* C0-1 */ { MUL_DSZ64_DRR(RCX, RDI, RDX),
                 NOP, NOP, NOP_SEQWORD },
    /* C0-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, RBX),      /* prep 19*b4 → RDX */
                 NOP_SEQWORD },
    /* Product 2: a1 × 19*b4 */
    /* C0-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, RDX),
                 NOP, NOP_SEQWORD },
    /* C0-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 ZEROEXT_DSZ64_DR(RDX, R10),       /* prep 19*b3 → RDX */
                 NOP_SEQWORD },
    /* Product 3: a2 × 19*b3 */
    /* C0-5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, RDX),
                 NOP, NOP_SEQWORD },
    /* C0-6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, R9),        /* prep 19*b2 → RDX */
                 NOP_SEQWORD },
    /* Product 4: a3 × 19*b2 */
    /* C0-7 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, RDX),
                 NOP, NOP_SEQWORD },
    /* C0-8 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* Product 5: a4 × 19*b1 (direct — last use of R13=19*b1) */
    /* C0-9 */ { ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R14, R13),
                 NOP, NOP_SEQWORD },
    /* C0-10 */ { ADD_DSZ64_DRR(TMP2, TMP0, R13),
                  SETCC_CONDB_DR(TMP3, TMP2),
                  NOP, NOP_SEQWORD },
    /* MAC_TAIL_5: carry extraction → R15 = out[0] */
    /* C0-11 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                  ADD_DSZ64_DRR(TMP0, RCX, TMP3),
                  NOP, NOP_SEQWORD },
    /* C0-12 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                  ADD_DSZ64_DRR(TMP1, TMP4, TMP5),
                  ADD_DSZ64_DRR(TMP0, TMP6, TMP0),
                  NOP_SEQWORD },
    /* C0-13 */ { ADD_DSZ64_DRR(R8, R8, TMP7),
                  ADD_DSZ64_DRR(TMP0, TMP1, TMP0),
                  NOP, NOP_SEQWORD },
    /* C0-14 */ { SHR_DSZ64_DRI(R15, TMP9, 13),
                  ADD_DSZ64_DRR(R8, R8, TMP0),
                  NOP, NOP_SEQWORD },

    /* ═══ c0→c1 LINK + LIMB c1 = a0*b1 + a1*b0 + a2*19b4 + a3*19b3 + a4*19b2 ═══ */

    /* LIMB_LINK: combine carry, prep b1→RDX, fire a0×b1 */
    /* C1-0 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 ZEROEXT_DSZ64_DR(RDX, TMP11),
                 NOP_SEQWORD },
    /* C1-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, RDI, RDX),
                 NOP, NOP_SEQWORD },
    /* RESUME + prep b0→RDX */
    /* C1-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),
                 NOP_SEQWORD },
    /* Product 2: a1 × b0 */
    /* C1-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, RDX),
                 NOP, NOP_SEQWORD },
    /* C1-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 ZEROEXT_DSZ64_DR(RDX, RBX),
                 NOP_SEQWORD },
    /* Product 3: a2 × 19*b4 */
    /* C1-5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, RDX),
                 NOP, NOP_SEQWORD },
    /* C1-6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, R10),
                 NOP_SEQWORD },
    /* Product 4: a3 × 19*b3 */
    /* C1-7 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, RDX),
                 NOP, NOP_SEQWORD },
    /* C1-8 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* Product 5: a4 × 19*b2 (direct — last use of R9=19*b2) */
    /* C1-9 */ { ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R14, R9),
                 NOP, NOP_SEQWORD },
    /* C1-10 */ { ADD_DSZ64_DRR(TMP2, TMP0, R9),
                  SETCC_CONDB_DR(TMP3, TMP2),
                  NOP, NOP_SEQWORD },
    /* MAC_TAIL_5 → R13 = out[1] (R13 freed after c0) */
    /* C1-11 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                  ADD_DSZ64_DRR(TMP0, RCX, TMP3),
                  NOP, NOP_SEQWORD },
    /* C1-12 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                  ADD_DSZ64_DRR(TMP1, TMP4, TMP5),
                  ADD_DSZ64_DRR(TMP0, TMP6, TMP0),
                  NOP_SEQWORD },
    /* C1-13 */ { ADD_DSZ64_DRR(R8, R8, TMP7),
                  ADD_DSZ64_DRR(TMP0, TMP1, TMP0),
                  NOP, NOP_SEQWORD },
    /* C1-14 */ { SHR_DSZ64_DRI(R13, TMP9, 13),
                  ADD_DSZ64_DRR(R8, R8, TMP0),
                  NOP, NOP_SEQWORD },

    /* ═══ c1→c2 LINK + LIMB c2 = a0*b2 + a1*b1 + a2*b0 + a3*19b4 + a4*19b3 ═══ */

    /* C2-0 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 ZEROEXT_DSZ64_DR(RDX, TMP12),
                 NOP_SEQWORD },
    /* C2-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, RDI, RDX),
                 NOP, NOP_SEQWORD },
    /* C2-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP11),
                 NOP_SEQWORD },
    /* Product 2: a1 × b1 */
    /* C2-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, RDX),
                 NOP, NOP_SEQWORD },
    /* C2-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),
                 NOP_SEQWORD },
    /* Product 3: a2 × b0 */
    /* C2-5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, RDX),
                 NOP, NOP_SEQWORD },
    /* C2-6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, RBX),
                 NOP_SEQWORD },
    /* Product 4: a3 × 19*b4 */
    /* C2-7 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, RDX),
                 NOP, NOP_SEQWORD },
    /* C2-8 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* Product 5: a4 × 19*b3 (direct — last use of R10=19*b3) */
    /* C2-9 */ { ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R14, R10),
                 NOP, NOP_SEQWORD },
    /* C2-10 */ { ADD_DSZ64_DRR(TMP2, TMP0, R10),
                  SETCC_CONDB_DR(TMP3, TMP2),
                  NOP, NOP_SEQWORD },
    /* MAC_TAIL_5 → R9 = out[2] (R9 freed after c1) */
    /* C2-11 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                  ADD_DSZ64_DRR(TMP0, RCX, TMP3),
                  NOP, NOP_SEQWORD },
    /* C2-12 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                  ADD_DSZ64_DRR(TMP1, TMP4, TMP5),
                  ADD_DSZ64_DRR(TMP0, TMP6, TMP0),
                  NOP_SEQWORD },
    /* C2-13 */ { ADD_DSZ64_DRR(R8, R8, TMP7),
                  ADD_DSZ64_DRR(TMP0, TMP1, TMP0),
                  NOP, NOP_SEQWORD },
    /* C2-14 */ { SHR_DSZ64_DRI(R9, TMP9, 13),
                  ADD_DSZ64_DRR(R8, R8, TMP0),
                  NOP, NOP_SEQWORD },

    /* ═══ c2→c3 LINK + LIMB c3 = a0*b3 + a1*b2 + a2*b1 + a3*b0 + a4*19b4 ═══ */

    /* C3-0 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 ZEROEXT_DSZ64_DR(RDX, TMP13),
                 NOP_SEQWORD },
    /* C3-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, RDI, RDX),
                 NOP, NOP_SEQWORD },
    /* C3-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP12),
                 NOP_SEQWORD },
    /* Product 2: a1 × b2 */
    /* C3-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, RDX),
                 NOP, NOP_SEQWORD },
    /* C3-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 ZEROEXT_DSZ64_DR(RDX, TMP11),
                 NOP_SEQWORD },
    /* Product 3: a2 × b1 */
    /* C3-5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, RDX),
                 NOP, NOP_SEQWORD },
    /* C3-6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),
                 NOP_SEQWORD },
    /* Product 4: a3 × b0 */
    /* C3-7 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, RDX),
                 NOP, NOP_SEQWORD },
    /* C3-8 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* Product 5: a4 × 19*b4 (direct — last use of RBX=19*b4) */
    /* C3-9 */ { ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R14, RBX),
                 NOP, NOP_SEQWORD },
    /* C3-10 */ { ADD_DSZ64_DRR(TMP2, TMP0, RBX),
                  SETCC_CONDB_DR(TMP3, TMP2),
                  NOP, NOP_SEQWORD },
    /* MAC_TAIL_5 → R10 = out[3] (R10 freed after c2) */
    /* C3-11 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                  ADD_DSZ64_DRR(TMP0, RCX, TMP3),
                  NOP, NOP_SEQWORD },
    /* C3-12 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                  ADD_DSZ64_DRR(TMP1, TMP4, TMP5),
                  ADD_DSZ64_DRR(TMP0, TMP6, TMP0),
                  NOP_SEQWORD },
    /* C3-13 */ { ADD_DSZ64_DRR(R8, R8, TMP7),
                  ADD_DSZ64_DRR(TMP0, TMP1, TMP0),
                  NOP, NOP_SEQWORD },
    /* C3-14 */ { SHR_DSZ64_DRI(R10, TMP9, 13),
                  ADD_DSZ64_DRR(R8, R8, TMP0),
                  NOP, NOP_SEQWORD },

    /* ═══ c3→c4 LINK + LIMB c4 = a0*b4 + a1*b3 + a2*b2 + a3*b1 + a4*b0 ═══ */
    /* All b values from TMP10-14, each used for last time → direct srcB */

    /* C4-0 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 NOP, NOP_SEQWORD },
    /* C4-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, RDI, TMP14),  /* a0 × b4 (consumes TMP14) */
                 NOP, NOP_SEQWORD },
    /* C4-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, TMP14),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 NOP, NOP_SEQWORD },
    /* Product 2: a1 × b3 (consumes TMP13) */
    /* C4-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, TMP13),
                 NOP, NOP_SEQWORD },
    /* C4-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, TMP13),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* Product 3: a2 × b2 (consumes TMP12) */
    /* C4-5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, TMP12),
                 NOP, NOP_SEQWORD },
    /* C4-6 */ { ADD_DSZ64_DRR(TMP2, TMP0, TMP12),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 NOP, NOP_SEQWORD },
    /* Product 4: a3 × b1 (consumes TMP11) */
    /* C4-7 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, TMP11),
                 NOP, NOP_SEQWORD },
    /* C4-8 */ { ADD_DSZ64_DRR(TMP0, TMP2, TMP11),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* Product 5: a4 × b0 (consumes TMP10) */
    /* C4-9 */ { ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R14, TMP10),
                 NOP, NOP_SEQWORD },
    /* C4-10 */ { ADD_DSZ64_DRR(TMP2, TMP0, TMP10),
                  SETCC_CONDB_DR(TMP3, TMP2),
                  NOP, NOP_SEQWORD },
    /* MAC_TAIL_5 → RAX = out[4] */
    /* C4-11 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                  ADD_DSZ64_DRR(TMP0, RCX, TMP3),
                  NOP, NOP_SEQWORD },
    /* C4-12 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                  ADD_DSZ64_DRR(TMP1, TMP4, TMP5),
                  ADD_DSZ64_DRR(TMP0, TMP6, TMP0),
                  NOP_SEQWORD },
    /* C4-13 */ { ADD_DSZ64_DRR(R8, R8, TMP7),
                  ADD_DSZ64_DRR(TMP0, TMP1, TMP0),
                  NOP, NOP_SEQWORD },
    /* C4-14 */ { SHR_DSZ64_DRI(RAX, TMP9, 13),
                  ADD_DSZ64_DRR(R8, R8, TMP0),
                  NOP, NOP_SEQWORD },

    /* ═══ FINAL REDUCTION ═══ */
    /* R0 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
               NOP, NOP, NOP_SEQWORD },
    /* R1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
               NOP, NOP, NOP_SEQWORD },
    /* R2 */ { MUL_DSZ64_DIR(TMP1, 19, TMP0),     /* TMP0 = 19*carry */
               NOP, NOP, NOP_SEQWORD },
    /* R3 */ { ADD_DSZ64_DRR(R15, R15, TMP0),
               NOP, NOP, NOP_SEQWORD },
    /* R4 */ { SHR_DSZ64_DRI(TMP0, R15, 51),
               NOP, NOP, NOP_SEQWORD },
    /* R5 */ { SHL_DSZ64_DRI(TMP1, R15, 13),
               ADD_DSZ64_DRR(R13, R13, TMP0),
               NOP, NOP_SEQWORD },
    /* R6 */ { SHR_DSZ64_DRI(R15, TMP1, 13),
               NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("fe_mul patch installed: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
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

        /* load b[0..4] from rbx */
        "mov r15, [rbx]\n\t"
        "mov r13, [rbx + 8]\n\t"
        "mov r9,  [rbx + 16]\n\t"
        "mov r10, [rbx + 24]\n\t"
        "mov rbx, [rbx + 32]\n\t"   /* last — clobbers pointer */

        /* clear accumulators */
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* fire microcode */
        "vmwrite rcx, rdx\n\t"

        /* recover output pointer, store 5 result limbs */
        "pop rcx\n\t"
        "mov [rcx],      r15\n\t"
        "mov [rcx + 8],  r13\n\t"
        "mov [rcx + 16], r9\n\t"
        "mov [rcx + 24], r10\n\t"
        "mov [rcx + 32], rax\n\t"

        : "+r"(_a), "+r"(_b), "+r"(_out)
        :
        : "rax", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14",
          "memory", "cc"
    );
}

/* ── fe_mul native C (compiled with -O3) ──────────────────────── */

static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t a0=a[0], a1=a[1], a2=a[2], a3=a[3], a4=a[4];
    uint64_t b0=b[0], b1=b[1], b2=b[2], b3=b[3], b4=b[4];
    uint64_t r1=19*b1, r2=19*b2, r3=19*b3, r4=19*b4;

    __uint128_t c0 = (__uint128_t)a0*b0 + (__uint128_t)a1*r4
                   + (__uint128_t)a2*r3 + (__uint128_t)a3*r2 + (__uint128_t)a4*r1;
    __uint128_t c1 = (__uint128_t)a0*b1 + (__uint128_t)a1*b0
                   + (__uint128_t)a2*r4 + (__uint128_t)a3*r3 + (__uint128_t)a4*r2;
    __uint128_t c2 = (__uint128_t)a0*b2 + (__uint128_t)a1*b1
                   + (__uint128_t)a2*b0 + (__uint128_t)a3*r4 + (__uint128_t)a4*r3;
    __uint128_t c3 = (__uint128_t)a0*b3 + (__uint128_t)a1*b2
                   + (__uint128_t)a2*b1 + (__uint128_t)a3*b0 + (__uint128_t)a4*r4;
    __uint128_t c4 = (__uint128_t)a0*b4 + (__uint128_t)a1*b3
                   + (__uint128_t)a2*b2 + (__uint128_t)a3*b1 + (__uint128_t)a4*b0;

    uint64_t carry;
    carry = (uint64_t)(c0>>51); out[0] = (uint64_t)c0 & MASK51;
    c1 += carry;
    carry = (uint64_t)(c1>>51); out[1] = (uint64_t)c1 & MASK51;
    c2 += carry;
    carry = (uint64_t)(c2>>51); out[2] = (uint64_t)c2 & MASK51;
    c3 += carry;
    carry = (uint64_t)(c3>>51); out[3] = (uint64_t)c3 & MASK51;
    c4 += carry;
    carry = (uint64_t)(c4>>51); out[4] = (uint64_t)c4 & MASK51;
    out[0] += carry * 19;
    carry = out[0] >> 51; out[0] &= MASK51;
    out[1] += carry;
}

/* ── independent reference (naive schoolbook multiply, no optimisations) ── */

static void fe_mul_reference(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    __uint128_t t[9] = {0};
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++)
            t[i + j] += (__uint128_t)a[i] * b[j];
    /* 2^(51*5) = 2^255 ≡ 19 (mod p) */
    for (int i = 5; i <= 8; i++)
        t[i - 5] += t[i] * 19;
    __uint128_t carry;
    carry = t[0] >> 51; out[0] = (uint64_t)t[0] & MASK51;
    t[1] += carry;
    carry = t[1] >> 51; out[1] = (uint64_t)t[1] & MASK51;
    t[2] += carry;
    carry = t[2] >> 51; out[2] = (uint64_t)t[2] & MASK51;
    t[3] += carry;
    carry = t[3] >> 51; out[3] = (uint64_t)t[3] & MASK51;
    t[4] += carry;
    carry = t[4] >> 51; out[4] = (uint64_t)t[4] & MASK51;
    out[0] += (uint64_t)carry * 19;
    carry = out[0] >> 51; out[0] &= MASK51;
    out[1] += (uint64_t)carry;
}

/* ── verification ────────────────────────────────────────────── */

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

/*
 * Hand-verified expected outputs:
 *   0 * 0 = 0
 *   1 * 1 = 1
 *   0 * 1 = 0
 *   2 * 3 = 6
 *   9 * 9 = 81
 *   (2^51) * (2^51) = 2^102  → limb 2 = 1
 *   (2^204) * (2^51) = 2^255 ≡ 19  → limb 0 = 19
 *   1 * x = x   (identity)
 */
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
    { "2^51*2^51",
      {0,1,0,0,0}, {0,1,0,0,0}, {0,0,1,0,0}, 1 },
    { "2^204*2^51",
      {0,0,0,0,1}, {0,1,0,0,0}, {19,0,0,0,0}, 1 },
    { "1*basepoint",
      {1,0,0,0,0},
      {0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
       0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
       0x000216936D3CD6E5ULL},
      {0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
       0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
       0x000216936D3CD6E5ULL}, 1 },
    { "bp*bp",
      {0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
       0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
       0x000216936D3CD6E5ULL},
      {0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
       0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
       0x000216936D3CD6E5ULL},
      {0}, 0 },
    { "all_max*all_max",
      {MASK51,MASK51,MASK51,MASK51,MASK51},
      {MASK51,MASK51,MASK51,MASK51,MASK51},
      {0}, 0 },
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[5], nat[5], ucd[5];
    fe_mul_reference(t->a, t->b, ref);
    fe_mul_native(t->a, t->b, nat);
    fe_mul_ucode(t->a, t->b, ucd);

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
    for (int i = 0; i < 5; i++) {
        if (ucd[i] >> 52) {
            printf("  FAIL [%s] limb %d overflow: %016" PRIx64 "\n",
                   t->label, i, ucd[i]);
            ok = 0;
        }
    }
    if (ok) printf("  PASS [%s]\n", t->label);
    return ok;
}

static int verify_random_quiet(const uint64_t a[5], const uint64_t b[5]) {
    uint64_t ref[5], nat[5], ucd[5];
    fe_mul_reference(a, b, ref);
    fe_mul_native(a, b, nat);
    fe_mul_ucode(a, b, ucd);
    if (memcmp(ref, nat, 40) != 0 || memcmp(ref, ucd, 40) != 0) {
        printf("  FAIL random: a={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 ",%016" PRIx64 "}\n",
               a[0], a[1], a[2], a[3], a[4]);
        printf("           b={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 ",%016" PRIx64 "}\n",
               b[0], b[1], b[2], b[3], b[4]);
        if (memcmp(ref, nat, 40) != 0) {
            printf("    native mismatch:");
            for (int i = 0; i < 5; i++) printf(" %016" PRIx64, nat[i]);
            printf("\n");
        }
        if (memcmp(ref, ucd, 40) != 0) {
            printf("    ucode  mismatch:");
            for (int i = 0; i < 5; i++) printf(" %016" PRIx64, ucd[i]);
            printf("\n");
        }
        printf("    reference:      ");
        for (int i = 0; i < 5; i++) printf(" %016" PRIx64, ref[i]);
        printf("\n");
        return 0;
    }
    return 1;
}

#define RANDOM_TESTS 10000
#define CHAIN_ITERS  1000

static int verify_all(void) {
    int pass = 0, fail = 0;

    /* ── known test vectors ──────────────────────────────────── */
    printf("--- Known test vectors ---\n");
    for (int i = 0; i < (int)N_VECS; i++) {
        if (verify_one(&test_vectors[i])) pass++; else fail++;
    }

    /* ── random stress test ──────────────────────────────────── */
    printf("\n--- Random stress test (%d vectors) ---\n", RANDOM_TESTS);
    uint64_t rng = 0xDEADBEEFCAFE1234ULL;
    int rpass = 0;
    for (int i = 0; i < RANDOM_TESTS; i++) {
        uint64_t a[5], b[5];
        for (int j = 0; j < 5; j++)
            a[j] = splitmix64(&rng) & MASK51;
        for (int j = 0; j < 5; j++)
            b[j] = splitmix64(&rng) & MASK51;
        if (verify_random_quiet(a, b)) rpass++;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    /* ── iterated chain: mul(x,x) == sq(x), compare ref vs native vs ucode ── */
    printf("\n--- Iterated chain (%d mul-self from basepoint) ---\n", CHAIN_ITERS);
    uint64_t bp[5] = { 0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                        0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                        0x000216936D3CD6E5ULL };
    uint64_t ri[5], ni[5], ui[5];
    memcpy(ri, bp, 40); memcpy(ni, bp, 40); memcpy(ui, bp, 40);
    for (int i = 0; i < CHAIN_ITERS; i++) {
        uint64_t tmp[5];
        memcpy(tmp, ri, 40); fe_mul_reference(tmp, tmp, ri);
        memcpy(tmp, ni, 40); fe_mul_native(tmp, tmp, ni);
        memcpy(tmp, ui, 40); fe_mul_ucode(tmp, tmp, ui);
    }
    int ref_nat = memcmp(ri, ni, 40) == 0;
    int ref_ucd = memcmp(ri, ui, 40) == 0;
    printf("  ref==native: %s   ref==ucode: %s   → %s\n",
           ref_nat ? "yes" : "NO", ref_ucd ? "yes" : "NO",
           (ref_nat && ref_ucd) ? "PASS" : "FAIL");
    if (ref_nat && ref_ucd) {
        pass++;
    } else {
        fail++;
        printf("  reference:");
        for (int i = 0; i < 5; i++) printf(" %016" PRIx64, ri[i]);
        printf("\n  native:   ");
        for (int i = 0; i < 5; i++) printf(" %016" PRIx64, ni[i]);
        printf("\n  ucode:    ");
        for (int i = 0; i < 5; i++) printf(" %016" PRIx64, ui[i]);
        printf("\n");
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

    printf("=== fe_mul: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_fe_mul_patch();

    /* ── correctness ──────────────────────────────────────────── */
    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    uint64_t state_a[5] = { 0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                             0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                             0x000216936D3CD6E5ULL };
    uint64_t state_b[5] = { 0x0006B17D1F2E12C4ULL, 0x000CF2546785FD89ULL,
                             0x00068B2F9BCCDC68ULL, 0x000E8AFBFC23A862ULL,
                             0x00014FE13A0540EAULL };

    /* ── benchmark ────────────────────────────────────────────── */
    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t tmp_a[5], tmp_b[5];

    /* native C -O3 */
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
    printf("Naive -O3:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* fiat-crypto (the real GCC baseline CryptOpt compares against) */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, sizeof(tmp_a));
        memcpy(tmp_b, state_b, sizeof(tmp_b));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_fiat(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Fiat-crypto: min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* microcode */
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

    /* clean up */
    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
