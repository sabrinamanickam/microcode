/*
 * asm_op_p521_sq.c — P-521 field squaring via microcode (vmwrite) vs native C
 *
 * Field: GF(2^521 - 1),  unsaturated radix-2^58,  9 limbs (limb 8 = 57 bits).
 *
 * Squaring formula (45 products, 5 per limb):
 *   c0 = a0^2       + 4*a1*a8 + 4*a2*a7 + 4*a3*a6 + 4*a4*a5
 *   c1 = 2*a0*a1    + 4*a2*a8 + 4*a3*a7 + 4*a4*a6 + 2*a5^2
 *   c2 = 2*a0*a2    + a1^2    + 4*a3*a8 + 4*a4*a7 + 4*a5*a6
 *   c3 = 2*a0*a3    + 2*a1*a2 + 4*a4*a8 + 4*a5*a7 + 2*a6^2
 *   c4 = 2*a0*a4    + 2*a1*a3 + a2^2    + 4*a5*a8 + 4*a6*a7
 *   c5 = 2*a0*a5    + 2*a1*a4 + 2*a2*a3 + 4*a6*a8 + 2*a7^2
 *   c6 = 2*a0*a6    + 2*a1*a5 + 2*a2*a4 + a3^2    + 4*a7*a8
 *   c7 = 2*a0*a7    + 2*a1*a6 + 2*a2*a5 + 2*a3*a4 + 2*a8^2
 *   c8 = 2*a0*a8    + 2*a1*a7 + 2*a2*a6 + 2*a3*a5 + a4^2
 *
 * Reduction: 2^522 = 2 mod p (Mersenne). Cross-terms that wrap get ×2
 * (reduction) plus ×2 (squaring symmetry) = ×4.
 *
 * Architecture: single vmwrite — all 9 limbs computed in one patch.
 * The inline asm copies a[0..8] into RDI..R13, fires vmwrite once,
 * and reads c[0..8] from the same registers plus carry from RAX.
 * Wrap-around carry (c8->c0->c1) done in native C.
 *
 * Patch register convention (114-triad optimized version):
 *   Input:  a[0..8] in RDI, RSI, R12, R11, R14, RBX, RBP, R15, R13
 *   PREP copies to TMP0..TMP8, precomputes 2*a[5..8] in TMP9..TMP12
 *   Per limb: Dettman MAC chain (5 products) with progressive hi accum
 *     TMP13 = lo accumulator, TMP15 = SETCC carry
 *     R8 = progressive hi accumulator, R10 = progressive carry accumulator
 *     R9 = lo carry bits (TMP13>>58), RAX = hi carry bits (R8<<6)
 *     RCX/RDX = MUL scratch (hi/lo)
 *   Output: c[0..8] in RDI, RSI, R12, R11, R14, RBX, RBP, R15, R13
 *           (UNMASKED — native C applies MASK58/MASK57)
 *           RAX = carry from last limb (58-bit extraction)
 *
 * Build:  make PROG=asm_op_p521_sq
 * Run:    sudo taskset -c 0 ./asm_op_p521_sq_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MASK58 UINT64_C(0x3ffffffffffffff)
#define MASK57 UINT64_C(0x1ffffffffffffff)
#define NLIMBS 9

/* ── fiat-crypto reference (the REAL GCC baseline CryptOpt compares against) ── */
#include "../curvesC/p521_square.c"

static void fe_sq_fiat(const uint64_t *a, uint64_t *out) {
    fiat_p521_carry_square(out, a);
}

/* ── microcode patch (all 9 limbs in one patch) ──────────────── */

