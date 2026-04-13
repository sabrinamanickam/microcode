/*
 * ucode_fe_templates.h — Parameterized microcode building blocks for
 *                         Solinas / Dettman field-element squaring
 *
 * Every unsaturated-limb field square decomposes into a repeating kernel:
 *
 *   For each output limb k:
 *     1. Accumulate N products:  ACC_128 += a_i * b_j     (MAC chain)
 *     2. Extract carry:          carry  = ACC >> W
 *                                out[k] = ACC & MASK(W)   (carry extraction)
 *     3. Feed carry to next limb                          (transition)
 *   Final: out[0] += last_carry * R                       (reduction)
 *
 * These macros generate the microcode triads for each phase.
 * The three curve parameters are:
 *
 *   N — products per limb        {2,3,4,5}
 *   W — limb bit-width           {43,44,48,51,52,56,58}
 *   R — reduction constant       curve25519=19, poly1305=5, secp256k1=0x1000003d1, P-521=1
 *
 *   Derived: S = 64 - W  (shift amount for mask-via-shift trick)
 *
 * IMPORTANT: For verification, the naive polynomial schoolbook
 *   (t[i+j] += a[i]*a[j], reduce t[N..2N]) only works for UNIFORM
 *   radix (all limbs same width, e.g. curve25519).  For non-uniform
 *   widths (poly1305: 44/43/43), use big-integer square + mod p instead.
 *
 * ┌──────────────┬───────┬────┬──────┬─────┬─────────────────────────┐
 * │ Curve        │ Limbs │  N │  W   │  R  │ Est. triads             │
 * ├──────────────┼───────┼────┼──────┼─────┼─────────────────────────┤
 * │ curve25519   │   5   │  3 │  51  │  19 │ 57 (9+4×10 + 7 reduce) │
 * │ poly1305     │   3   │  2 │ 44/43│   5 │ ~32                     │
 * │ secp256k1    │   5   │ 2-4│ 52/48│ *   │ ~65                     │
 * │ P-521        │   9   │ 5-9│  58  │   1 │ ~130                    │
 * └──────────────┴───────┴────┴──────┴─────┴─────────────────────────┘
 *   * secp256k1 Dettman uses interleaved reduction with multiple constants
 *
 * Register convention (fixed across all curves):
 *   RCX        high product of every MUL (clobbered per multiply)
 *   R8         accumulator high word (zeroed between limbs)
 *   RAX        initial acc low (= 0), later overwritten
 *   TMP0/TMP2  alternating accumulator low word
 *   TMP3       carry flag from SETCC after each lo-add
 *   TMP4..TMP7 saved high products (one per MAC, up to 4)
 *   TMP8       lo carry bits  (acc >> W)
 *   TMP9       scratch for shift-mask
 *   TMP1       scratch for carry alignment in transitions
 *
 * MUL convention:
 *   MUL_DSZ64_DRR(hi_out, srcA, srcB_and_lo_out)
 *     srcA is read-only.  srcB is overwritten with the low 64-bit product.
 *
 *   MUL_DSZ64_DIR(hi_out, imm, src_and_lo_out)
 *     Multiply by immediate.  src is overwritten with the low product.
 */

#ifndef UCODE_FE_TEMPLATES_H
#define UCODE_FE_TEMPLATES_H

#include "../../include/ucode_macro.h"

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  1.  MAC CHAIN — Multiply-Accumulate building blocks
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

/*
 * MAC_HEAD(sA, sB) — Start MAC chain for the FIRST limb.
 *
 * Saves RAX (= 0) into TMP0, fires the first MUL.
 * After: TMP2 = 0 + lo(sA*sB), TMP3 = carry, RCX = hi(sA*sB).
 * sB is overwritten with the low product.
 * 2 triads.
 */
#define MAC_HEAD(sA, sB)                                              \
    { ZEROEXT_DSZ64_DR(TMP0, RAX),                                    \
      MUL_DSZ64_DRR(RCX, sA, sB),                                    \
      NOP, NOP_SEQWORD },                                             \
    { ADD_DSZ64_DRR(TMP2, TMP0, sB),                                  \
      SETCC_CONDB_DR(TMP3, TMP2),                                     \
      NOP, NOP_SEQWORD }

