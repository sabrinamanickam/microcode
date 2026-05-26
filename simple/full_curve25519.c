/*
 * full_curve25519.c — Complete X25519 Diffie-Hellman key exchange
 *
 * Implements the full Montgomery ladder (RFC 7748) using:
 *   - Native C field arithmetic (-O3, __uint128_t)
 *   - Microcode-accelerated field arithmetic (fe_mul + fe_sq via vmwrite/vmread)
 *
 * Field: GF(2^255 - 19), unsaturated radix-2^51, 5 limbs.
 *
 * Two patches are installed simultaneously:
 *   fe_mul (66 triads) at U7c00, fires on vmwrite (opcode 0x0cd8)
 *   fe_sq  (42 triads) at U7d08, fires on vmread  (opcode 0x0618, byte: 0f 78 ca)
 * Total patch RAM: 108 triads (under 128 limit).
 *
 * Build:  make PROG=full_curve25519
 * Run:    sudo taskset -c 0 ./full_curve25519_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <emmintrin.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* ── field element type ──────────────────────────────────────────── */

typedef uint64_t fe[5];   /* 5 limbs x 51 bits */
#define MASK51 0x7FFFFFFFFFFFFULL

/* ════════════════════════════════════════════════════════════════════
 * MICROCODE PATCH INSTALLATION
 *
 * Patch contents are verbatim from asm_op_curve25519_mul.c and
 * asm_op_curve25519.c (the standalone benchmark files).
 * ════════════════════════════════════════════════════════════════════ */

static void install_field_patches(void) {
    /* ─────────────────────────────────────────────────────────────────
     * fe_mul patch (66 triads, hooked to vmwrite at U7c00).
     *
     * Caller convention (set up in fe_mul_ucode inline asm):
     *   RDI=a0  RSI=a1  R12=a2  R11=a3  R14=a4
     *   R15=b0  R13=b1  R9=b2   R10=b3  RBX=b4   RAX=0  R8=0
     * Output: R15=h0  R13=h1  R9=h2  R10=h3  RAX=h4
     *
     * The TMP9 carry chain (per-c-section "TMP9 += TMP15 ... R8 += TMP9")
     * runs in parallel with the R8 hi-accumulator chain. Collapsing the two
     * chains into a single R8 chain (as a 65-triad shrink attempted) saves
     * a triad but lengthens R8's critical path enough to add ~5k cyc to
     * X25519. Keep the two-chain structure.
     * ───────────────────────────────────────────────────────────────── */
    ucode_t mul_patch[] = {

    /* ═══ PREP (3 triads — 3 MULs/triad via slot-WAW) ═══ */
    { ZEROEXT_DSZ64_DR(TMP10, R15),
      ZEROEXT_DSZ64_DR(TMP11, R13),
      ZEROEXT_DSZ64_DR(TMP12, R9), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R10),
      ZEROEXT_DSZ64_DR(TMP14, RBX),
      MUL_DSZ64_DIR(RCX, 19, R13), NOP_SEQWORD },
    { MUL_DSZ64_DIR(RCX, 19, R9),
      MUL_DSZ64_DIR(RCX, 19, R10),
      MUL_DSZ64_DIR(RCX, 19, RBX), NOP_SEQWORD },

    /* ═══ c0 = a0*b0 + a1*19b4 + a2*19b3 + a3*19b2 + a4*19b1 (12 triads) ═══ */
    { ZEROEXT_DSZ64_DR(RDX, TMP10),
      MUL_DSZ64_DRR(RCX, RDI, RDX),
      NOTAND_DSZ64_DRR(TMP0, TMP0, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX),
      MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHR_DSZ64_DRI(R15, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* ═══ c1 = a0*b1 + a1*b0 + a2*19b4 + a3*19b3 + a4*19b2 (12 triads) ═══ */
    { ZEROEXT_DSZ64_DR(RDX, TMP11),
      MUL_DSZ64_DRR(RCX, RDI, RDX),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX),
      MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHR_DSZ64_DRI(R13, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* ═══ c2 = a0*b2 + a1*b1 + a2*b0 + a3*19b4 + a4*19b3 (12 triads) ═══ */
    { ZEROEXT_DSZ64_DR(RDX, TMP12),
      MUL_DSZ64_DRR(RCX, RDI, RDX),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX),
      MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHR_DSZ64_DRI(R9, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* ═══ c3 = a0*b3 + a1*b2 + a2*b1 + a3*b0 + a4*19b4 (12 triads) ═══ */
    { ZEROEXT_DSZ64_DR(RDX, TMP13),
      MUL_DSZ64_DRR(RCX, RDI, RDX),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP12), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX),
      MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHR_DSZ64_DRI(R10, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* ═══ c4 = a0*b4 + a1*b3 + a2*b2 + a3*b1 + a4*b0 (12 triads, last limb) ═══ */
    { ZEROEXT_DSZ64_DR(RDX, TMP14),
      MUL_DSZ64_DRR(RCX, RDI, RDX),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP13), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX),
      MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP12), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHR_DSZ64_DRI(RAX, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* ═══ FINAL REDUCTION (3 triads): R15 += 19*carry, propagate, mask ═══ */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DIR(TMP2, 19, TMP0),
      ADD_DSZ64_DRR(R15, R15, TMP0), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, R15, 51),
      ADD_DSZ64_DRR(R13, R13, TMP0),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP2, R15, 13),
      SHR_DSZ64_DRI(R15, TMP2, 13),
      NOP, END_SEQWORD }
    };

    /* ─────────────────────────────────────────────────────────────────
     * fe_sq patch (42 triads, hooked to vmread at U7d08).
     *
     * Caller convention (set up in fe_sq_ucode inline asm):
     *   RDI=a0  RSI=a1  R12=a2  R11=a3  R14=a4
     *   R15=2*a0  R13=2*a1  R9=2*a2  R10=2*a3
     *   RBX=19*a4  RDX=19*a3   RAX=0  R8=0
     * Output: RDI=h0  R9=h1  R10=h2  RBX=h3  RAX=h4
     * ───────────────────────────────────────────────────────────────── */
    ucode_t sq_patch[] = {

    /* ═══ c0 = a0*a0 + d1*r4 + d2*r3 (8 triads) ═══ */
    { ZEROEXT_DSZ64_DR(TMP0, RAX),
      MUL_DSZ64_DRR(RCX, RDI, RDI),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDI),
      SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP9, TMP15),
      MUL_DSZ64_DRR(RCX, RBX, R13),
      ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RDX, R9),
      ADD_DSZ64_DRR(TMP0, TMP0, R9),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(RDI, TMP6, 13),
      ZEROEXT_DSZ64_DR(R13, RSI),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },

    /* ═══ c1 = d0*a1 + r3*a3 + d2*r4 (8 triads) ═══ */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, R13),
      ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(R9, R12, R12), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP9, TMP15),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RBX, R9),
      ADD_DSZ64_DRR(TMP0, TMP0, R9),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(R9, TMP6, 13),
      ZEROEXT_DSZ64_DR(RDX, R12),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },

    /* ═══ c2 = d0*a2 + a1*a1 + d3*r4 (8 triads) ═══ */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX),
      ZEROEXT_DSZ64_DR(R13, RSI), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP9, TMP15),
      MUL_DSZ64_DRR(RCX, RSI, R13),
      ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RBX, R10),
      ADD_DSZ64_DRR(TMP0, TMP0, R10),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(R10, TMP6, 13),
      ZEROEXT_DSZ64_DR(RDX, R11),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },

    /* ═══ c3 = d0*a3 + d1*a2 + r4*a4 (8 triads) ═══ */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, RDX),
      ADD_DSZ64_DRR(R13, RSI, RSI), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RAX, R12),
      MUL_DSZ64_DRR(RCX, R13, RAX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RAX),
      SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, R14, RBX),
      ADD_DSZ64_DRR(TMP0, TMP0, RBX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9),
      SHR_DSZ64_DRI(TMP8, TMP0, 51),
      SHL_DSZ64_DRI(TMP6, TMP0, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(RBX, TMP6, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },

    /* ═══ c4 = d0*a4 + d1*a3 + a2*a2 (7 triads — last limb) ═══ */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, R14),
      ADD_DSZ64_DRR(TMP0, TMP0, R14), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, R13, R11),
      ADD_DSZ64_DRR(TMP0, TMP0, R11),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      MUL_DSZ64_DRR(RCX, R12, R12), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, R12),
      SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 51),
      ADD_DSZ64_DRR(R8, R8, TMP9), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13),
      SHR_DSZ64_DRI(RAX, TMP6, 13), NOP_SEQWORD },

    /* ═══ FINAL REDUCTION (3 triads): RDI += 19*carry, propagate, mask ═══ */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DIR(TMP6, 19, TMP0),
      ADD_DSZ64_DRR(RDI, RDI, TMP0), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, RDI, 51),
      ADD_DSZ64_DRR(R9, R9, TMP0),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP6, RDI, 13),
      SHR_DSZ64_DRI(RDI, TMP6, 13),
      NOP, END_SEQWORD }
    };

    /* Install fe_mul at U7c00, hooked to vmwrite (slot 0). */
    patch_ucode(0x7c00, mul_patch, ARRAY_SZ(mul_patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);

    /* Install fe_sq immediately after, hooked to vmread (slot 1). */
    uint64_t sq_addr = 0x7c00 + ARRAY_SZ(mul_patch) * 4;
    patch_ucode(sq_addr, sq_patch, ARRAY_SZ(sq_patch));
    hook_match_and_patch(1, 0x0618, sq_addr);

    printf("fe_mul: %d triads at U%04lx (vmwrite hook)\n",
           (int)ARRAY_SZ(mul_patch), (unsigned long)0x7c00);
    printf("fe_sq : %d triads at U%04lx (vmread  hook)\n",
           (int)ARRAY_SZ(sq_patch),  (unsigned long)sq_addr);
}

