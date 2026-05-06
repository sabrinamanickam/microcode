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
 * Patch: 73 triads (was 88). Optimized using p521_sq's progressive carry
 * accumulation pattern: per-MAC carry bits flow into TMP9 inside the MAC
 * triad's free slot (instead of being saved to separate TMPs and summed
 * at the end), and per-MAC hi values flow into R8 the same way.
 *
 * Register convention (caller → microcode):
 *   RDI=a0  RSI=a1  R12=a2  R11=a3  R14=a4
 *   R15=b0  R13=b1  R9=b2   R10=b3  RBX=b4
 *   RAX=0  R8=0
 *
 * Microcode PREP: saves b[0..4] → TMP10..TMP14, then in-place computes
 *   R13=19*b1  R9=19*b2  R10=19*b3  RBX=19*b4
 * Initializes TMP0=0 (lo accumulator) and seeds RDX=b0 for c0's MAC1.
 *
 * Per-limb carry chain (TMP8 = 13-bit lo carry, TMP1 = hi carry shifted by 13):
 *   limb K's T9 slot 2: SHR TMP8 = TMP0 >> 51
 *   limb K's T11 slot 2: SHL TMP1 = R8 << 13
 *   limb K+1's T0 slot 0: OR TMP0 = TMP8 | TMP1   (link)
 *
 * Output: R15=h0  R13=h1  R9=h2  R10=h3  RAX=h4   (all masked to 51 bits;
 * R13 may be lazily ≤ 52 bits after final fold's carry propagation)
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
     * 73-triad version (was 88) using p521_sq's progressive accumulation:
     *   R8  accumulates per-MAC hi values inside the MAC triad's slot 0
     *   TMP9 accumulates per-MAC SETCC carry bits in the MAC triad's slot 2
     *   TMP1 = hi carry to next limb (R8 << 13, computed in T11)
     *   TMP8 = lo carry to next limb (TMP0 >> 51, computed in T9)
     *   TMP2 = mask scratch (TMP0 << 13, computed in T10)
     *
     * Per-limb (13 triads, c4 omits T12):
     *   T0  LINK or NOP: OR TMP0=link, MUL_MAC1, NOTAND R8=0
     *   T1  ACC1+prep:   ADD TMP0+=lo, SETCC, prep RDX
     *   T2  hi+MAC2+init: ADD R8+=hi, MUL_MAC2, ZEROEXT TMP9=TMP15
     *   T3..T7  alternating ACC+prep / hi+MAC+carry-sum
     *   T8  hi+MAC5+carry
     *   T9  lastACC+SHR_TMP8: ADD TMP0+=lo, SETCC, SHR TMP8=TMP0>>51
     *   T10 last_hi+last_carry+mask_prep: ADD R8+=hi, ADD TMP9+=carry, SHL TMP2=TMP0<<13
     *   T11 finalize+output+hi_carry: ADD R8+=TMP9, SHR out=TMP2>>13, SHL TMP1=R8<<13
     *   T12 prep RDX for next limb's MAC1   (omitted on c4)
     *
     * Register state at entry:
     *   RDI=a0  RSI=a1  R12=a2  R11=a3  R14=a4
     *   R15=b0  R13=b1  R9=b2   R10=b3  RBX=b4   RAX=0  R8=0
     * After PREP:
     *   TMP10..14 = b0..b4 (preserved across patch)
     *   R13=19*b1, R9=19*b2, R10=19*b3, RBX=19*b4
     *   RDX = b0, TMP0 = 0 (lo accumulator init)
     */

    /* ═══ PREP (6 triads) ═══ */
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
               ZEROEXT_DSZ64_DR(RDX, TMP10),       /* prep b0 for c0's MAC1 */
               NOTAND_DSZ64_DRR(TMP0, TMP0, TMP0), /* TMP0 = 0 (acc init) */
               NOP_SEQWORD },

    /* ═══ c0 = a0*b0 + a1*19b4 + a2*19b3 + a3*19b2 + a4*19b1 (13 triads) ═══ */

    /* T0  MAC1: a0×b0. RDX=b0 from PREP; TMP0=0; R8=0. */
    { NOP, MUL_DSZ64_DRR(RCX, RDI, RDX), NOP, NOP_SEQWORD },
    /* T1  ACC1, SETCC, prep RDX=19*b4 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    /* T2  hi+=hi, MAC2: a1×19b4, init carry sum */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    /* T3  ACC2, SETCC, prep RDX=19*b3 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    /* T4  hi+=hi, MAC3: a2×19b3, TMP9+=carry */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T5  ACC3, SETCC, prep RDX=19*b2 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R9), NOP_SEQWORD },
    /* T6  hi+=hi, MAC4: a3×19b2, TMP9+=carry */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T7  ACC4, SETCC, prep RDX=19*b1 (R13, last use) */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R13), NOP_SEQWORD },
    /* T8  hi+=hi, MAC5: a4×19b1, TMP9+=carry */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T9  lastACC, SETCC, SHR TMP8=TMP0>>51 (slot 0→2 RAW) */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    /* T10 last hi, last carry, mask prep TMP2=TMP0<<13 */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    /* T11 R8+=carry_sum, output R15=TMP2>>13 (masked), TMP1=R8<<13 (slot 0→2 RAW) */
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHR_DSZ64_DRI(R15, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    /* T12 prep RDX = b1 for c1's MAC1 */
    { ZEROEXT_DSZ64_DR(RDX, TMP11), NOP, NOP, NOP_SEQWORD },

    /* ═══ c1 = a0*b1 + a1*b0 + a2*19b4 + a3*19b3 + a4*19b2 (13 triads) ═══ */

    /* T0  LINK: TMP0 = TMP8|TMP1, MAC1: a0×b1, reset R8 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, RDI, RDX),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    /* T1  ACC1, SETCC, prep RDX=b0 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    /* T2  hi+=hi, MAC2: a1×b0, init carry sum */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    /* T3  ACC2, SETCC, prep RDX=19*b4 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    /* T4  hi+=hi, MAC3: a2×19b4 */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T5  ACC3, SETCC, prep RDX=19*b3 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    /* T6  hi+=hi, MAC4: a3×19b3 */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T7  ACC4, SETCC, prep RDX=19*b2 (R9, last use) */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R9), NOP_SEQWORD },
    /* T8  hi+=hi, MAC5: a4×19b2 */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T9  lastACC, SETCC, SHR TMP8 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    /* T10 last hi, last carry, mask prep */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    /* T11 R8+=carry_sum, output R13 (freed after c0), TMP1=R8<<13 */
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHR_DSZ64_DRI(R13, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    /* T12 prep RDX = b2 for c2's MAC1 */
    { ZEROEXT_DSZ64_DR(RDX, TMP12), NOP, NOP, NOP_SEQWORD },

    /* ═══ c2 = a0*b2 + a1*b1 + a2*b0 + a3*19b4 + a4*19b3 (13 triads) ═══ */

    /* T0  LINK, MAC1: a0×b2, reset R8 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, RDI, RDX),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    /* T1  ACC1, SETCC, prep RDX=b1 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    /* T2  hi+=hi, MAC2: a1×b1, init carry sum */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    /* T3  ACC2, SETCC, prep RDX=b0 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    /* T4  hi+=hi, MAC3: a2×b0 */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T5  ACC3, SETCC, prep RDX=19*b4 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    /* T6  hi+=hi, MAC4: a3×19b4 */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T7  ACC4, SETCC, prep RDX=19*b3 (R10, last use) */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    /* T8  hi+=hi, MAC5: a4×19b3 */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T9  lastACC, SETCC, SHR TMP8 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    /* T10 last hi, last carry, mask prep */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    /* T11 output R9 (freed after c1) */
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHR_DSZ64_DRI(R9, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    /* T12 prep RDX = b3 for c3's MAC1 */
    { ZEROEXT_DSZ64_DR(RDX, TMP13), NOP, NOP, NOP_SEQWORD },

    /* ═══ c3 = a0*b3 + a1*b2 + a2*b1 + a3*b0 + a4*19b4 (13 triads) ═══ */

    /* T0  LINK, MAC1: a0×b3, reset R8 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, RDI, RDX),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    /* T1  ACC1, SETCC, prep RDX=b2 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP12), NOP_SEQWORD },
    /* T2  hi+=hi, MAC2: a1×b2, init carry sum */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    /* T3  ACC2, SETCC, prep RDX=b1 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    /* T4  hi+=hi, MAC3: a2×b1 */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T5  ACC3, SETCC, prep RDX=b0 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    /* T6  hi+=hi, MAC4: a3×b0 */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T7  ACC4, SETCC, prep RDX=19*b4 (RBX, last use) */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    /* T8  hi+=hi, MAC5: a4×19b4 */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T9  lastACC, SETCC, SHR TMP8 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    /* T10 last hi, last carry, mask prep */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    /* T11 output R10 (freed after c2) */
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHR_DSZ64_DRI(R10, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    /* T12 prep RDX = b4 for c4's MAC1 */
    { ZEROEXT_DSZ64_DR(RDX, TMP14), NOP, NOP, NOP_SEQWORD },

    /* ═══ c4 = a0*b4 + a1*b3 + a2*b2 + a3*b1 + a4*b0 (12 triads, no T12) ═══ */

    /* T0  LINK, MAC1: a0×b4, reset R8 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, RDI, RDX),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    /* T1  ACC1, SETCC, prep RDX=b3 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP13), NOP_SEQWORD },
    /* T2  hi+=hi, MAC2: a1×b3, init carry sum */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    /* T3  ACC2, SETCC, prep RDX=b2 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP12), NOP_SEQWORD },
    /* T4  hi+=hi, MAC3: a2×b2 */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T5  ACC3, SETCC, prep RDX=b1 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    /* T6  hi+=hi, MAC4: a3×b1 */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T7  ACC4, SETCC, prep RDX=b0 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    /* T8  hi+=hi, MAC5: a4×b0 */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    /* T9  lastACC, SETCC, SHR TMP8 */
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    /* T10 last hi, last carry, mask prep */
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    /* T11 output → RAX, TMP1 = R8<<13 (final hi carry, used in F0) */
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHR_DSZ64_DRI(RAX, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* ═══ FINAL REDUCTION (3 triads) ═══
     * full_carry = TMP8 | TMP1 (lo | hi)
     * R15 += 19*full_carry; propagate overflow to R13; mask R15. */

    /* F0: TMP0 = full carry (slot 0); MUL TMP0 = 19*TMP0 (slot 1, 0→1 RAW);
     *     R15 += TMP0 (slot 2 reads new TMP0 = 19*carry, 1→2 RAW). */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DIR(TMP2, 19, TMP0),
      ADD_DSZ64_DRR(R15, R15, TMP0), NOP_SEQWORD },
    /* F1: extract overflow from R15, propagate to R13 (lazy: R13 may be ≤52 bits) */
    { SHR_DSZ64_DRI(TMP0, R15, 51),
      ADD_DSZ64_DRR(R13, R13, TMP0),
      NOP, NOP_SEQWORD },
    /* F2: mask R15 to 51 bits (SHL+SHR pair) */
    { SHL_DSZ64_DRI(TMP2, R15, 13),
      SHR_DSZ64_DRI(R15, TMP2, 13),
      NOP, END_SEQWORD }

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