/*
 * MAC_RESUME(lo_reg) — Start MAC chain for a CHAIN limb (not the first).
 *
 * Assumes TMP0 = carry from previous limb (set by LIMB_LINK).
 * Assumes the first MUL was already started in the LIMB_LINK triad,
 * so lo_reg holds the low product.
 * After: TMP2 = carry + lo, TMP3 = carry_flag.
 * 1 triad.
 */
#define MAC_RESUME(lo_reg)                                            \
    { ADD_DSZ64_DRR(TMP2, TMP0, lo_reg),                              \
      SETCC_CONDB_DR(TMP3, TMP2),                                     \
      NOP, NOP_SEQWORD }

/*
 * MAC_NEXT(hi_save, sA, sB, acc_in, acc_out) — Chain another product.
 *
 * Saves the previous product's high part into hi_save,
 * starts the next MUL, and adds the low product to the accumulator.
 *
 * hi_save : TMP4 for product 0, TMP5 for 1, TMP6 for 2, TMP7 for 3
 * acc_in  : current accumulator (TMP2 after HEAD/RESUME, then alternates)
 * acc_out : next accumulator    (TMP0 if acc_in=TMP2, TMP2 if acc_in=TMP0)
 * sB is overwritten with the low product.
 *
 * Accumulator alternation pattern:
 *   After MAC_HEAD / MAC_RESUME : acc = TMP2
 *   After 1st MAC_NEXT          : acc = TMP0
 *   After 2nd MAC_NEXT          : acc = TMP2
 *   After 3rd MAC_NEXT          : acc = TMP0
 *   After 4th MAC_NEXT          : acc = TMP2
 *
 * 2 triads.
 */
#define MAC_NEXT(hi_save, sA, sB, acc_in, acc_out)                    \
    { ADD_DSZ64_DRR(hi_save, RCX, TMP3),                              \
      MUL_DSZ64_DRR(RCX, sA, sB),                                    \
      NOP, NOP_SEQWORD },                                             \
    { ADD_DSZ64_DRR(acc_out, acc_in, sB),                              \
      SETCC_CONDB_DR(TMP3, acc_out),                                   \
      NOP, NOP_SEQWORD }

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  2.  CARRY EXTRACTION — End MAC chain, extract carry @ bit W
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *
 *  After the last MAC_NEXT, we have:
 *    acc           accumulator low  (TMP0 or TMP2)
 *    RCX           high product of the last MUL
 *    TMP3          carry from the last lo-add
 *    TMP4..TMP(3+N) saved high products from earlier MACs
 *    R8            running hi accumulator (0 for the first limb)
 *
 *  These macros:
 *    - Save the last hi part (RCX + TMP3)
 *    - Extract TMP8 = acc >> W  (lo-carry)
 *    - Output limb = acc & MASK(W)  via (acc << S) >> S
 *    - Sum all hi parts into R8
 *
 *  W = limb bit-width,  S = 64 - W
 *
 *  Final acc register after N products (same for HEAD and RESUME start):
 *    N=2 → TMP0    N=3 → TMP2    N=4 → TMP0    N=5 → TMP2
 */

/* 2 products: hi parts in {TMP4, last→TMP5}.  3 triads. */
#define MAC_TAIL_2(acc, out, W, S)                                    \
    { SHR_DSZ64_DRI(TMP8, acc, W),                                    \
      ADD_DSZ64_DRR(TMP5, RCX, TMP3),                                 \
      NOP, NOP_SEQWORD },                                             \
    { SHL_DSZ64_DRI(TMP9, acc, S),                                     \
      ADD_DSZ64_DRR(TMP0, TMP4, TMP5),                                \
      NOP, NOP_SEQWORD },                                             \
    { SHR_DSZ64_DRI(out, TMP9, S),                                     \
      ADD_DSZ64_DRR(R8, R8, TMP0),                                    \
      NOP, NOP_SEQWORD }

/* 3 products: hi parts in {TMP4, TMP5, last→TMP6}.  3 triads. */
#define MAC_TAIL_3(acc, out, W, S)                                    \
    { SHR_DSZ64_DRI(TMP8, acc, W),                                    \
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),                                 \
      NOP, NOP_SEQWORD },                                             \
    { SHL_DSZ64_DRI(TMP9, acc, S),                                     \
      ADD_DSZ64_DRR(R8, R8, TMP4),                                    \
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),                                \
      NOP_SEQWORD },                                                   \
    { SHR_DSZ64_DRI(out, TMP9, S),                                     \
      ADD_DSZ64_DRR(R8, R8, TMP0),                                    \
      NOP, NOP_SEQWORD }