/* ════════════════════════════════════════════════════════════════════
 * MICROCODE FIELD OPERATIONS
 * ════════════════════════════════════════════════════════════════════ */

/* Optimization notes:
 *   1. Use SysV-ABI arg registers (rdi, rsi, rdx) directly via constraint
 *      letters "D", "S", "d" — saves 3 reg-reg moves per call that the
 *      previous `register asm("rcx")` etc. pinning required GCC to emit.
 *   2. Stash the output pointer in callee-saved rbp instead of pinned r15.
 *      r15 was clobbered by the patch (output limb h0), forcing an inner
 *      `push r15 / pop rcx` to preserve the out-ptr across vmwrite/vmread.
 *      rbp survives the patch untouched, so the inner push/pop disappears
 *      — GCC handles save/restore of rbp once per call in the prologue/
 *      epilogue (same cost as the previous push r15 in the prologue).
 *
 * Net per-call savings: ~5–7 cycles (3 reg-reg moves elided + 2 stack
 * accesses eliminated). At ~2300 calls per X25519, that's ~11–16k cycles
 * off the total runtime. Pure wrapper work — the microcode patches and
 * operand semantics are unchanged.
 *
 * Non-static so the amd64-51-ucode hybrid can call directly. */

void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    /* Pin args to their natural SysV-ABI registers — GCC won't emit
     * extra reg-reg moves to relocate them. "+r" tells GCC the regs are
     * both input and clobbered (no need to also list in clobber set). */
    register const uint64_t *_a   asm("rdi") = a;
    register const uint64_t *_b   asm("rsi") = b;
    register       uint64_t *_out asm("rdx") = out;

    asm volatile(
        /* Stash out pointer in callee-saved rbp; survives the patch.
         * Avoids the old inner `push r15 / pop rcx` pair. */
        "mov rbp, rdx\n\t"

        /* Load b[0..4] from rsi (rsi unchanged through these). */
        "mov r15, [rsi]\n\t"
        "mov r13, [rsi + 8]\n\t"
        "mov r9,  [rsi + 16]\n\t"
        "mov r10, [rsi + 24]\n\t"
        "mov rbx, [rsi + 32]\n\t"

        /* Load a[1..4] then a[0] last (a[0] destroys rdi's pointer). */
        "mov rsi, [rdi + 8]\n\t"
        "mov r12, [rdi + 16]\n\t"
        "mov r11, [rdi + 24]\n\t"
        "mov r14, [rdi + 32]\n\t"
        "mov rdi, [rdi]\n\t"

        /* Clear accumulators */
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* Fire fe_mul microcode via vmwrite */
        "vmwrite rcx, rdx\n\t"

        /* Store 5 result limbs via rbp */
        "mov [rbp],      r15\n\t"
        "mov [rbp + 8],  r13\n\t"
        "mov [rbp + 16], r9\n\t"
        "mov [rbp + 24], r10\n\t"
        "mov [rbp + 32], rax\n\t"

        : "+r"(_a), "+r"(_b), "+r"(_out)
        :
        : "rax", "rbx", "rcx", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );
}

void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    register const uint64_t *_a   asm("rdi") = a;
    register       uint64_t *_out asm("rsi") = out;

    asm volatile(
        /* Stash out pointer in callee-saved rbp; survives the patch. */
        "mov rbp, rsi\n\t"

        /* Load a[1..4] then a[0] last (a[0] destroys rdi's pointer). */
        "mov r14, [rdi + 32]\n\t"
        "mov r11, [rdi + 24]\n\t"
        "mov r12, [rdi + 16]\n\t"
        "mov rsi, [rdi + 8]\n\t"
        "mov rdi, [rdi]\n\t"

        /* Precompute doubled (2*a_i) and reduced (19*a_i) operands */
        "lea r15, [rdi + rdi]\n\t"
        "lea r13, [rsi + rsi]\n\t"
        "lea r9,  [r12 + r12]\n\t"
        "lea r10, [r11 + r11]\n\t"
        "imul rbx, r14, 19\n\t"
        "imul rdx, r11, 19\n\t"

        /* Clear accumulators */
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* Fire fe_sq microcode via vmread (opcode: 0f 78 ca) */
        ".byte 0x0f, 0x78, 0xca\n\t"

        /* Store 5 result limbs via rbp */
        "mov [rbp],      rdi\n\t"
        "mov [rbp + 8],  r9\n\t"
        "mov [rbp + 16], r10\n\t"
        "mov [rbp + 24], rbx\n\t"
        "mov [rbp + 32], rax\n\t"

        : "+r"(_a), "+r"(_out)
        :
        : "rax", "rbx", "rcx", "rdx", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );
}

/* fe_sq_ucode_n: square `a` `n` times in place (out = a^(2^n)).
 *
 * Keeps the running result in arch registers across iterations to
 * skip the memory store/load and the precompute-from-memory cost each
 * iteration. Saves ~10 cyc/iter vs n separate fe_sq_ucode calls.
 *
 * After each patch fire, the output lives in (rdi, r9, r10, rbx, rax)
 * = (h0..h4). The next iter wants inputs in (rdi, rsi, r12, r11, r14).
 * Only rdi is already in place; 4 reg-moves stage the next iter.
 *
 * n must be >= 1. */
static void fe_sq_ucode_n(uint64_t *out, const uint64_t *a, int n) {
    register const uint64_t *_a   asm("rdi") = a;
    register       uint64_t *_out asm("rsi") = out;
    register int             _n   asm("edx") = n;

    asm volatile(
        /* Stash out ptr and counter on stack so they survive the
         * patch's clobbering of every GP register. */
        "sub rsp, 16\n\t"
        "mov [rsp],   rsi\n\t"     /* save out ptr */
        "mov [rsp+8], rdx\n\t"     /* save loop counter */

        /* Load a[1..4] then a[0] (a[0] destroys rdi). */
        "mov r14, [rdi + 32]\n\t"
        "mov r11, [rdi + 24]\n\t"
        "mov r12, [rdi + 16]\n\t"
        "mov rsi, [rdi + 8]\n\t"
        "mov rdi, [rdi]\n\t"

        "Lsqn%=:\n\t"
        /* Precompute 2*a_i and 19*a_i for the patch */
        "lea r15, [rdi + rdi]\n\t"
        "lea r13, [rsi + rsi]\n\t"
        "lea r9,  [r12 + r12]\n\t"
        "lea r10, [r11 + r11]\n\t"
        "imul rbx, r14, 19\n\t"
        "imul rdx, r11, 19\n\t"
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* Fire fe_sq (vmread, 0f 78 ca) */
        ".byte 0x0f, 0x78, 0xca\n\t"

        /* After patch: rdi=h0, r9=h1, r10=h2, rbx=h3, rax=h4 */
        /* Stage h[1..4] into the input regs for next iter.
         * (rdi already has h0; need rsi=h1, r12=h2, r11=h3, r14=h4) */
        "mov rsi, r9\n\t"
        "mov r12, r10\n\t"
        "mov r11, rbx\n\t"
        "mov r14, rax\n\t"

        "dec qword ptr [rsp+8]\n\t"
        "jnz Lsqn%=\n\t"

        /* Final result is now in rdi/rsi/r12/r11/r14 (h0..h4). */
        "mov rbp, [rsp]\n\t"
        "mov [rbp],      rdi\n\t"
        "mov [rbp + 8],  rsi\n\t"
        "mov [rbp + 16], r12\n\t"
        "mov [rbp + 24], r11\n\t"
        "mov [rbp + 32], r14\n\t"

        "add rsp, 16\n\t"

        : "+r"(_a), "+r"(_out), "+r"(_n)
        :
        : "rax", "rbx", "rcx", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );
}

/* fe_add_sq_ucode: fused (x + y) → A_out, then A_out² → AA_out.
 *
 * EXPERIMENTAL: measured break-even with separate fe_add + fe_sq_ucode on
 * Goldmont (2026-05-19). The memory roundtrip between the add and the
 * patch fire is already hidden by OoO + store-to-load forwarding while
 * the patch's ~37-cyc microcode block executes, so eliminating it via
 * a fused wrapper yields no measurable gain. Kept here as documentation
 * of the failed hypothesis; not used in the ladder.
 *
 * Register staging mirrors fe_sq_ucode after the add:
 *   A[0]=RDI, A[1]=RSI, A[2]=R12, A[3]=R11, A[4]=R14
 * Output of patch (h0..h4) goes through (RDI, R9, R10, RBX, RAX), stored to AA_out. */