static void install_p521_sq_patch(void) {
    ucode_t patch[] = {

    /*
     * Optimized single-vmwrite P-521 squaring: 114 triads.
     *
     * Key optimizations vs previous 156-triad version:
     * 1. Progressive hi accumulation: R8 accumulates MUL hi values,
     *    R10 accumulates SETCC carries during the MAC chain (not after).
     * 2. Slot 0→2 value RAW (confirmed): lastACC absorbs SHR carry
     *    extraction in slot 2 reading TMP13 from slot 0's ADD result.
     * 3. Slot 0→2 value RAW in CE-1: SHL reads R8 from slot 0's ADD.
     * 4. 3-triad transition (CE-0, CE-1, LINK+MAC1) instead of 4.
     *    Output stored unmasked; native C applies MASK58/MASK57.
     * 5. MAC2-hi uses ZEROEXT(R10,TMP15) to initialize R10 from first
     *    carry, avoiding separate R10 zeroing.
     *
     * Per-limb structure (12 triads for c0 and middle limbs):
     *   LINK+MAC1 (or init for c0): OR carry→TMP13, MUL, zero R8
     *   ACC1: ADD lo, SETCC, prep next RDX
     *   MAC2-hi: ADD R8+=hi, MUL, ZEROEXT R10=carry (init)
     *   ACC2: ADD lo, SETCC, prep next RDX
     *   MAC3-hi: ADD R8+=hi, MUL, ADD R10+=carry
     *   ACC3: ADD lo, SETCC, prep next RDX
     *   MAC4-hi: ADD R8+=hi, MUL, ADD R10+=carry
     *   ACC4: ADD lo, SETCC, prep next RDX
     *   MAC5-hi: ADD R8+=hi, MUL, ADD R10+=carry
     *   lastACC: ADD lo, SETCC, SHR R9=lo>>58 (slot 0→2 RAW!)
     *   CE-0: ADD R8+=hi, ADD R10+=carry, prep next limb RDX
     *   CE-1: ADD R8+=R10, ZEROEXT output=TMP13, SHL RAX=R8<<6 (slot 0→2 RAW!)
     *
     * Triad count: 5 (PREP) + 12 (c0) + 7×12 (c1-c7) + 13 (c8) = 114
     */

    /* ═══ PREP: copy a[0..8] → TMP0..TMP8, precompute 2*a[5..8] → TMP9..TMP12 ═══ */
    /* P0 */ { ZEROEXT_DSZ64_DR(TMP0, RDI),
               ZEROEXT_DSZ64_DR(TMP1, RSI),
               ZEROEXT_DSZ64_DR(TMP2, R12),
               NOP_SEQWORD },
    /* P1 */ { ZEROEXT_DSZ64_DR(TMP3, R11),
               ZEROEXT_DSZ64_DR(TMP4, R14),
               ZEROEXT_DSZ64_DR(TMP5, RBX),
               NOP_SEQWORD },
    /* P2 */ { ZEROEXT_DSZ64_DR(TMP6, RBP),
               ZEROEXT_DSZ64_DR(TMP7, R15),
               ZEROEXT_DSZ64_DR(TMP8, R13),
               NOP_SEQWORD },
    /* P3 */ { ADD_DSZ64_DRR(TMP9, TMP5, TMP5),     /* 2*a5 */
               ADD_DSZ64_DRR(TMP10, TMP6, TMP6),    /* 2*a6 */
               ADD_DSZ64_DRR(TMP11, TMP7, TMP7),    /* 2*a7 */
               NOP_SEQWORD },
    /* P4 */ { ADD_DSZ64_DRR(TMP12, TMP8, TMP8),    /* 2*a8 */
               NOTAND_DSZ64_DRR(R8, R8, R8),        /* R8 = 0 */
               ZEROEXT_DSZ64_DR(RDX, TMP0),         /* prep a0 for c0 MAC1 */
               NOP_SEQWORD },

    /* ═══ LIMB c0 = a0² + 4·a1·a8 + 4·a2·a7 + 4·a3·a6 + 4·a4·a5 ═══ */

    /* C0-0: init acc=0, MAC1 = a0 × a0 */
    /* C0-0 */ { NOTAND_DSZ64_DRR(TMP13, TMP13, TMP13), /* acc = 0 */
                 MUL_DSZ64_DRR(RCX, TMP0, RDX),
                 NOP, NOP_SEQWORD },
    /* C0-1: ACC1 + prep 4*a8 */
    /* C0-1 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP12, TMP12),  /* 4*a8 */
                 NOP_SEQWORD },
    /* C0-2: hi1 + MAC2 = a1 × 4a8, init R10=carry1 */
    /* C0-2 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP1, RDX),
                 ZEROEXT_DSZ64_DR(R10, TMP15),       /* R10 = carry1 */
                 NOP_SEQWORD },
    /* C0-3: ACC2 + prep 4*a7 */
    /* C0-3 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP11, TMP11),  /* 4*a7 */
                 NOP_SEQWORD },
    /* C0-4: hi2 + MAC3 = a2 × 4a7 */
    /* C0-4 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP2, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C0-5: ACC3 + prep 4*a6 */
    /* C0-5 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP10, TMP10),  /* 4*a6 */
                 NOP_SEQWORD },
    /* C0-6: hi3 + MAC4 = a3 × 4a6 */
    /* C0-6 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP3, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C0-7: ACC4 + prep 4*a5 */
    /* C0-7 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP9, TMP9),    /* 4*a5 */
                 NOP_SEQWORD },
    /* C0-8: hi4 + MAC5 = a4 × 4a5 */
    /* C0-8 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP4, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C0-9: lastACC5 — slot 0→2 RAW: SHR reads TMP13 from slot 0's ADD */
    /* C0-9 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 SHR_DSZ64_DRI(R9, TMP13, 58),      /* lo carry bits */
                 NOP_SEQWORD },
    /* C0-10: CE-0: accumulate last hi+carry, prep 2*a1 for c1 */
    /* C0-10 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                  ADD_DSZ64_DRR(R10, R10, TMP15),
                  ADD_DSZ64_DRR(RDX, TMP1, TMP1),   /* 2*a1 for c1 MAC1 */
                  NOP_SEQWORD },
    /* C0-11: CE-1: combine R8+R10, output c0→RDI (unmasked), hi carry bits
     *   slot 0→2 RAW: SHL reads R8 from slot 0's ADD result */
    /* C0-11 */ { ADD_DSZ64_DRR(R8, R8, R10),
                  ZEROEXT_DSZ64_DR(RDI, TMP13),      /* c0 → RDI (unmasked) */
                  SHL_DSZ64_DRI(RAX, R8, 6),         /* hi carry bits */
                  NOP_SEQWORD },

    /* ═══ LIMB c1 = 2·a0·a1 + 4·a2·a8 + 4·a3·a7 + 4·a4·a6 + 2·a5² ═══ */

    /* C1-0: LINK+MAC1: carry→TMP13, a0 × 2a1, zero R8 */
    /* C1-0 */ { OR_DSZ64_DRR(TMP13, R9, RAX),
                 MUL_DSZ64_DRR(RCX, TMP0, RDX),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 NOP_SEQWORD },
    /* C1-1: ACC1 + prep 4*a8 */
    /* C1-1 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP12, TMP12),  /* 4*a8 */
                 NOP_SEQWORD },
    /* C1-2: hi1 + MAC2 = a2 × 4a8, init R10 */
    /* C1-2 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP2, RDX),
                 ZEROEXT_DSZ64_DR(R10, TMP15),
                 NOP_SEQWORD },
    /* C1-3: ACC2 + prep 4*a7 */
    /* C1-3 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP11, TMP11),  /* 4*a7 */
                 NOP_SEQWORD },
    /* C1-4: hi2 + MAC3 = a3 × 4a7 */
    /* C1-4 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP3, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C1-5: ACC3 + prep 4*a6 */
    /* C1-5 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP10, TMP10),  /* 4*a6 */
                 NOP_SEQWORD },
    /* C1-6: hi3 + MAC4 = a4 × 4a6 */
    /* C1-6 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP4, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C1-7: ACC4 + prep 2*a5 (for ×2 diagonal) */
    /* C1-7 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP9),       /* 2*a5 */
                 NOP_SEQWORD },
    /* C1-8: hi4 + MAC5 = a5 × 2a5 */
    /* C1-8 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP5, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C1-9: lastACC — slot 0→2 RAW */
    /* C1-9 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 SHR_DSZ64_DRI(R9, TMP13, 58),
                 NOP_SEQWORD },
    /* C1-10: CE-0 + prep 2*a2 for c2 */
    /* C1-10 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                  ADD_DSZ64_DRR(R10, R10, TMP15),
                  ADD_DSZ64_DRR(RDX, TMP2, TMP2),   /* 2*a2 */
                  NOP_SEQWORD },
    /* C1-11: CE-1: output c1→RSI (unmasked) */
    /* C1-11 */ { ADD_DSZ64_DRR(R8, R8, R10),
                  ZEROEXT_DSZ64_DR(RSI, TMP13),      /* c1 → RSI */
                  SHL_DSZ64_DRI(RAX, R8, 6),
                  NOP_SEQWORD },

    /* ═══ LIMB c2 = 2·a0·a2 + a1² + 4·a3·a8 + 4·a4·a7 + 4·a5·a6 ═══ */

    /* C2-0: LINK+MAC1: a0 × 2a2 */
    /* C2-0 */ { OR_DSZ64_DRR(TMP13, R9, RAX),
                 MUL_DSZ64_DRR(RCX, TMP0, RDX),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 NOP_SEQWORD },
    /* C2-1: ACC1 + prep a1 (×1 diagonal) */
    /* C2-1 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP1),       /* a1 */
                 NOP_SEQWORD },
    /* C2-2: hi1 + MAC2 = a1 × a1, init R10 */
    /* C2-2 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP1, RDX),
                 ZEROEXT_DSZ64_DR(R10, TMP15),
                 NOP_SEQWORD },
    /* C2-3: ACC2 + prep 4*a8 */
    /* C2-3 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP12, TMP12),  /* 4*a8 */
                 NOP_SEQWORD },
    /* C2-4: hi2 + MAC3 = a3 × 4a8 */
    /* C2-4 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP3, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C2-5: ACC3 + prep 4*a7 */
    /* C2-5 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP11, TMP11),  /* 4*a7 */
                 NOP_SEQWORD },
    /* C2-6: hi3 + MAC4 = a4 × 4a7 */
    /* C2-6 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP4, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C2-7: ACC4 + prep 4*a6 */
    /* C2-7 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP10, TMP10),  /* 4*a6 */
                 NOP_SEQWORD },
    /* C2-8: hi4 + MAC5 = a5 × 4a6 */
    /* C2-8 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP5, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C2-9: lastACC — slot 0→2 RAW */
    /* C2-9 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 SHR_DSZ64_DRI(R9, TMP13, 58),
                 NOP_SEQWORD },
    /* C2-10: CE-0 + prep 2*a3 for c3 */
    /* C2-10 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                  ADD_DSZ64_DRR(R10, R10, TMP15),
                  ADD_DSZ64_DRR(RDX, TMP3, TMP3),   /* 2*a3 */
                  NOP_SEQWORD },
    /* C2-11: CE-1: output c2→R12 */
    /* C2-11 */ { ADD_DSZ64_DRR(R8, R8, R10),
                  ZEROEXT_DSZ64_DR(R12, TMP13),      /* c2 → R12 */
                  SHL_DSZ64_DRI(RAX, R8, 6),
                  NOP_SEQWORD },

    /* ═══ LIMB c3 = 2·a0·a3 + 2·a1·a2 + 4·a4·a8 + 4·a5·a7 + 2·a6² ═══ */

    /* C3-0: LINK+MAC1: a0 × 2a3 */
    /* C3-0 */ { OR_DSZ64_DRR(TMP13, R9, RAX),
                 MUL_DSZ64_DRR(RCX, TMP0, RDX),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 NOP_SEQWORD },
    /* C3-1: ACC1 + prep 2*a2 */
    /* C3-1 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP2, TMP2),    /* 2*a2 */
                 NOP_SEQWORD },
    /* C3-2: hi1 + MAC2 = a1 × 2a2, init R10 */
    /* C3-2 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP1, RDX),
                 ZEROEXT_DSZ64_DR(R10, TMP15),
                 NOP_SEQWORD },
    /* C3-3: ACC2 + prep 4*a8 */
    /* C3-3 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP12, TMP12),  /* 4*a8 */
                 NOP_SEQWORD },
    /* C3-4: hi2 + MAC3 = a4 × 4a8 */
    /* C3-4 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP4, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C3-5: ACC3 + prep 4*a7 */
    /* C3-5 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP11, TMP11),  /* 4*a7 */
                 NOP_SEQWORD },
    /* C3-6: hi3 + MAC4 = a5 × 4a7 */
    /* C3-6 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP5, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C3-7: ACC4 + prep 2*a6 (for ×2 diagonal) */
    /* C3-7 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),      /* 2*a6 */
                 NOP_SEQWORD },
    /* C3-8: hi4 + MAC5 = a6 × 2a6 */
    /* C3-8 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP6, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C3-9: lastACC — slot 0→2 RAW */
    /* C3-9 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 SHR_DSZ64_DRI(R9, TMP13, 58),
                 NOP_SEQWORD },
    /* C3-10: CE-0 + prep 2*a4 for c4 */
    /* C3-10 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                  ADD_DSZ64_DRR(R10, R10, TMP15),
                  ADD_DSZ64_DRR(RDX, TMP4, TMP4),   /* 2*a4 */
                  NOP_SEQWORD },
    /* C3-11: CE-1: output c3→R11 */
    /* C3-11 */ { ADD_DSZ64_DRR(R8, R8, R10),
                  ZEROEXT_DSZ64_DR(R11, TMP13),      /* c3 → R11 */
                  SHL_DSZ64_DRI(RAX, R8, 6),
                  NOP_SEQWORD },

    /* ═══ LIMB c4 = 2·a0·a4 + 2·a1·a3 + a2² + 4·a5·a8 + 4·a6·a7 ═══ */

    /* C4-0: LINK+MAC1: a0 × 2a4 */
    /* C4-0 */ { OR_DSZ64_DRR(TMP13, R9, RAX),
                 MUL_DSZ64_DRR(RCX, TMP0, RDX),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 NOP_SEQWORD },
    /* C4-1: ACC1 + prep 2*a3 */
    /* C4-1 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP3, TMP3),    /* 2*a3 */
                 NOP_SEQWORD },
    /* C4-2: hi1 + MAC2 = a1 × 2a3, init R10 */
    /* C4-2 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP1, RDX),
                 ZEROEXT_DSZ64_DR(R10, TMP15),
                 NOP_SEQWORD },
    /* C4-3: ACC2 + prep a2 (×1 diagonal) */
    /* C4-3 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP2),       /* a2 */
                 NOP_SEQWORD },
    /* C4-4: hi2 + MAC3 = a2 × a2 */
    /* C4-4 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP2, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C4-5: ACC3 + prep 4*a8 */
    /* C4-5 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP12, TMP12),  /* 4*a8 */
                 NOP_SEQWORD },
    /* C4-6: hi3 + MAC4 = a5 × 4a8 */
    /* C4-6 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP5, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C4-7: ACC4 + prep 4*a7 */
    /* C4-7 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP11, TMP11),  /* 4*a7 */
                 NOP_SEQWORD },
    /* C4-8: hi4 + MAC5 = a6 × 4a7 */
    /* C4-8 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP6, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C4-9: lastACC — slot 0→2 RAW */
    /* C4-9 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 SHR_DSZ64_DRI(R9, TMP13, 58),
                 NOP_SEQWORD },
    /* C4-10: CE-0 + prep 2*a5 for c5 */
    /* C4-10 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                  ADD_DSZ64_DRR(R10, R10, TMP15),
                  ZEROEXT_DSZ64_DR(RDX, TMP9),       /* 2*a5 */
                  NOP_SEQWORD },
    /* C4-11: CE-1: output c4→R14 */
    /* C4-11 */ { ADD_DSZ64_DRR(R8, R8, R10),
                  ZEROEXT_DSZ64_DR(R14, TMP13),      /* c4 → R14 */
                  SHL_DSZ64_DRI(RAX, R8, 6),
                  NOP_SEQWORD },

    /* ═══ LIMB c5 = 2·a0·a5 + 2·a1·a4 + 2·a2·a3 + 4·a6·a8 + 2·a7² ═══ */

    /* C5-0: LINK+MAC1: a0 × 2a5 */
    /* C5-0 */ { OR_DSZ64_DRR(TMP13, R9, RAX),
                 MUL_DSZ64_DRR(RCX, TMP0, RDX),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 NOP_SEQWORD },
    /* C5-1: ACC1 + prep 2*a4 */
    /* C5-1 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP4, TMP4),    /* 2*a4 */
                 NOP_SEQWORD },
    /* C5-2: hi1 + MAC2 = a1 × 2a4, init R10 */
    /* C5-2 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP1, RDX),
                 ZEROEXT_DSZ64_DR(R10, TMP15),
                 NOP_SEQWORD },
    /* C5-3: ACC2 + prep 2*a3 */
    /* C5-3 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP3, TMP3),    /* 2*a3 */
                 NOP_SEQWORD },
    /* C5-4: hi2 + MAC3 = a2 × 2a3 */
    /* C5-4 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP2, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C5-5: ACC3 + prep 4*a8 */
    /* C5-5 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP12, TMP12),  /* 4*a8 */
                 NOP_SEQWORD },
    /* C5-6: hi3 + MAC4 = a6 × 4a8 */
    /* C5-6 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP6, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C5-7: ACC4 + prep 2*a7 (for ×2 diagonal) */
    /* C5-7 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP11),      /* 2*a7 */
                 NOP_SEQWORD },
    /* C5-8: hi4 + MAC5 = a7 × 2a7 */
    /* C5-8 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP7, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C5-9: lastACC — slot 0→2 RAW */
    /* C5-9 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 SHR_DSZ64_DRI(R9, TMP13, 58),
                 NOP_SEQWORD },
    /* C5-10: CE-0 + prep 2*a6 for c6 */
    /* C5-10 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                  ADD_DSZ64_DRR(R10, R10, TMP15),
                  ZEROEXT_DSZ64_DR(RDX, TMP10),      /* 2*a6 */
                  NOP_SEQWORD },
    /* C5-11: CE-1: output c5→RBX */
    /* C5-11 */ { ADD_DSZ64_DRR(R8, R8, R10),
                  ZEROEXT_DSZ64_DR(RBX, TMP13),      /* c5 → RBX */
                  SHL_DSZ64_DRI(RAX, R8, 6),
                  NOP_SEQWORD },

    /* ═══ LIMB c6 = 2·a0·a6 + 2·a1·a5 + 2·a2·a4 + a3² + 4·a7·a8 ═══ */

    /* C6-0: LINK+MAC1: a0 × 2a6 */
    /* C6-0 */ { OR_DSZ64_DRR(TMP13, R9, RAX),
                 MUL_DSZ64_DRR(RCX, TMP0, RDX),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 NOP_SEQWORD },
    /* C6-1: ACC1 + prep 2*a5 */
    /* C6-1 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP9),       /* 2*a5 */
                 NOP_SEQWORD },
    /* C6-2: hi1 + MAC2 = a1 × 2a5, init R10 */
    /* C6-2 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP1, RDX),
                 ZEROEXT_DSZ64_DR(R10, TMP15),
                 NOP_SEQWORD },
    /* C6-3: ACC2 + prep 2*a4 */
    /* C6-3 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP4, TMP4),    /* 2*a4 */
                 NOP_SEQWORD },
    /* C6-4: hi2 + MAC3 = a2 × 2a4 */
    /* C6-4 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP2, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C6-5: ACC3 + prep a3 (×1 diagonal) */
    /* C6-5 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP3),       /* a3 */
                 NOP_SEQWORD },
    /* C6-6: hi3 + MAC4 = a3 × a3 */
    /* C6-6 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP3, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C6-7: ACC4 + prep 4*a8 */
    /* C6-7 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP12, TMP12),  /* 4*a8 */
                 NOP_SEQWORD },
    /* C6-8: hi4 + MAC5 = a7 × 4a8 */
    /* C6-8 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP7, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C6-9: lastACC — slot 0→2 RAW */
    /* C6-9 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 SHR_DSZ64_DRI(R9, TMP13, 58),
                 NOP_SEQWORD },
    /* C6-10: CE-0 + prep 2*a7 for c7 */
    /* C6-10 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                  ADD_DSZ64_DRR(R10, R10, TMP15),
                  ZEROEXT_DSZ64_DR(RDX, TMP11),      /* 2*a7 */
                  NOP_SEQWORD },
    /* C6-11: CE-1: output c6→RBP */
    /* C6-11 */ { ADD_DSZ64_DRR(R8, R8, R10),
                  ZEROEXT_DSZ64_DR(RBP, TMP13),      /* c6 → RBP */
                  SHL_DSZ64_DRI(RAX, R8, 6),
                  NOP_SEQWORD },

    /* ═══ LIMB c7 = 2·a0·a7 + 2·a1·a6 + 2·a2·a5 + 2·a3·a4 + 2·a8² ═══ */

    /* C7-0: LINK+MAC1: a0 × 2a7 */
    /* C7-0 */ { OR_DSZ64_DRR(TMP13, R9, RAX),
                 MUL_DSZ64_DRR(RCX, TMP0, RDX),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 NOP_SEQWORD },
    /* C7-1: ACC1 + prep 2*a6 */
    /* C7-1 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),      /* 2*a6 */
                 NOP_SEQWORD },
    /* C7-2: hi1 + MAC2 = a1 × 2a6, init R10 */
    /* C7-2 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP1, RDX),
                 ZEROEXT_DSZ64_DR(R10, TMP15),
                 NOP_SEQWORD },
    /* C7-3: ACC2 + prep 2*a5 */
    /* C7-3 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP9),       /* 2*a5 */
                 NOP_SEQWORD },
    /* C7-4: hi2 + MAC3 = a2 × 2a5 */
    /* C7-4 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP2, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C7-5: ACC3 + prep 2*a4 */
    /* C7-5 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ADD_DSZ64_DRR(RDX, TMP4, TMP4),    /* 2*a4 */
                 NOP_SEQWORD },
    /* C7-6: hi3 + MAC4 = a3 × 2a4 */
    /* C7-6 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP3, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C7-7: ACC4 + prep 2*a8 (for ×2 diagonal) */
    /* C7-7 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP12),      /* 2*a8 */
                 NOP_SEQWORD },
    /* C7-8: hi4 + MAC5 = a8 × 2a8 */
    /* C7-8 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP8, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C7-9: lastACC — slot 0→2 RAW */
    /* C7-9 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 SHR_DSZ64_DRI(R9, TMP13, 58),
                 NOP_SEQWORD },
    /* C7-10: CE-0 + prep 2*a8 for c8 */
    /* C7-10 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                  ADD_DSZ64_DRR(R10, R10, TMP15),
                  ZEROEXT_DSZ64_DR(RDX, TMP12),      /* 2*a8 */
                  NOP_SEQWORD },
    /* C7-11: CE-1: output c7→R15 */
    /* C7-11 */ { ADD_DSZ64_DRR(R8, R8, R10),
                  ZEROEXT_DSZ64_DR(R15, TMP13),      /* c7 → R15 */
                  SHL_DSZ64_DRI(RAX, R8, 6),
                  NOP_SEQWORD },

    /* ═══ LIMB c8 = 2·a0·a8 + 2·a1·a7 + 2·a2·a6 + 2·a3·a5 + a4² ═══ */

    /* C8-0: LINK+MAC1: a0 × 2a8 */
    /* C8-0 */ { OR_DSZ64_DRR(TMP13, R9, RAX),
                 MUL_DSZ64_DRR(RCX, TMP0, RDX),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 NOP_SEQWORD },
    /* C8-1: ACC1 + prep 2*a7 */
    /* C8-1 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP11),      /* 2*a7 */
                 NOP_SEQWORD },
    /* C8-2: hi1 + MAC2 = a1 × 2a7, init R10 */
    /* C8-2 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP1, RDX),
                 ZEROEXT_DSZ64_DR(R10, TMP15),
                 NOP_SEQWORD },
    /* C8-3: ACC2 + prep 2*a6 */
    /* C8-3 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),      /* 2*a6 */
                 NOP_SEQWORD },
    /* C8-4: hi2 + MAC3 = a2 × 2a6 */
    /* C8-4 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP2, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C8-5: ACC3 + prep 2*a5 */
    /* C8-5 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP9),       /* 2*a5 */
                 NOP_SEQWORD },
    /* C8-6: hi3 + MAC4 = a3 × 2a5 */
    /* C8-6 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP3, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C8-7: ACC4 + prep a4 (×1 diagonal) */
    /* C8-7 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 ZEROEXT_DSZ64_DR(RDX, TMP4),       /* a4 */
                 NOP_SEQWORD },
    /* C8-8: hi4 + MAC5 = a4 × a4 */
    /* C8-8 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                 MUL_DSZ64_DRR(RCX, TMP4, RDX),
                 ADD_DSZ64_DRR(R10, R10, TMP15),
                 NOP_SEQWORD },
    /* C8-9: lastACC — slot 0→2 RAW */
    /* C8-9 */ { ADD_DSZ64_DRR(TMP13, TMP13, RDX),
                 SETCC_CONDB_DR(TMP15, TMP13),
                 SHR_DSZ64_DRI(R9, TMP13, 58),
                 NOP_SEQWORD },
    /* C8-10: CE-0 (no next limb prep) */
    /* C8-10 */ { ADD_DSZ64_DRR(R8, R8, RCX),
                  ADD_DSZ64_DRR(R10, R10, TMP15),
                  NOP, NOP_SEQWORD },
    /* C8-11: CE-1: output c8→R13 (unmasked), hi carry bits */
    /* C8-11 */ { ADD_DSZ64_DRR(R8, R8, R10),
                  ZEROEXT_DSZ64_DR(R13, TMP13),      /* c8 → R13 */
                  SHL_DSZ64_DRI(RAX, R8, 6),
                  NOP_SEQWORD },
    /* C8-12: END: carry = lo_carry_bits | hi_carry_bits → RAX */
    /* C8-12 */ { OR_DSZ64_DRR(RAX, R9, RAX),
                  NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("p521_sq: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ── fe_sq native C ──────────────────────────────────────────── */

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    uint64_t a0=a[0], a1=a[1], a2=a[2], a3=a[3], a4=a[4];
    uint64_t a5=a[5], a6=a[6], a7=a[7], a8=a[8];

    __uint128_t c0 = (__uint128_t)a0*a0     + (__uint128_t)a1*(4*a8) + (__uint128_t)a2*(4*a7) + (__uint128_t)a3*(4*a6) + (__uint128_t)a4*(4*a5);
    __uint128_t c1 = (__uint128_t)a0*(2*a1)  + (__uint128_t)a2*(4*a8) + (__uint128_t)a3*(4*a7) + (__uint128_t)a4*(4*a6) + (__uint128_t)a5*(2*a5);
    __uint128_t c2 = (__uint128_t)a0*(2*a2)  + (__uint128_t)a1*a1     + (__uint128_t)a3*(4*a8) + (__uint128_t)a4*(4*a7) + (__uint128_t)a5*(4*a6);
    __uint128_t c3 = (__uint128_t)a0*(2*a3)  + (__uint128_t)a1*(2*a2) + (__uint128_t)a4*(4*a8) + (__uint128_t)a5*(4*a7) + (__uint128_t)a6*(2*a6);
    __uint128_t c4 = (__uint128_t)a0*(2*a4)  + (__uint128_t)a1*(2*a3) + (__uint128_t)a2*a2     + (__uint128_t)a5*(4*a8) + (__uint128_t)a6*(4*a7);
    __uint128_t c5 = (__uint128_t)a0*(2*a5)  + (__uint128_t)a1*(2*a4) + (__uint128_t)a2*(2*a3) + (__uint128_t)a6*(4*a8) + (__uint128_t)a7*(2*a7);
    __uint128_t c6 = (__uint128_t)a0*(2*a6)  + (__uint128_t)a1*(2*a5) + (__uint128_t)a2*(2*a4) + (__uint128_t)a3*a3     + (__uint128_t)a7*(4*a8);
    __uint128_t c7 = (__uint128_t)a0*(2*a7)  + (__uint128_t)a1*(2*a6) + (__uint128_t)a2*(2*a5) + (__uint128_t)a3*(2*a4) + (__uint128_t)a8*(2*a8);
    __uint128_t c8 = (__uint128_t)a0*(2*a8)  + (__uint128_t)a1*(2*a7) + (__uint128_t)a2*(2*a6) + (__uint128_t)a3*(2*a5) + (__uint128_t)a4*a4;

    uint64_t carry;
    carry = (uint64_t)(c0 >> 58); out[0] = (uint64_t)c0 & MASK58;
    c1 += carry;
    carry = (uint64_t)(c1 >> 58); out[1] = (uint64_t)c1 & MASK58;
    c2 += carry;
    carry = (uint64_t)(c2 >> 58); out[2] = (uint64_t)c2 & MASK58;
    c3 += carry;
    carry = (uint64_t)(c3 >> 58); out[3] = (uint64_t)c3 & MASK58;
    c4 += carry;
    carry = (uint64_t)(c4 >> 58); out[4] = (uint64_t)c4 & MASK58;
    c5 += carry;
    carry = (uint64_t)(c5 >> 58); out[5] = (uint64_t)c5 & MASK58;
    c6 += carry;
    carry = (uint64_t)(c6 >> 58); out[6] = (uint64_t)c6 & MASK58;
    c7 += carry;
    carry = (uint64_t)(c7 >> 58); out[7] = (uint64_t)c7 & MASK58;
    c8 += carry;
    carry = (uint64_t)(c8 >> 57); out[8] = (uint64_t)c8 & MASK57;
    out[0] += carry;
    carry = out[0] >> 58; out[0] &= MASK58;
    out[1] += carry;
    carry = out[1] >> 58; out[1] &= MASK58;
    out[2] += carry;
}

/* ── reference (naive schoolbook) ────────────────────────────── */

static void fe_sq_reference(const uint64_t *a, uint64_t *out) {
    __uint128_t t[17] = {0};
    for (int i = 0; i < 9; i++)
        for (int j = 0; j < 9; j++)
            t[i + j] += (__uint128_t)a[i] * a[j];
    for (int i = 9; i <= 16; i++)
        t[i - 9] += t[i] * 2;
    __uint128_t carry = 0;
    for (int i = 0; i < 9; i++) {
        t[i] += carry;
        int bits = (i == 8) ? 57 : 58;
        uint64_t mask = (i == 8) ? MASK57 : MASK58;
        carry = t[i] >> bits;
        out[i] = (uint64_t)t[i] & mask;
    }
    out[0] += (uint64_t)carry;
    carry = out[0] >> 58; out[0] &= MASK58;
    out[1] += (uint64_t)carry;
    carry = out[1] >> 58; out[1] &= MASK58;
    out[2] += (uint64_t)carry;
}

/* ── fe_sq via microcode ─────────────────────────────────────── */

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    /*
     * Single vmwrite: load a[0..8] → RDI,RSI,R12,R11,R14,RBX,RBP,R15,R13.
     * Patch computes all 9 limbs, outputs c[0..8] in the same registers,
     * carry in RAX.
     *
     * Output limbs are UNMASKED (contain carry bits in upper bits).
     * Native C masks to 58 bits (57 for limb 8) and does wrap-around.
     */
    register const uint64_t *_a   asm("rcx") = a;
    register uint64_t       *_out asm("r8")  = out;
    uint64_t carry;

    asm volatile(
        /* save callee-saved registers + output pointer */
        "push rbp\n\t"
        "push rbx\n\t"
        "push r12\n\t"
        "push r13\n\t"
        "push r14\n\t"
        "push r15\n\t"
        "push r8\n\t"

        /* load a[0..8] from rcx */
        "mov rdi, [rcx]\n\t"
        "mov rsi, [rcx + 8]\n\t"
        "mov r12, [rcx + 16]\n\t"
        "mov r11, [rcx + 24]\n\t"
        "mov r14, [rcx + 32]\n\t"
        "mov rbx, [rcx + 40]\n\t"
        "mov rbp, [rcx + 48]\n\t"
        "mov r15, [rcx + 56]\n\t"
        "mov r13, [rcx + 64]\n\t"

        /* fire microcode — single vmwrite computes all 9 limbs */
        "vmwrite rcx, rdx\n\t"

        /* recover output pointer, store results */
        "pop rcx\n\t"
        "mov [rcx],      rdi\n\t"      /* c0 (unmasked) */
        "mov [rcx + 8],  rsi\n\t"      /* c1 (unmasked) */
        "mov [rcx + 16], r12\n\t"      /* c2 (unmasked) */
        "mov [rcx + 24], r11\n\t"      /* c3 (unmasked) */
        "mov [rcx + 32], r14\n\t"      /* c4 (unmasked) */
        "mov [rcx + 40], rbx\n\t"      /* c5 (unmasked) */
        "mov [rcx + 48], rbp\n\t"      /* c6 (unmasked) */
        "mov [rcx + 56], r15\n\t"      /* c7 (unmasked) */
        "mov [rcx + 64], r13\n\t"      /* c8 (unmasked) */

        /* restore callee-saved */
        "pop r15\n\t"
        "pop r14\n\t"
        "pop r13\n\t"
        "pop r12\n\t"
        "pop rbx\n\t"
        "pop rbp\n\t"

        : "=a"(carry), "+r"(_a), "+r"(_out)
        :
        : "rdx", "rsi", "rdi",
          "r9", "r10", "r11",
          "memory", "cc"
    );

    /*
     * Patch outputs unmasked limbs (carry bits in upper bits of each limb).
     * The inter-limb carries are already propagated within the patch.
     * RAX (carry) = combined carry from c8: (lo>>58) | (hi<<6).
     * Mask all limbs to 58 bits, then handle c8's 57-bit top limb.
     */
    for (int i = 0; i < 8; i++)
        out[i] &= MASK58;

    /* c8 is 57 bits; the 58th bit (bit 57) is extra wrap-around */
    uint64_t extra = (out[8] >> 57) & 1;
    out[8] &= MASK57;

    /* Mersenne wrap: 2^521 ≡ 1, so carry wraps as carry*2 + extra */
    uint64_t wrap_carry = carry * 2 + extra;

    out[0] += wrap_carry;
    uint64_t c = out[0] >> 58; out[0] &= MASK58;
    out[1] += c;
    c = out[1] >> 58; out[1] &= MASK58;
    out[2] += c;
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
    uint64_t    input[9];
    uint64_t    expected[9];
    int         has_expected;
} test_vec_t;