/* 4 products: hi parts in {TMP4, TMP5, TMP6, last→TMP7}.  4 triads. */
#define MAC_TAIL_4(acc, out, W, S)                                    \
    { SHR_DSZ64_DRI(TMP8, acc, W),                                    \
      ADD_DSZ64_DRR(TMP7, RCX, TMP3),                                 \
      NOP, NOP_SEQWORD },                                             \
    { SHL_DSZ64_DRI(TMP9, acc, S),                                     \
      ADD_DSZ64_DRR(TMP0, TMP4, TMP5),                                \
      ADD_DSZ64_DRR(TMP1, TMP6, TMP7),                                \
      NOP_SEQWORD },                                                   \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP1),                                \
      NOP, NOP, NOP_SEQWORD },                                        \
    { SHR_DSZ64_DRI(out, TMP9, S),                                     \
      ADD_DSZ64_DRR(R8, R8, TMP0),                                    \
      NOP, NOP_SEQWORD }

/* 5 products: hi parts in {TMP4..TMP7, last→via RCX+TMP3}.  4 triads. */
#define MAC_TAIL_5(acc, out, W, S)                                    \
    { SHR_DSZ64_DRI(TMP8, acc, W),                                    \
      ADD_DSZ64_DRR(TMP0, RCX, TMP3),                                 \
      NOP, NOP_SEQWORD },                                             \
    { SHL_DSZ64_DRI(TMP9, acc, S),                                     \
      ADD_DSZ64_DRR(TMP1, TMP4, TMP5),                                \
      ADD_DSZ64_DRR(TMP0, TMP6, TMP0),                                \
      NOP_SEQWORD },                                                   \
    { ADD_DSZ64_DRR(R8, R8, TMP7),                                    \
      ADD_DSZ64_DRR(TMP0, TMP1, TMP0),                                \
      NOP, NOP_SEQWORD },                                             \
    { SHR_DSZ64_DRI(out, TMP9, S),                                     \
      ADD_DSZ64_DRR(R8, R8, TMP0),                                    \
      NOP, NOP_SEQWORD }

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  3.  LIMB_LINK — Transition between limbs
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *
 *  Combines the lo carry (TMP8) and hi carry (R8) from the limb that
 *  just finished, clears R8 for the next limb, and starts the first
 *  MUL of the next limb — all in 2 triads.
 *
 *  S     = 64 - W  of the limb that just finished
 *  sA,sB = operands for the first product of the NEXT limb
 *  prep1,prep2 = free slots for curve-specific register reload/prep
 *                (use NOP if not needed)
 *
 *  After: TMP0 = combined carry,  first MUL running (lo will land in sB).
 *  Follow with MAC_RESUME(sB) to continue the chain.
 *  2 triads.
 */
#define LIMB_LINK(S, sA, sB, prep1, prep2)                           \
    { SHL_DSZ64_DRI(TMP1, R8, S),                                     \
      NOTAND_DSZ64_DRR(R8, R8, R8),                                   \
      prep1,                                                           \
      NOP_SEQWORD },                                                   \
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),                                 \
      MUL_DSZ64_DRR(RCX, sA, sB),                                     \
      prep2,                                                           \
      NOP_SEQWORD }

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  4.  FINAL REDUCTION — Wrap last carry back into limb 0
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *
 *  After the last limb's MAC_TAIL, TMP8 and R8 hold the final carry.
 *  We combine them, multiply by R, add to out[0], and re-propagate.
 *
 *  S_last = 64 - W  of the last limb
 */

/*
 * REDUCE_COMBINE(S) — Combine lo+hi carry into TMP0.  2 triads.
 */
#define REDUCE_COMBINE(S)                                             \
    { SHL_DSZ64_DRI(TMP1, R8, S),                                     \
      NOP, NOP, NOP_SEQWORD },                                        \
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),                                 \
      NOP, NOP, NOP_SEQWORD }

/*
 * REDUCE_MUL(R) — Multiply carry by reduction constant.
 * After: TMP0 = lo(carry * R).  1 triad.
 * For R=1 (P-521 Mersenne), skip this and use TMP0 directly.
 */