static void fe_add_sq_ucode(const uint64_t *x, const uint64_t *y,
                            uint64_t *A_out, uint64_t *AA_out) {
    register const uint64_t *_x    asm("rdi") = x;
    register const uint64_t *_y    asm("rsi") = y;
    register       uint64_t *_A    asm("rdx") = A_out;
    register       uint64_t *_AA   asm("rcx") = AA_out;

    asm volatile(
        /* Load x[4..0]; x[1] parks in R8 so RSI stays live for y[] loads. */
        "mov r14, [rdi + 32]\n\t"
        "mov r11, [rdi + 24]\n\t"
        "mov r12, [rdi + 16]\n\t"
        "mov r8,  [rdi + 8]\n\t"
        "mov rdi, [rdi]\n\t"

        /* Fold y[*] into the same registers via add-mem. */
        "add r14, [rsi + 32]\n\t"
        "add r11, [rsi + 24]\n\t"
        "add r12, [rsi + 16]\n\t"
        "add r8,  [rsi + 8]\n\t"
        "add rdi, [rsi]\n\t"

        /* Store A to A_out (RDX still holds A_out — still alive). */
        "mov [rdx],      rdi\n\t"
        "mov [rdx + 8],  r8\n\t"
        "mov [rdx + 16], r12\n\t"
        "mov [rdx + 24], r11\n\t"
        "mov [rdx + 32], r14\n\t"

        /* Move A[1] to RSI for the patch convention; AA_out to RBP. */
        "mov rsi, r8\n\t"
        "mov rbp, rcx\n\t"

        /* Precompute 2A and 19A (RDX now safe to clobber). */
        "lea r15, [rdi + rdi]\n\t"
        "lea r13, [rsi + rsi]\n\t"
        "lea r9,  [r12 + r12]\n\t"
        "lea r10, [r11 + r11]\n\t"
        "imul rbx, r14, 19\n\t"
        "imul rdx, r11, 19\n\t"

        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* Fire fe_sq (vmread, 0f 78 ca) */
        ".byte 0x0f, 0x78, 0xca\n\t"

        "mov [rbp],      rdi\n\t"
        "mov [rbp + 8],  r9\n\t"
        "mov [rbp + 16], r10\n\t"
        "mov [rbp + 24], rbx\n\t"
        "mov [rbp + 32], rax\n\t"

        : "+r"(_x), "+r"(_y), "+r"(_A), "+r"(_AA)
        :
        : "rax", "rbx", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );
}

/* fe_sub_sq_ucode: fused (x − y) → B_out, then B_out² → BB_out.
 *
 * EXPERIMENTAL — see fe_add_sq_ucode above. Same break-even result; kept
 * as documentation. Subtraction uses the 2p bias (same constants as the
 * scalar fe_sub) to keep limbs positive prior to squaring. */
static void fe_sub_sq_ucode(const uint64_t *x, const uint64_t *y,
                            uint64_t *B_out, uint64_t *BB_out) {
    register const uint64_t *_x    asm("rdi") = x;
    register const uint64_t *_y    asm("rsi") = y;
    register       uint64_t *_B    asm("rdx") = B_out;
    register       uint64_t *_BB   asm("rcx") = BB_out;

    asm volatile(
        /* Load x[4..0]. */
        "mov r14, [rdi + 32]\n\t"
        "mov r11, [rdi + 24]\n\t"
        "mov r12, [rdi + 16]\n\t"
        "mov r8,  [rdi + 8]\n\t"
        "mov rdi, [rdi]\n\t"

        /* Bias: limb 0 += 2*(2^51-19); limbs 1..4 += 2*(2^51-1). */
        "mov rax, 0xFFFFFFFFFFFDA\n\t"
        "mov rbx, 0xFFFFFFFFFFFFE\n\t"
        "add rdi, rax\n\t"
        "add r8,  rbx\n\t"
        "add r12, rbx\n\t"
        "add r11, rbx\n\t"
        "add r14, rbx\n\t"

        /* Subtract y[*]. */
        "sub rdi, [rsi]\n\t"
        "sub r8,  [rsi + 8]\n\t"
        "sub r12, [rsi + 16]\n\t"
        "sub r11, [rsi + 24]\n\t"
        "sub r14, [rsi + 32]\n\t"

        /* Store B to B_out (RDX still alive). */
        "mov [rdx],      rdi\n\t"
        "mov [rdx + 8],  r8\n\t"
        "mov [rdx + 16], r12\n\t"
        "mov [rdx + 24], r11\n\t"
        "mov [rdx + 32], r14\n\t"

        "mov rsi, r8\n\t"
        "mov rbp, rcx\n\t"

        "lea r15, [rdi + rdi]\n\t"
        "lea r13, [rsi + rsi]\n\t"
        "lea r9,  [r12 + r12]\n\t"
        "lea r10, [r11 + r11]\n\t"
        "imul rbx, r14, 19\n\t"
        "imul rdx, r11, 19\n\t"

        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        ".byte 0x0f, 0x78, 0xca\n\t"

        "mov [rbp],      rdi\n\t"
        "mov [rbp + 8],  r9\n\t"
        "mov [rbp + 16], r10\n\t"
        "mov [rbp + 24], rbx\n\t"
        "mov [rbp + 32], rax\n\t"

        : "+r"(_x), "+r"(_y), "+r"(_B), "+r"(_BB)
        :
        : "rax", "rbx", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );
}

/* ════════════════════════════════════════════════════════════════════
 * NATIVE C FIELD OPERATIONS (compiled with -O3)
 * ════════════════════════════════════════════════════════════════════ */

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

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    uint64_t a0=a[0], a1=a[1], a2=a[2], a3=a[3], a4=a[4];
    uint64_t d0=2*a0, d1=2*a1, d2=2*a2, d3=2*a3;
    uint64_t r3=19*a3, r4=19*a4;

    __uint128_t c0 = (__uint128_t)a0*a0 + (__uint128_t)d1*r4 + (__uint128_t)d2*r3;
    __uint128_t c1 = (__uint128_t)d0*a1 + (__uint128_t)r3*a3 + (__uint128_t)d2*r4;
    __uint128_t c2 = (__uint128_t)d0*a2 + (__uint128_t)a1*a1 + (__uint128_t)d3*r4;
    __uint128_t c3 = (__uint128_t)d0*a3 + (__uint128_t)d1*a2 + (__uint128_t)r4*a4;
    __uint128_t c4 = (__uint128_t)d0*a4 + (__uint128_t)d1*a3 + (__uint128_t)a2*a2;

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

/* ════════════════════════════════════════════════════════════════════
 * FIAT-CRYPTO FIELD OPERATIONS (SUPERCOP-style reference baseline)
 *
 * Both files share identical helper typedefs; C11 allows redundant
 * typedef declarations of the same type, so including both compiles
 * cleanly under -std=gnu11+. We only consume the two carry_* functions.
 * ════════════════════════════════════════════════════════════════════ */

#include "../curvesC/curve25519_mul.c"
#include "../curvesC/curve25519_square.c"

static inline void fe_mul_fiat(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    fiat_curve25519_carry_mul(out, a, b);
}

static inline void fe_sq_fiat(const uint64_t *a, uint64_t *out) {
    fiat_curve25519_carry_square(out, a);
}

/* CryptOpt-tuned x86-64 asm (assembled from cryptopt_{mul,sq}.asm). Same
 * fiat-style signature as the C version; symbols renamed at assemble
 * time to avoid clashing. */
extern void cryptopt_carry_mul(uint64_t out[5], const uint64_t a[5], const uint64_t b[5]);
extern void cryptopt_carry_square(uint64_t out[5], const uint64_t a[5]);

static inline void fe_mul_cryptopt(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    cryptopt_carry_mul(out, a, b);
}

static inline void fe_sq_cryptopt(const uint64_t *a, uint64_t *out) {
    cryptopt_carry_square(out, a);
}

/* ════════════════════════════════════════════════════════════════════
 * SUPERCOP donna_c64 (vendored from supercop-20260330)
 *
 * The entry point `crypto_scalarmult` in donna_c64/smult.c is renamed to
 * x25519_donna_c64 via macro substitution before #include. The static
 * helper functions (fmul, fexpand, crecip, ...) are self-contained and
 * do not clash with our identifiers. The `crypto_scalarmult.h` stub at
 * simple/include/ satisfies smult.c's only external include.
 * ════════════════════════════════════════════════════════════════════ */

#define crypto_scalarmult x25519_donna_c64
#include "supercop-20260330/crypto_scalarmult/curve25519/donna_c64/smult.c"
#undef crypto_scalarmult

/* Forward-declare with our public name for use below. */
extern int x25519_donna_c64(unsigned char *mypublic,
                            const unsigned char *secret,
                            const unsigned char *basepoint);

/* ════════════════════════════════════════════════════════════════════
 * SUPERCOP amd64-51 (hand-tuned x86-64 assembly, Bernstein/Schwabe 2011)
 *
 * Compiled as separate .o files by the Makefile from the SUPERCOP tree.
 * The entry point `crypto_scalarmult` in mont25519.c is renamed to
 * x25519_amd64_51 via -include include/amd64_51_namespace.h. The static
 * library libcryptoint.a provides supercop_int64_optblocker.
 * ════════════════════════════════════════════════════════════════════ */

extern int x25519_amd64_51(unsigned char *out,
                           const unsigned char *scalar,
                           const unsigned char *point);

/* ════════════════════════════════════════════════════════════════════
 * SUPERCOP amd64-64 (4x64-bit saturated, Bernstein/Schwabe)
 *
 * Same author family as amd64-51 but uses radix 2^64 (saturated 4-limb
 * representation). lib25519's autotuner selects this on Goldmont — its
 * Goldmont measurement of ~280k cycles maps to this implementation,
 * not amd64-51.
 * ════════════════════════════════════════════════════════════════════ */

extern int x25519_amd64_64(unsigned char *out,
                           const unsigned char *scalar,
                           const unsigned char *point);

/* ════════════════════════════════════════════════════════════════════
 * Hybrid: amd64-51's driver/invert/pack + microcode field ops.
 *
 * Same SUPERCOP framework (mont25519/mladder/invert/pack/unpack/freeze/
 * cswap) but with ladderstep + fe25519_mul + fe25519_square replaced by
 * C versions that call our microcode wrappers. Isolates field-op cost
 * from the surrounding ladder structure — see benchmark_review.md issue
 * #1. Sources in simple/amd64-51-ucode/.
 * ════════════════════════════════════════════════════════════════════ */