static const test_vec_t test_vectors[] = {
    { "zero",
      {0,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0}, 1 },
    { "one",
      {1,0,0,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0,0}, 1 },
    { "nine",
      {9,0,0,0,0,0,0,0,0}, {81,0,0,0,0,0,0,0,0}, 1 },
    { "2^58",
      {0,1,0,0,0,0,0,0,0}, {0,0,1,0,0,0,0,0,0}, 1 },
    { "2^116",
      {0,0,1,0,0,0,0,0,0}, {0,0,0,0,1,0,0,0,0}, 1 },
    { "2^232",
      {0,0,0,0,1,0,0,0,0}, {0,0,0,0,0,0,0,0,1}, 1 },
    /* 2^464 squared = 2^928. 928=521+407. 2^521 ≡ 1 mod p → 2^928 = 2^407.
       407 = 7*58+1 → limb7 = 2^1 = 2. */
    { "2^464",
      {0,0,0,0,0,0,0,0,1}, {0,0,0,0,0,0,0,2,0}, 1 },
    /* 2^261 squared = 2^522 = 2 mod p */
    { "2^261",
      {0,0,0,0,UINT64_C(1)<<29,0,0,0,0}, {2,0,0,0,0,0,0,0,0}, 1 },
    { "all_ones",
      {1,1,1,1,1,1,1,1,1}, {0}, 0 },
    { "all_max58",
      {MASK58,MASK58,MASK58,MASK58,MASK58,MASK58,MASK58,MASK58,MASK57}, {0}, 0 },
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[9], nat[9], ucd[9];
    fe_sq_reference(t->input, ref);
    fe_sq_native(t->input, nat);
    fe_sq_ucode(t->input, ucd);

    int ok = 1;

    if (memcmp(ref, nat, 72) != 0) {
        printf("  FAIL [%s] native != reference\n", t->label);
        for (int i = 0; i < 9; i++)
            printf("    [%d] native=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, nat[i], ref[i], nat[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (memcmp(ref, ucd, 72) != 0) {
        printf("  FAIL [%s] ucode != reference\n", t->label);
        for (int i = 0; i < 9; i++)
            printf("    [%d] ucode=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, ucd[i], ref[i], ucd[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (t->has_expected && memcmp(ref, t->expected, 72) != 0) {
        printf("  FAIL [%s] result != expected\n", t->label);
        for (int i = 0; i < 9; i++)
            printf("    [%d] got=%016" PRIx64 " exp=%016" PRIx64 " %s\n",
                   i, ref[i], t->expected[i],
                   ref[i] == t->expected[i] ? "" : "***");
        ok = 0;
    }
    if (ok) printf("  PASS [%s]\n", t->label);
    return ok;
}

static int verify_random_quiet(const uint64_t a[9]) {
    uint64_t ref[9], nat[9], ucd[9];
    fe_sq_reference(a, ref);
    fe_sq_native(a, nat);
    fe_sq_ucode(a, ucd);
    if (memcmp(ref, nat, 72) != 0 || memcmp(ref, ucd, 72) != 0) {
        printf("  FAIL random: a={");
        for (int i = 0; i < 9; i++) printf("%s%016" PRIx64, i?",":"", a[i]);
        printf("}\n");
        if (memcmp(ref, nat, 72) != 0) {
            printf("    native mismatch:");
            for (int i = 0; i < 9; i++) printf(" %016" PRIx64, nat[i]);
            printf("\n");
        }
        if (memcmp(ref, ucd, 72) != 0) {
            printf("    ucode  mismatch:");
            for (int i = 0; i < 9; i++) printf(" %016" PRIx64, ucd[i]);
            printf("\n");
        }
        printf("    reference:      ");
        for (int i = 0; i < 9; i++) printf(" %016" PRIx64, ref[i]);
        printf("\n");
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
    uint64_t rng = 0xDEADBEEFCAFE1234ULL;
    int rpass = 0;
    for (int i = 0; i < RANDOM_TESTS; i++) {
        uint64_t a[9];
        for (int j = 0; j < 8; j++)
            a[j] = splitmix64(&rng) & MASK58;
        a[8] = splitmix64(&rng) & MASK57;
        if (verify_random_quiet(a)) rpass++;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    printf("\n--- Iterated chain (%d sq from (1,0,...,0)) ---\n", CHAIN_ITERS);
    uint64_t ri[9] = {1,0,0,0,0,0,0,0,0};
    uint64_t ni[9], ui[9];
    memcpy(ni, ri, 72); memcpy(ui, ri, 72);
    for (int i = 0; i < CHAIN_ITERS; i++) {
        fe_sq_reference(ri, ri);
        fe_sq_native(ni, ni);
        fe_sq_ucode(ui, ui);
    }
    int ref_nat = memcmp(ri, ni, 72) == 0;
    int ref_ucd = memcmp(ri, ui, 72) == 0;
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

    printf("=== P-521 field squaring: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_p521_sq_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    uint64_t state[9] = {
        0x00062D608F25D51AULL & MASK58, 0x000412A4B4F6592AULL & MASK58,
        0x00075B7171A4B31DULL & MASK58, 0x0001FF60527118FEULL & MASK58,
        0x000216936D3CD6E5ULL & MASK58, 0x0003B0A65E59EC35ULL & MASK58,
        0x00025EA3B4488A68ULL & MASK58, 0x0001DB1232A6754AULL & MASK58,
        0x0000E5B7C53A1B5EULL & MASK57,
    };

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t tmp[9];

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_native(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Naive -O3:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* fiat-crypto (the real GCC baseline CryptOpt compares against) */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_fiat(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Fiat-crypto: min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

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

    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