#define REDUCE_MUL(R)                                                 \
    { MUL_DSZ64_DIR(TMP1, R, TMP0),                                   \
      NOP, NOP, NOP_SEQWORD }

/*
 * REDUCE_ADD(limb0) — Add reduced carry to limb 0.  1 triad.
 */
#define REDUCE_ADD(limb0)                                             \
    { ADD_DSZ64_DRR(limb0, limb0, TMP0),                               \
      NOP, NOP, NOP_SEQWORD }

/*
 * REPROP(limb, W, S, next_limb) — Re-propagate carry from limb to next.
 *
 * Extracts carry = limb >> W, masks limb, adds carry to next_limb.
 * 2 triads.
 */
#define REPROP(limb, W, S, next_limb)                                 \
    { SHR_DSZ64_DRI(TMP0, limb, W),                                   \
      NOP, NOP, NOP_SEQWORD },                                        \
    { SHL_DSZ64_DRI(TMP1, limb, S),                                    \
      ADD_DSZ64_DRR(next_limb, next_limb, TMP0),                      \
      NOP, NOP_SEQWORD }

/*
 * REPROP_MASK(limb, S) — Mask limb after REPROP (extract final value).
 * Use NOP_SEQWORD or END_SEQWORD for seq.
 * 1 triad.
 */
#define REPROP_MASK(limb, S, seq)                                     \
    { SHR_DSZ64_DRI(limb, TMP1, S),                                   \
      NOP, NOP, seq }

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  5.  COMPOSITION RECIPES
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *
 *  First limb with N=3 products, W=51 (curve25519):
 *  ─────────────────────────────────────────────────
 *    MAC_HEAD(sA0, sB0)                          // 2 triads
 *    MAC_NEXT(TMP4, sA1, sB1, TMP2, TMP0)       // 2 triads
 *    MAC_NEXT(TMP5, sA2, sB2, TMP0, TMP2)       // 2 triads
 *    MAC_TAIL_3(TMP2, out0, 51, 13)             // 3 triads  = 9 total
 *
 *  Transition + chain limb with N=3:
 *  ──────────────────────────────────
 *    LIMB_LINK(13, sA0, sB0, prep1, prep2)      // 2 triads
 *    MAC_RESUME(sB0)                             // 1 triad
 *    MAC_NEXT(TMP4, sA1, sB1, TMP2, TMP0)       // 2 triads
 *    MAC_NEXT(TMP5, sA2, sB2, TMP0, TMP2)       // 2 triads
 *    MAC_TAIL_3(TMP2, out, 51, 13)              // 3 triads  = 10 total
 *
 *  First limb with N=2 products, W=44 (poly1305 limb 0):
 *  ──────────────────────────────────────────────────────
 *    MAC_HEAD(sA0, sB0)                          // 2 triads
 *    MAC_NEXT(TMP4, sA1, sB1, TMP2, TMP0)       // 2 triads
 *    MAC_TAIL_2(TMP0, out0, 44, 20)             // 3 triads  = 7 total
 *
 *  Transition + chain limb with N=2, W=43 (poly1305 limb 1):
 *  ──────────────────────────────────────────────────────────
 *    LIMB_LINK(20, sA0, sB0, prep1, NOP)        // 2 triads
 *    MAC_RESUME(sB0)                             // 1 triad
 *    MAC_NEXT(TMP4, sA1, sB1, TMP2, TMP0)       // 2 triads
 *    MAC_TAIL_2(TMP0, out, 43, 21)              // 3 triads  = 8 total
 *
 *  Final reduction (Solinas, R fits in immediate):
 *  ───────────────────────────────────────────────
 *    REDUCE_COMBINE(S_last)                      // 2 triads
 *    REDUCE_MUL(R)                               // 1 triad (skip for R=1)
 *    REDUCE_ADD(limb0)                           // 1 triad
 *    REPROP(limb0, W0, S0, limb1)               // 2 triads
 *    REPROP_MASK(limb0, S0, NOP_SEQWORD)         // 1 triad
 *    [optional: more REPROP for poly1305 wrap]
 *    last triad uses END_SEQWORD
 */

#endif /* UCODE_FE_TEMPLATES_H */