extern int x25519_amd64_51_ucode(unsigned char *out,
                                 const unsigned char *scalar,
                                 const unsigned char *point);

/* ════════════════════════════════════════════════════════════════════
 * COMMON FIELD OPERATIONS (pure C, used by all backends)
 * ════════════════════════════════════════════════════════════════════ */

static inline void fe_add(fe out, const fe a, const fe b) {
    out[0] = a[0] + b[0];
    out[1] = a[1] + b[1];
    out[2] = a[2] + b[2];
    out[3] = a[3] + b[3];
    out[4] = a[4] + b[4];
}

/*
 * fe_sub: out = a - b, with bias to keep limbs positive.
 * We add 2*p to a before subtracting b.
 * 2*p = 2*(2^255 - 19) in 5-limb representation:
 *   limb 0: 2*(2^51 - 19) = 0xFFFFFFFFFFFFDA
 *   limbs 1-4: 2*(2^51 - 1) = 0xFFFFFFFFFFFFFE = 2*MASK51
 */
static inline void fe_sub(fe out, const fe a, const fe b) {
    out[0] = (a[0] + 0xFFFFFFFFFFFDAULL) - b[0];
    out[1] = (a[1] + 0xFFFFFFFFFFFFEULL) - b[1];
    out[2] = (a[2] + 0xFFFFFFFFFFFFEULL) - b[2];
    out[3] = (a[3] + 0xFFFFFFFFFFFFEULL) - b[3];
    out[4] = (a[4] + 0xFFFFFFFFFFFFEULL) - b[4];
}

/* fe_mul121665: out = a * 121665 */
static void fe_mul121665(fe out, const fe a) {
    __uint128_t c;
    c = (__uint128_t)a[0] * 121665;
    out[0] = (uint64_t)c & MASK51; c >>= 51;
    c += (__uint128_t)a[1] * 121665;
    out[1] = (uint64_t)c & MASK51; c >>= 51;
    c += (__uint128_t)a[2] * 121665;
    out[2] = (uint64_t)c & MASK51; c >>= 51;
    c += (__uint128_t)a[3] * 121665;
    out[3] = (uint64_t)c & MASK51; c >>= 51;
    c += (__uint128_t)a[4] * 121665;
    out[4] = (uint64_t)c & MASK51; c >>= 51;
    out[0] += (uint64_t)c * 19;
    uint64_t carry = out[0] >> 51;
    out[0] &= MASK51;
    out[1] += carry;
}

/* constant-time conditional swap — scalar */
static inline void fe_cswap_scalar(fe a, fe b, uint64_t swap) {
    swap = (uint64_t)(-(int64_t)swap);  /* 0 or 0xFFFF...FFFF */
    for (int i = 0; i < 5; i++) {
        uint64_t x = (a[i] ^ b[i]) & swap;
        a[i] ^= x;
        b[i] ^= x;
    }
}

/* SSE2 conditional swap: 5 limbs packed as 2 × (2 × 64-bit) XMMs plus
 * one scalar word for limb 4. */
static inline void fe_cswap_sse2(fe a, fe b, uint64_t swap) {
    uint64_t mask = (uint64_t)(-(int64_t)swap);
    __m128i m = _mm_set1_epi64x((long long)mask);

    __m128i a01 = _mm_loadu_si128((const __m128i *)&a[0]);
    __m128i b01 = _mm_loadu_si128((const __m128i *)&b[0]);
    __m128i x01 = _mm_and_si128(_mm_xor_si128(a01, b01), m);
    _mm_storeu_si128((__m128i *)&a[0], _mm_xor_si128(a01, x01));
    _mm_storeu_si128((__m128i *)&b[0], _mm_xor_si128(b01, x01));

    __m128i a23 = _mm_loadu_si128((const __m128i *)&a[2]);
    __m128i b23 = _mm_loadu_si128((const __m128i *)&b[2]);
    __m128i x23 = _mm_and_si128(_mm_xor_si128(a23, b23), m);
    _mm_storeu_si128((__m128i *)&a[2], _mm_xor_si128(a23, x23));
    _mm_storeu_si128((__m128i *)&b[2], _mm_xor_si128(b23, x23));

    uint64_t x4 = (a[4] ^ b[4]) & mask;
    a[4] ^= x4;
    b[4] ^= x4;
}

/* Scalar wins on Goldmont: 2.63 cyc/call vs SSE2 3.10 cyc/call.
 * GCC -O3 -march=native pipelines the 5-iter scalar loop well; the
 * SSE2 version pays a mask-broadcast and cross-domain penalty. */
static inline void fe_cswap(fe a, fe b, uint64_t swap) {
    fe_cswap_scalar(a, b, swap);
}

static inline void fe_copy(fe out, const fe a) {
    memcpy(out, a, 5 * sizeof(uint64_t));
}

/* fe_reduce: full reduction mod p = 2^255 - 19 */
static void fe_reduce(fe out, const fe a) {
    uint64_t t[5];
    memcpy(t, a, 40);

    /* first, propagate carries */
    uint64_t carry;
    carry = t[0] >> 51; t[0] &= MASK51; t[1] += carry;
    carry = t[1] >> 51; t[1] &= MASK51; t[2] += carry;
    carry = t[2] >> 51; t[2] &= MASK51; t[3] += carry;
    carry = t[3] >> 51; t[3] &= MASK51; t[4] += carry;
    carry = t[4] >> 51; t[4] &= MASK51; t[0] += carry * 19;
    carry = t[0] >> 51; t[0] &= MASK51; t[1] += carry;

    /* now t is in [0, 2^255-1]. subtract p if t >= p */
    /* compute t - p; if no borrow, use the result */
    uint64_t s[5];
    int64_t borrow;
    s[0] = t[0] - (MASK51 - 18);  /* p limb 0 = 2^51 - 19 */
    borrow = (int64_t)s[0] >> 63;
    s[1] = t[1] - MASK51 + borrow;
    borrow = (int64_t)s[1] >> 63;
    s[2] = t[2] - MASK51 + borrow;
    borrow = (int64_t)s[2] >> 63;
    s[3] = t[3] - MASK51 + borrow;
    borrow = (int64_t)s[3] >> 63;
    s[4] = t[4] - MASK51 + borrow;
    borrow = (int64_t)s[4] >> 63;

    /* if borrow == 0, s >= 0, so t >= p: use s. else use t. */
    uint64_t mask = (uint64_t)borrow;  /* 0 if t>=p, 0xFFFF... if t<p */
    for (int i = 0; i < 5; i++) {
        out[i] = (t[i] & mask) | (s[i] & ~mask);
        out[i] &= MASK51;
    }
}

/* ════════════════════════════════════════════════════════════════════
 * BYTE ENCODING / DECODING (little-endian, 256-bit)
 * ════════════════════════════════════════════════════════════════════ */

static void fe_frombytes(fe out, const uint8_t in[32]) {
    uint64_t t[5];
    /* read 256 bits as a little-endian integer, split into 51-bit limbs */
    t[0]  = ((uint64_t)in[0])
           | ((uint64_t)in[1] << 8)
           | ((uint64_t)in[2] << 16)
           | ((uint64_t)in[3] << 24)
           | ((uint64_t)in[4] << 32)
           | ((uint64_t)in[5] << 40)
           | ((uint64_t)(in[6] & 0x07) << 48);
    t[1]  = ((uint64_t)in[6] >> 3)
           | ((uint64_t)in[7] << 5)
           | ((uint64_t)in[8] << 13)
           | ((uint64_t)in[9] << 21)
           | ((uint64_t)in[10] << 29)
           | ((uint64_t)in[11] << 37)
           | ((uint64_t)(in[12] & 0x3f) << 45);
    t[2]  = ((uint64_t)in[12] >> 6)
           | ((uint64_t)in[13] << 2)
           | ((uint64_t)in[14] << 10)
           | ((uint64_t)in[15] << 18)
           | ((uint64_t)in[16] << 26)
           | ((uint64_t)in[17] << 34)
           | ((uint64_t)in[18] << 42)
           | ((uint64_t)(in[19] & 0x01) << 50);
    t[3]  = ((uint64_t)in[19] >> 1)
           | ((uint64_t)in[20] << 7)
           | ((uint64_t)in[21] << 15)
           | ((uint64_t)in[22] << 23)
           | ((uint64_t)in[23] << 31)
           | ((uint64_t)in[24] << 39)
           | ((uint64_t)(in[25] & 0x0f) << 47);
    t[4]  = ((uint64_t)in[25] >> 4)
           | ((uint64_t)in[26] << 4)
           | ((uint64_t)in[27] << 12)
           | ((uint64_t)in[28] << 20)
           | ((uint64_t)in[29] << 28)
           | ((uint64_t)in[30] << 36)
           | ((uint64_t)(in[31] & 0x7f) << 44);  /* clear bit 255 */

    memcpy(out, t, 40);
}

static void fe_tobytes(uint8_t out[32], const fe in) {
    fe t;
    fe_reduce(t, in);

    uint64_t h0 = t[0], h1 = t[1], h2 = t[2], h3 = t[3], h4 = t[4];

    out[0]  = (uint8_t)(h0);
    out[1]  = (uint8_t)(h0 >> 8);
    out[2]  = (uint8_t)(h0 >> 16);
    out[3]  = (uint8_t)(h0 >> 24);
    out[4]  = (uint8_t)(h0 >> 32);
    out[5]  = (uint8_t)(h0 >> 40);
    out[6]  = (uint8_t)((h0 >> 48) | (h1 << 3));
    out[7]  = (uint8_t)(h1 >> 5);
    out[8]  = (uint8_t)(h1 >> 13);
    out[9]  = (uint8_t)(h1 >> 21);
    out[10] = (uint8_t)(h1 >> 29);
    out[11] = (uint8_t)(h1 >> 37);
    out[12] = (uint8_t)((h1 >> 45) | (h2 << 6));
    out[13] = (uint8_t)(h2 >> 2);
    out[14] = (uint8_t)(h2 >> 10);
    out[15] = (uint8_t)(h2 >> 18);
    out[16] = (uint8_t)(h2 >> 26);
    out[17] = (uint8_t)(h2 >> 34);
    out[18] = (uint8_t)(h2 >> 42);
    out[19] = (uint8_t)((h2 >> 50) | (h3 << 1));
    out[20] = (uint8_t)(h3 >> 7);
    out[21] = (uint8_t)(h3 >> 15);
    out[22] = (uint8_t)(h3 >> 23);
    out[23] = (uint8_t)(h3 >> 31);
    out[24] = (uint8_t)(h3 >> 39);
    out[25] = (uint8_t)((h3 >> 47) | (h4 << 4));
    out[26] = (uint8_t)(h4 >> 4);
    out[27] = (uint8_t)(h4 >> 12);
    out[28] = (uint8_t)(h4 >> 20);
    out[29] = (uint8_t)(h4 >> 28);
    out[30] = (uint8_t)(h4 >> 36);
    out[31] = (uint8_t)(h4 >> 44);
}

/* ════════════════════════════════════════════════════════════════════
 * FIELD INVERSION via Fermat's little theorem: a^(p-2) mod p
 * p - 2 = 2^255 - 21
 *
 * Standard addition chain from donna/ref10.
 * ════════════════════════════════════════════════════════════════════ */

static void fe_invert_native(fe out, const fe z) {
    fe z2, z9, z11, t, t0, t1, t2, t3;
    int i;

    fe_sq_native(z, z2);
    fe_sq_native(z2, t);
    fe_sq_native(t, t);
    fe_mul_native(t, z, z9);
    fe_mul_native(z9, z2, z11);
    fe_sq_native(z11, t);
    fe_mul_native(t, z9, t0);

    fe_sq_native(t0, t1);
    for (i = 1; i < 5; i++) fe_sq_native(t1, t1);
    fe_mul_native(t1, t0, t1);

    fe_sq_native(t1, t2);
    for (i = 1; i < 10; i++) fe_sq_native(t2, t2);
    fe_mul_native(t2, t1, t2);

    fe_sq_native(t2, t3);
    for (i = 1; i < 20; i++) fe_sq_native(t3, t3);
    fe_mul_native(t3, t2, t3);

    for (i = 0; i < 10; i++) fe_sq_native(t3, t3);
    fe_mul_native(t3, t1, t1);

    fe_sq_native(t1, t2);
    for (i = 1; i < 50; i++) fe_sq_native(t2, t2);
    fe_mul_native(t2, t1, t2);

    fe_sq_native(t2, t3);
    for (i = 1; i < 100; i++) fe_sq_native(t3, t3);
    fe_mul_native(t3, t2, t3);

    for (i = 0; i < 50; i++) fe_sq_native(t3, t3);
    fe_mul_native(t3, t1, t1);

    fe_sq_native(t1, t1);
    fe_sq_native(t1, t1);
    fe_sq_native(t1, t1);
    fe_sq_native(t1, t1);
    fe_sq_native(t1, t1);
    fe_mul_native(t1, z11, out);
}

static void fe_invert_ucode(fe out, const fe z) {
    fe z2, z9, z11, t, t0, t1, t2, t3;

    fe_sq_ucode(z, z2);
    fe_sq_ucode(z2, t);
    fe_sq_ucode(t, t);
    fe_mul_ucode(z, t, z9);
    fe_mul_ucode(z9, z2, z11);
    fe_sq_ucode(z11, t);
    fe_mul_ucode(z9, t, t0);

    fe_sq_ucode_n(t1, t0,   5);    /* 5 squarings */
    fe_mul_ucode(t0, t1, t1);

    fe_sq_ucode_n(t2, t1,  10);    /* 10 */
    fe_mul_ucode(t1, t2, t2);

    fe_sq_ucode_n(t3, t2,  20);    /* 20 */
    fe_mul_ucode(t2, t3, t3);

    fe_sq_ucode_n(t3, t3,  10);    /* 10 */
    fe_mul_ucode(t1, t3, t1);

    fe_sq_ucode_n(t2, t1,  50);    /* 50 */
    fe_mul_ucode(t1, t2, t2);

    fe_sq_ucode_n(t3, t2, 100);    /* 100 */
    fe_mul_ucode(t2, t3, t3);

    fe_sq_ucode_n(t3, t3,  50);    /* 50 */
    fe_mul_ucode(t1, t3, t1);

    fe_sq_ucode_n(t1, t1,   5);    /* final 5 */
    fe_mul_ucode(z11, t1, out);
}

static void fe_invert_fiat(fe out, const fe z) {
    fe z2, z9, z11, t, t0, t1, t2, t3;
    int i;

    fe_sq_fiat(z, z2);
    fe_sq_fiat(z2, t);
    fe_sq_fiat(t, t);
    fe_mul_fiat(t, z, z9);
    fe_mul_fiat(z9, z2, z11);
    fe_sq_fiat(z11, t);
    fe_mul_fiat(t, z9, t0);

    fe_sq_fiat(t0, t1);
    for (i = 1; i < 5; i++) fe_sq_fiat(t1, t1);
    fe_mul_fiat(t1, t0, t1);

    fe_sq_fiat(t1, t2);
    for (i = 1; i < 10; i++) fe_sq_fiat(t2, t2);
    fe_mul_fiat(t2, t1, t2);

    fe_sq_fiat(t2, t3);
    for (i = 1; i < 20; i++) fe_sq_fiat(t3, t3);
    fe_mul_fiat(t3, t2, t3);

    for (i = 0; i < 10; i++) fe_sq_fiat(t3, t3);
    fe_mul_fiat(t3, t1, t1);

    fe_sq_fiat(t1, t2);
    for (i = 1; i < 50; i++) fe_sq_fiat(t2, t2);
    fe_mul_fiat(t2, t1, t2);

    fe_sq_fiat(t2, t3);
    for (i = 1; i < 100; i++) fe_sq_fiat(t3, t3);
    fe_mul_fiat(t3, t2, t3);

    for (i = 0; i < 50; i++) fe_sq_fiat(t3, t3);
    fe_mul_fiat(t3, t1, t1);

    fe_sq_fiat(t1, t1);
    fe_sq_fiat(t1, t1);
    fe_sq_fiat(t1, t1);
    fe_sq_fiat(t1, t1);
    fe_sq_fiat(t1, t1);
    fe_mul_fiat(t1, z11, out);
}

static void fe_invert_cryptopt(fe out, const fe z) {
    fe z2, z9, z11, t, t0, t1, t2, t3;
    int i;

    fe_sq_cryptopt(z, z2);
    fe_sq_cryptopt(z2, t);
    fe_sq_cryptopt(t, t);
    fe_mul_cryptopt(t, z, z9);
    fe_mul_cryptopt(z9, z2, z11);
    fe_sq_cryptopt(z11, t);
    fe_mul_cryptopt(t, z9, t0);

    fe_sq_cryptopt(t0, t1);
    for (i = 1; i < 5; i++) fe_sq_cryptopt(t1, t1);
    fe_mul_cryptopt(t1, t0, t1);

    fe_sq_cryptopt(t1, t2);
    for (i = 1; i < 10; i++) fe_sq_cryptopt(t2, t2);
    fe_mul_cryptopt(t2, t1, t2);

    fe_sq_cryptopt(t2, t3);
    for (i = 1; i < 20; i++) fe_sq_cryptopt(t3, t3);
    fe_mul_cryptopt(t3, t2, t3);

    for (i = 0; i < 10; i++) fe_sq_cryptopt(t3, t3);
    fe_mul_cryptopt(t3, t1, t1);

    fe_sq_cryptopt(t1, t2);
    for (i = 1; i < 50; i++) fe_sq_cryptopt(t2, t2);
    fe_mul_cryptopt(t2, t1, t2);

    fe_sq_cryptopt(t2, t3);
    for (i = 1; i < 100; i++) fe_sq_cryptopt(t3, t3);
    fe_mul_cryptopt(t3, t2, t3);

    for (i = 0; i < 50; i++) fe_sq_cryptopt(t3, t3);
    fe_mul_cryptopt(t3, t1, t1);

    fe_sq_cryptopt(t1, t1);
    fe_sq_cryptopt(t1, t1);
    fe_sq_cryptopt(t1, t1);
    fe_sq_cryptopt(t1, t1);
    fe_sq_cryptopt(t1, t1);
    fe_mul_cryptopt(t1, z11, out);
}

/* ════════════════════════════════════════════════════════════════════
 * X25519 MONTGOMERY LADDER (RFC 7748)
 * ════════════════════════════════════════════════════════════════════ */

static void scalar_clamp(uint8_t s[32]) {
    s[0]  &= 248;
    s[31] &= 127;
    s[31] |= 64;
}

static void x25519_native(uint8_t out[32], const uint8_t scalar[32],
                           const uint8_t point[32]) {
    uint8_t e[32];
    memcpy(e, scalar, 32);
    scalar_clamp(e);

    fe x1, x2, z2, x3, z3;
    fe A, AA, B, BB, E, C, D, DA, CB, t0;

    fe_frombytes(x1, point);
    fe_copy(x2, (const uint64_t[]){1,0,0,0,0});
    memset(z2, 0, sizeof(fe));
    fe_copy(x3, x1);
    fe_copy(z3, (const uint64_t[]){1,0,0,0,0});

    uint64_t swap = 0;

    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (e[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(A, x2, z2);
        fe_sq_native(A, AA);
        fe_sub(B, x2, z2);
        fe_sq_native(B, BB);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul_native(D, A, DA);
        fe_mul_native(C, B, CB);

        fe_add(t0, DA, CB);
        fe_sq_native(t0, x3);

        fe_sub(t0, DA, CB);
        fe_sq_native(t0, z3);
        fe_mul_native(x1, z3, z3);

        fe_mul_native(AA, BB, x2);

        fe_mul121665(t0, E);
        fe_add(t0, AA, t0);
        fe_mul_native(E, t0, z2);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert_native(z2, z2);
    fe_mul_native(x2, z2, x2);
    fe_tobytes(out, x2);
}

static void x25519_ucode(uint8_t out[32], const uint8_t scalar[32],
                          const uint8_t point[32]) {
    uint8_t e[32];
    memcpy(e, scalar, 32);
    scalar_clamp(e);

    fe x1, x2, z2, x3, z3;
    fe A, AA, B, BB, E, C, D, DA, CB, t0;

    fe_frombytes(x1, point);
    fe_copy(x2, (const uint64_t[]){1,0,0,0,0});
    memset(z2, 0, sizeof(fe));
    fe_copy(x3, x1);
    fe_copy(z3, (const uint64_t[]){1,0,0,0,0});

    uint64_t swap = 0;

    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (e[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(A, x2, z2);
        fe_sq_ucode(A, AA);
        fe_sub(B, x2, z2);
        fe_sq_ucode(B, BB);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul_ucode(D, A, DA);
        fe_mul_ucode(C, B, CB);

        fe_add(t0, DA, CB);
        fe_sq_ucode(t0, x3);

        fe_sub(t0, DA, CB);
        fe_sq_ucode(t0, z3);
        fe_mul_ucode(x1, z3, z3);

        fe_mul_ucode(AA, BB, x2);

        fe_mul121665(t0, E);
        fe_add(t0, AA, t0);
        fe_mul_ucode(E, t0, z2);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert_ucode(z2, z2);
    fe_mul_ucode(x2, z2, x2);
    fe_tobytes(out, x2);
}

static void x25519_fiat(uint8_t out[32], const uint8_t scalar[32],
                        const uint8_t point[32]) {
    uint8_t e[32];
    memcpy(e, scalar, 32);
    scalar_clamp(e);

    fe x1, x2, z2, x3, z3;
    fe A, AA, B, BB, E, C, D, DA, CB, t0;

    fe_frombytes(x1, point);
    fe_copy(x2, (const uint64_t[]){1,0,0,0,0});
    memset(z2, 0, sizeof(fe));
    fe_copy(x3, x1);
    fe_copy(z3, (const uint64_t[]){1,0,0,0,0});

    uint64_t swap = 0;

    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (e[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(A, x2, z2);
        fe_sq_fiat(A, AA);
        fe_sub(B, x2, z2);
        fe_sq_fiat(B, BB);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul_fiat(D, A, DA);
        fe_mul_fiat(C, B, CB);

        fe_add(t0, DA, CB);
        fe_sq_fiat(t0, x3);

        fe_sub(t0, DA, CB);
        fe_sq_fiat(t0, z3);
        fe_mul_fiat(x1, z3, z3);

        fe_mul_fiat(AA, BB, x2);

        fe_mul121665(t0, E);
        fe_add(t0, AA, t0);
        fe_mul_fiat(E, t0, z2);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert_fiat(z2, z2);
    fe_mul_fiat(x2, z2, x2);
    fe_tobytes(out, x2);
}

static void x25519_cryptopt(uint8_t out[32], const uint8_t scalar[32],
                            const uint8_t point[32]) {
    uint8_t e[32];
    memcpy(e, scalar, 32);
    scalar_clamp(e);

    fe x1, x2, z2, x3, z3;
    fe A, AA, B, BB, E, C, D, DA, CB, t0;

    fe_frombytes(x1, point);
    fe_copy(x2, (const uint64_t[]){1,0,0,0,0});
    memset(z2, 0, sizeof(fe));
    fe_copy(x3, x1);
    fe_copy(z3, (const uint64_t[]){1,0,0,0,0});

    uint64_t swap = 0;

    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (e[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(A, x2, z2);
        fe_sq_cryptopt(A, AA);
        fe_sub(B, x2, z2);
        fe_sq_cryptopt(B, BB);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul_cryptopt(D, A, DA);
        fe_mul_cryptopt(C, B, CB);

        fe_add(t0, DA, CB);
        fe_sq_cryptopt(t0, x3);

        fe_sub(t0, DA, CB);
        fe_sq_cryptopt(t0, z3);
        fe_mul_cryptopt(x1, z3, z3);

        fe_mul_cryptopt(AA, BB, x2);

        fe_mul121665(t0, E);
        fe_add(t0, AA, t0);
        fe_mul_cryptopt(E, t0, z2);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert_cryptopt(z2, z2);
    fe_mul_cryptopt(x2, z2, x2);
    fe_tobytes(out, x2);
}

/* ════════════════════════════════════════════════════════════════════
 * UTILITY FUNCTIONS
 * ════════════════════════════════════════════════════════════════════ */

static void hex_to_bytes(const char *hex, uint8_t *out, int len) {
    for (int i = 0; i < len; i++) {
        unsigned int val;
        sscanf(hex + 2*i, "%02x", &val);
        out[i] = (uint8_t)val;
    }
}

static void print_hex(const char *label, const uint8_t *data, int len) {
    printf("  %s: ", label);
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
    printf("\n");
}

static int memcmp_hex(const uint8_t *data, const char *hex, int len) {
    uint8_t expected[32];
    hex_to_bytes(hex, expected, len);
    return memcmp(data, expected, len);
}

/* ════════════════════════════════════════════════════════════════════
 * RFC 7748 TEST VECTORS
 * ════════════════════════════════════════════════════════════════════ */

static int test_rfc7748(void) {
    int pass = 0, fail = 0;
    uint8_t scalar[32], point[32], result_native[32], result_ucode[32];

    printf("=== RFC 7748 Test Vectors ===\n\n");

    /* --- Test vector 1 --- */
    printf("--- Test vector 1 ---\n");
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
                 scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
                 point, 32);
    x25519_native(result_native, scalar, point);
    x25519_ucode(result_ucode, scalar, point);

    print_hex("native", result_native, 32);
    print_hex("ucode ", result_ucode, 32);

    if (memcmp_hex(result_native,
                   "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552",
                   32) == 0) {
        printf("  native: PASS\n"); pass++;
    } else {
        printf("  native: FAIL\n"); fail++;
    }
    if (memcmp_hex(result_ucode,
                   "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552",
                   32) == 0) {
        printf("  ucode:  PASS\n"); pass++;
    } else {
        printf("  ucode:  FAIL\n"); fail++;
    }

    /* --- Test vector 2 --- */
    printf("\n--- Test vector 2 ---\n");
    hex_to_bytes("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
                 scalar, 32);
    hex_to_bytes("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
                 point, 32);
    x25519_native(result_native, scalar, point);
    x25519_ucode(result_ucode, scalar, point);

    print_hex("native", result_native, 32);
    print_hex("ucode ", result_ucode, 32);

    if (memcmp_hex(result_native,
                   "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957",
                   32) == 0) {
        printf("  native: PASS\n"); pass++;
    } else {
        printf("  native: FAIL\n"); fail++;
    }
    if (memcmp_hex(result_ucode,
                   "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957",
                   32) == 0) {
        printf("  ucode:  PASS\n"); pass++;
    } else {
        printf("  ucode:  FAIL\n"); fail++;
    }

    /* --- Iterated test: 1 iteration --- */
    printf("\n--- Iterated test (1 iteration) ---\n");
    {
        uint8_t k[32] = {0}, u[32] = {0}, r[32];
        k[0] = 9;
        u[0] = 9;

        x25519_native(r, k, u);

        print_hex("native after 1", r, 32);
        if (memcmp_hex(r,
                       "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079",
                       32) == 0) {
            printf("  native: PASS\n"); pass++;
        } else {
            printf("  native: FAIL\n"); fail++;
        }

        x25519_ucode(r, k, u);
        print_hex("ucode  after 1", r, 32);
        if (memcmp_hex(r,
                       "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079",
                       32) == 0) {
            printf("  ucode:  PASS\n"); pass++;
        } else {
            printf("  ucode:  FAIL\n"); fail++;
        }
    }

    /* --- Iterated test: 1000 iterations --- */
    printf("\n--- Iterated test (1000 iterations) ---\n");
    {
        uint8_t k[32] = {0}, u[32] = {0}, r[32];
        k[0] = 9;
        u[0] = 9;

        uint8_t kn[32], un[32];
        memcpy(kn, k, 32);
        memcpy(un, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_native(r, kn, un);
            memcpy(un, kn, 32);
            memcpy(kn, r, 32);
        }
        print_hex("native after 1000", kn, 32);
        if (memcmp_hex(kn,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  native: PASS\n"); pass++;
        } else {
            printf("  native: FAIL\n"); fail++;
        }

        uint8_t ku[32], uu[32];
        memcpy(ku, k, 32);
        memcpy(uu, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_ucode(r, ku, uu);
            memcpy(uu, ku, 32);
            memcpy(ku, r, 32);
        }
        print_hex("ucode  after 1000", ku, 32);
        if (memcmp_hex(ku,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  ucode:  PASS\n"); pass++;
        } else {
            printf("  ucode:  FAIL\n"); fail++;
        }

        uint8_t kf[32], uf[32];
        memcpy(kf, k, 32);
        memcpy(uf, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_fiat(r, kf, uf);
            memcpy(uf, kf, 32);
            memcpy(kf, r, 32);
        }
        print_hex("fiat   after 1000", kf, 32);
        if (memcmp_hex(kf,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  fiat:   PASS\n"); pass++;
        } else {
            printf("  fiat:   FAIL\n"); fail++;
        }

        uint8_t kd[32], ud[32];
        memcpy(kd, k, 32);
        memcpy(ud, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_donna_c64(r, kd, ud);
            memcpy(ud, kd, 32);
            memcpy(kd, r, 32);
        }
        print_hex("donna  after 1000", kd, 32);
        if (memcmp_hex(kd,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  donna:  PASS\n"); pass++;
        } else {
            printf("  donna:  FAIL\n"); fail++;
        }

        uint8_t ka[32], ua[32];
        memcpy(ka, k, 32);
        memcpy(ua, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_amd64_51(r, ka, ua);
            memcpy(ua, ka, 32);
            memcpy(ka, r, 32);
        }
        print_hex("amd51  after 1000", ka, 32);
        if (memcmp_hex(ka,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  amd51:  PASS\n"); pass++;
        } else {
            printf("  amd51:  FAIL\n"); fail++;
        }

        uint8_t kau[32], uau[32];
        memcpy(kau, k, 32);
        memcpy(uau, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_amd64_51_ucode(r, kau, uau);
            memcpy(uau, kau, 32);
            memcpy(kau, r, 32);
        }
        print_hex("a51u   after 1000", kau, 32);
        if (memcmp_hex(kau,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  a51u:   PASS\n"); pass++;
        } else {
            printf("  a51u:   FAIL\n"); fail++;
        }

        uint8_t ka4[32], ua4[32];
        memcpy(ka4, k, 32);
        memcpy(ua4, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_amd64_64(r, ka4, ua4);
            memcpy(ua4, ka4, 32);
            memcpy(ka4, r, 32);
        }
        print_hex("amd64  after 1000", ka4, 32);
        if (memcmp_hex(ka4,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  amd64:  PASS\n"); pass++;
        } else {
            printf("  amd64:  FAIL\n"); fail++;
        }

        uint8_t kc[32], uc[32];
        memcpy(kc, k, 32);
        memcpy(uc, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_cryptopt(r, kc, uc);
            memcpy(uc, kc, 32);
            memcpy(kc, r, 32);
        }
        print_hex("crypto after 1000", kc, 32);
        if (memcmp_hex(kc,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  crypto: PASS\n"); pass++;
        } else {
            printf("  crypto: FAIL\n"); fail++;
        }

        if (memcmp(kn, ku, 32) == 0) {
            printf("  native==ucode: PASS\n"); pass++;
        } else {
            printf("  native==ucode: FAIL\n"); fail++;
        }
        if (memcmp(kn, kf, 32) == 0) {
            printf("  native==fiat:  PASS\n"); pass++;
        } else {
            printf("  native==fiat:  FAIL\n"); fail++;
        }
        if (memcmp(kn, kd, 32) == 0) {
            printf("  native==donna: PASS\n"); pass++;
        } else {
            printf("  native==donna: FAIL\n"); fail++;
        }
        if (memcmp(kn, ka, 32) == 0) {
            printf("  native==amd51: PASS\n"); pass++;
        } else {
            printf("  native==amd51: FAIL\n"); fail++;
        }
        if (memcmp(kn, kau, 32) == 0) {
            printf("  native==a51u:  PASS\n"); pass++;
        } else {
            printf("  native==a51u:  FAIL\n"); fail++;
        }
        if (memcmp(kn, ka4, 32) == 0) {
            printf("  native==amd64: PASS\n"); pass++;
        } else {
            printf("  native==amd64: FAIL\n"); fail++;
        }
        if (memcmp(kn, kc, 32) == 0) {
            printf("  native==crypto: PASS\n"); pass++;
        } else {
            printf("  native==crypto: FAIL\n"); fail++;
        }
    }

    printf("\n=== RFC 7748: %d passed, %d failed ===\n\n", pass, fail);
    return fail;
}

/* ════════════════════════════════════════════════════════════════════
 * BENCHMARKING
 * ════════════════════════════════════════════════════════════════════ */

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

#define BENCH_REPS 100

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Sort samples in place and return (min, median). */
static void bench_stats(uint64_t *samples, int n, uint64_t *out_min, uint64_t *out_median) {
    qsort(samples, n, sizeof(uint64_t), cmp_u64);
    *out_min    = samples[0];
    *out_median = samples[n / 2];
}

/* Micro-bench the cswap variants directly.
 *
 * Runs 1000 cswap pairs (x2,x3) + (z2,z3) per rep — i.e. 2000 cswap
 * calls — matching the X25519 ladder's per-call density (≈512/X25519
 * × 4× to get a stable sample). Tests both swap=0 and swap=1 to make
 * sure the constant-time path isn't optimized away.
 */
static void cswap_microbench(void) {
    uint64_t t0, t1, mn, med;
    uint64_t samples[BENCH_REPS];
    fe a, b;
    /* arbitrary fixed bit pattern, alternating */
    for (int i = 0; i < 5; i++) { a[i] = 0x1234567890abcdefULL ^ (i*0x111); b[i] = ~a[i]; }

    printf("=== fe_cswap microbench (2000 calls/rep) ===\n");

    /* SSE2 */
    for (int r = 0; r < BENCH_REPS; r++) {
        t0 = rdtsc_start();
        for (int i = 0; i < 1000; i++) {
            fe_cswap_sse2(a, b, (uint64_t)(i & 1));
            fe_cswap_sse2(a, b, (uint64_t)((i ^ 1) & 1));
        }
        t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    bench_stats(samples, BENCH_REPS, &mn, &med);
    printf("  sse2  : min %6" PRIu64 "  median %6" PRIu64 "  cyc/2000-calls   (%.2f cyc/call)\n",
           mn, med, (double)mn / 2000.0);

    /* Scalar */
    for (int r = 0; r < BENCH_REPS; r++) {
        t0 = rdtsc_start();
        for (int i = 0; i < 1000; i++) {
            fe_cswap_scalar(a, b, (uint64_t)(i & 1));
            fe_cswap_scalar(a, b, (uint64_t)((i ^ 1) & 1));
        }
        t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    bench_stats(samples, BENCH_REPS, &mn, &med);
    printf("  scalar: min %6" PRIu64 "  median %6" PRIu64 "  cyc/2000-calls   (%.2f cyc/call)\n",
           mn, med, (double)mn / 2000.0);

    /* Anti-DCE: ensure the compiler can't elide the work */
    volatile uint64_t sink = 0;
    for (int i = 0; i < 5; i++) sink ^= a[i] ^ b[i];
    (void)sink;
    printf("\n");
}

/* Measure cost of N back-to-back fe_sq_ucode calls.
 * Comparing:
 *   100 separate fe_sq_ucode calls (mem round-trip between each)
 *   1 fe_sq_ucode_n call with n=100 (registers chained)
 * The delta is per-iter wrapper savings × 99. */
static void fe_sq_chain_microbench(void) {
    uint64_t t0, t1, mn, med;
    uint64_t samples[BENCH_REPS];
    fe x;
    /* Small valid limbs */
    for (int i = 0; i < 5; i++) x[i] = (uint64_t)(0x123456ULL + i);

    printf("=== fe_sq chaining microbench (100 sequential squarings) ===\n");

    /* Method A: 100 separate calls */
    for (int r = 0; r < BENCH_REPS; r++) {
        fe tmp; memcpy(tmp, x, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < 100; i++) fe_sq_ucode(tmp, tmp);
        t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    bench_stats(samples, BENCH_REPS, &mn, &med);
    uint64_t mn_separate = mn;
    printf("  100 × fe_sq_ucode (separate): min %6" PRIu64 "  median %6" PRIu64
           "   (%.1f cyc/call)\n", mn, med, (double)mn / 100.0);

    /* Method B: 1 chained call with n=100 */
    for (int r = 0; r < BENCH_REPS; r++) {
        fe tmp; memcpy(tmp, x, sizeof(tmp));
        t0 = rdtsc_start();
        fe_sq_ucode_n(tmp, tmp, 100);
        t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    bench_stats(samples, BENCH_REPS, &mn, &med);
    uint64_t mn_chained = mn;
    printf("  1 × fe_sq_ucode_n(100):       min %6" PRIu64 "  median %6" PRIu64
           "   (%.1f cyc/iter)\n", mn, med, (double)mn / 100.0);

    int64_t saved = (int64_t)mn_separate - (int64_t)mn_chained;
    printf("  chaining saves: %+" PRId64 " cyc total (%.1f cyc/iter)\n\n",
           saved, (double)saved / 99.0);

    /* Verify correctness: 100 fe_sq is equivalent to fe_sq_ucode_n(100) */
    fe ref, chained;
    memcpy(ref, x, sizeof(ref));
    memcpy(chained, x, sizeof(chained));
    for (int i = 0; i < 100; i++) fe_sq_ucode(ref, ref);
    fe_sq_ucode_n(chained, chained, 100);
    int ok = memcmp(ref, chained, sizeof(ref)) == 0;
    printf("  correctness vs reference: %s\n\n", ok ? "PASS" : "FAIL");
}

static void benchmark(void) {
    uint8_t scalar[32] = {0}, point[32] = {0}, out[32];
    uint64_t t0, t1, mn, med;
    uint64_t samples[BENCH_REPS];

    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
                 scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
                 point, 32);

    printf("=== X25519 Benchmark (%d repetitions) ===\n\n", BENCH_REPS);

    /* Warm up */
    x25519_native(out, scalar, point);
    x25519_fiat(out, scalar, point);
    x25519_cryptopt(out, scalar, point);
    x25519_donna_c64(out, scalar, point);
    x25519_amd64_51(out, scalar, point);
    x25519_amd64_51_ucode(out, scalar, point);
    x25519_amd64_64(out, scalar, point);
    x25519_ucode(out, scalar, point);

    /* Benchmark native C */
    for (int r = 0; r < BENCH_REPS; r++) {
        t0 = rdtsc_start();
        x25519_native(out, scalar, point);
        t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    bench_stats(samples, BENCH_REPS, &mn, &med);
    printf("ours/hand-C:          min %8" PRIu64 "  median %8" PRIu64 " cycles\n", mn, med);

    /* Benchmark ours/fiat */
    for (int r = 0; r < BENCH_REPS; r++) {
        t0 = rdtsc_start();
        x25519_fiat(out, scalar, point);
        t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    bench_stats(samples, BENCH_REPS, &mn, &med);
    printf("ours/fiat:            min %8" PRIu64 "  median %8" PRIu64 " cycles\n", mn, med);

    /* Benchmark ours/cryptopt (our ladder + CryptOpt Goldmont-tuned asm field ops) */
    for (int r = 0; r < BENCH_REPS; r++) {
        t0 = rdtsc_start();
        x25519_cryptopt(out, scalar, point);
        t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    bench_stats(samples, BENCH_REPS, &mn, &med);
    printf("ours/cryptopt:        min %8" PRIu64 "  median %8" PRIu64 " cycles\n", mn, med);

    /* Benchmark donna_c64 (whole stack) */
    for (int r = 0; r < BENCH_REPS; r++) {
        t0 = rdtsc_start();
        x25519_donna_c64(out, scalar, point);
        t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    bench_stats(samples, BENCH_REPS, &mn, &med);
    printf("donna_c64:            min %8" PRIu64 "  median %8" PRIu64 " cycles\n", mn, med);

    /* Benchmark amd64-51/asm (SUPERCOP whole stack) */
    for (int r = 0; r < BENCH_REPS; r++) {
        t0 = rdtsc_start();
        x25519_amd64_51(out, scalar, point);
        t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    bench_stats(samples, BENCH_REPS, &mn, &med);
    printf("amd64-51/asm:         min %8" PRIu64 "  median %8" PRIu64 " cycles\n", mn, med);

    /* Benchmark amd64-51/ucode (amd64-51 framework + microcode field ops) */
    for (int r = 0; r < BENCH_REPS; r++) {
        t0 = rdtsc_start();
        x25519_amd64_51_ucode(out, scalar, point);
        t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    bench_stats(samples, BENCH_REPS, &mn, &med);
    printf("amd64-51/ucode:       min %8" PRIu64 "  median %8" PRIu64 " cycles\n", mn, med);

    /* Benchmark amd64-64/asm (SUPERCOP whole stack, 4x64 saturated;
     * what lib25519's autotuner selects on Goldmont) */
    for (int r = 0; r < BENCH_REPS; r++) {
        t0 = rdtsc_start();
        x25519_amd64_64(out, scalar, point);
        t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    bench_stats(samples, BENCH_REPS, &mn, &med);
    printf("amd64-64/asm:         min %8" PRIu64 "  median %8" PRIu64 " cycles\n", mn, med);

    /* Benchmark ours/ucode (our ladder + microcode field ops) */
    for (int r = 0; r < BENCH_REPS; r++) {
        t0 = rdtsc_start();
        x25519_ucode(out, scalar, point);
        t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    bench_stats(samples, BENCH_REPS, &mn, &med);
    printf("ours/ucode:           min %8" PRIu64 "  median %8" PRIu64 " cycles\n", mn, med);

    printf("\n");
}

/* ════════════════════════════════════════════════════════════════════
 * MAIN
 * ════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("=== Full X25519: microcode vs native C ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();

    /* Diagnostic: does our hybrid's C ladderstep produce the same field
     * element values as amd64-51's asm ladderstep on the same input?
     *
     * We compare via fe25519_pack output (32 canonical bytes), which
     * freezes the limbs first — so two different limb representations
     * of the same field element compare equal here. */
    {
        typedef struct { unsigned long long v[5]; } sc_fe25519;
        extern void supercop_amd64_51_ladderstep(void *work);
        extern void supercop_amd64_51_ucode_ladderstep(void *work);
        extern void supercop_amd64_51_fe25519_pack(unsigned char r[32], const void *x);
        extern void supercop_amd64_51_ucode_fe25519_pack(unsigned char r[32], const void *x);

        printf("--- Ladderstep equivalence diagnostic ---\n");
        printf("  Initial work[] = small non-degenerate values; one ladderstep each.\n");
        printf("  Comparing pack(x2), pack(z2), pack(x3), pack(z3) byte-for-byte.\n");

        sc_fe25519 work_asm[5];
        memset(work_asm, 0, sizeof(work_asm));
        work_asm[0].v[0] = 9;        /* x1 */
        work_asm[1].v[0] = 5;        /* x2 */
        work_asm[2].v[0] = 7;        /* z2 */
        work_asm[3].v[0] = 11;       /* x3 */
        work_asm[4].v[0] = 13;       /* z3 */

        sc_fe25519 work_ucode[5];
        memcpy(work_ucode, work_asm, sizeof(work_asm));

        supercop_amd64_51_ladderstep(work_asm);
        supercop_amd64_51_ucode_ladderstep(work_ucode);

        int all_field_match = 1;
        int all_proj_match = 1;
        const char *names[5] = {"x1", "x2", "z2", "x3", "z3"};
        unsigned char b_asm[5][32], b_ucode[5][32];
        for (int i = 0; i < 5; i++) {
            supercop_amd64_51_fe25519_pack(b_asm[i],  &work_asm[i]);
            supercop_amd64_51_ucode_fe25519_pack(b_ucode[i], &work_ucode[i]);
            int match = (memcmp(b_asm[i], b_ucode[i], 32) == 0);
            printf("  work[%d] (%s): %s\n", i, names[i],
                   match ? "MATCH (field elem equal)" : "DIFFER (field elem differ)");
            if (!match) all_field_match = 0;
        }

        /* If field elements differ, check projective equivalence: x2/z2 and
         * x3/z3 ratios should still be equal. Two points are projectively
         * equal iff x2_a * z2_b == x2_b * z2_a (mod p). We can't compute
         * that directly without fe25519_mul, so skip — but if X25519
         * end-to-end matches in the RFC test below, projective equivalence
         * is implied. */

        if (all_field_match) {
            printf("  → Ladders produce IDENTICAL field elements (bit-for-bit\n"
                   "    after canonicalization). Our hybrid is a faithful\n"
                   "    drop-in for amd64-51's ladderstep.\n");
        } else {
            printf("  → Ladders produce DIFFERENT field elements. The two\n"
                   "    formulations may be projectively equivalent (same\n"
                   "    affine point x/z) but with different (x,z) scalings.\n"
                   "    The end-to-end RFC 7748 test below will confirm whether\n"
                   "    X25519 is still correct despite the divergence.\n");
            (void)all_proj_match;
        }
        printf("\n");
    }

    /* Quick diagnostic: verify fe_invert and encode/decode */
    {
        printf("--- Diagnostics ---\n");
        fe x = {7, 0, 0, 0, 0};
        fe inv_x, product;
        fe_invert_native(inv_x, x);
        fe_mul_native(x, inv_x, product);
        fe_reduce(product, product);
        printf("  inv(7)*7 = {%lu,%lu,%lu,%lu,%lu} (expect {1,0,0,0,0})\n",
               product[0], product[1], product[2], product[3], product[4]);

        uint8_t bytes[32];
        fe decoded;
        fe orig = {0x123456789ABULL, 0x23456789ABCULL, 0x3456789ABCDULL,
                   0x456789ABCDEULL, 0x56789ABCDEFULL};
        fe_tobytes(bytes, orig);
        fe_frombytes(decoded, bytes);
        printf("  encode/decode: {%lx,%lx,%lx,%lx,%lx} -> {%lx,%lx,%lx,%lx,%lx} %s\n",
               orig[0], orig[1], orig[2], orig[3], orig[4],
               decoded[0], decoded[1], decoded[2], decoded[3], decoded[4],
               memcmp(orig, decoded, 40) == 0 ? "MATCH" : "MISMATCH");

        uint8_t sc9[32] = {0}, bp9[32] = {0}, result9[32];
        sc9[0] = 9; bp9[0] = 9;
        x25519_native(result9, sc9, bp9);
        printf("  x25519(9, 9) = ");
        for (int i = 0; i < 32; i++) printf("%02x", result9[i]);
        printf("\n  (expect 422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079)\n");
        printf("\n");
    }

    int failures = test_rfc7748();

    if (failures) {
        printf("Test vectors FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    cswap_microbench();
    fe_sq_chain_microbench();
    benchmark();

    init_match_and_patch();
    do_fix_IN_patch();
    printf("Done.\n");
    return 0;
}
