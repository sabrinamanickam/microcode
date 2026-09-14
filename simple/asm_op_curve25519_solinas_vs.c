/*
 * asm_op_curve25519_solinas_vs.c — v3 vs v4 saturated 4x64 fe_mul, back to
 * back in ONE process.
 *
 * The two patches are 75 and 67 triads, so they cannot both live in the
 * 128-triad patch RAM: this binary installs one, benches it, re-installs the
 * other, benches that, and alternates the phases so that any slow drift in
 * clock or thermal state lands on both arms equally. The reported figure for
 * each arm is the min over all of its phases.
 *
 * Why not just compare two separate runs: rdtsc ticks at the fixed TSC rate
 * (1.10 GHz on the N3350) while the core bursts to 2.4 GHz, so an unpinned run
 * reports roughly 0.46x of the true cycle count. The v4 standalone run read
 * 87 cyc/op against v3's published 200, but in that same process the naive -O3
 * and fiat-crypto baselines read 60 and 104 where their pinned values are 132
 * and 224 — everything was scaled by ~2.17. frequency_guard() below prints the
 * measured TSC/core ratio and refuses to produce absolute numbers unpinned
 * (ALLOW_UNPINNED=1 downgrades that to ratios only).
 *
 * Both arms are verified against __uint128_t before either is timed, and the
 * two are checked to agree with each other on the same random inputs.
 *
 * Build:  make PROG=asm_op_curve25519_solinas_vs
 * Run:    sudo taskset -c 0 ./asm_op_curve25519_solinas_vs_static
 *   pinned:  echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

static const uint64_t CURVE25519_P[4] = {
    UINT64_C(0xFFFFFFFFFFFFFFED), UINT64_C(0xFFFFFFFFFFFFFFFF),
    UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x7FFFFFFFFFFFFFFF)
};

#include "../curvesC/curve25519_solinas_mul.c"

static void fe_mul_fiat(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    fiat_curve25519_solinas_mul(out, a, b);
}

static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    __uint128_t t;
    uint64_t prod[8] = {0};
    for (int i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 4; j++) {
            t = (__uint128_t)a[i] * b[j] + prod[i+j] + carry;
            prod[i+j] = (uint64_t)t;
            carry = (uint64_t)(t >> 64);
        }
        prod[i+4] += carry;
    }
    uint64_t r[4]; uint64_t c = 0;
    for (int i = 0; i < 4; i++) {
        t = (__uint128_t)38 * prod[i+4] + prod[i] + c;
        r[i] = (uint64_t)t;
        c = (uint64_t)(t >> 64);
    }
    t = (__uint128_t)38 * c + r[0];
    r[0] = (uint64_t)t;
    c = (uint64_t)(t >> 64);
    r[1] += c; c = (r[1] < c);
    r[2] += c; c = (r[2] < c);
    r[3] += c;
    uint64_t diff[4]; __uint128_t b128;
    b128 = (__uint128_t)r[0] - CURVE25519_P[0];                                  diff[0] = (uint64_t)b128;
    b128 = (__uint128_t)r[1] - CURVE25519_P[1] - ((uint64_t)(b128 >> 64) & 1);   diff[1] = (uint64_t)b128;
    b128 = (__uint128_t)r[2] - CURVE25519_P[2] - ((uint64_t)(b128 >> 64) & 1);   diff[2] = (uint64_t)b128;
    b128 = (__uint128_t)r[3] - CURVE25519_P[3] - ((uint64_t)(b128 >> 64) & 1);   diff[3] = (uint64_t)b128;
    uint64_t borrow = (uint64_t)(b128 >> 64) & 1;
    uint64_t mask = (uint64_t)0 - borrow;
    out[0] = (r[0] & mask) | (diff[0] & ~mask);
    out[1] = (r[1] & mask) | (diff[1] & ~mask);
    out[2] = (r[2] & mask) | (diff[2] & ~mask);
    out[3] = (r[3] & mask) | (diff[3] & ~mask);
}

#include "curve25519/include/freq_guard.h"

/* ---- shared row scaffolding (identical in both arms) ---- */
#define SCHOOLBOOK_ROW_START(a_src) \
    { ZEROEXT_DSZ64_DR(RDX, a_src), ZEROEXT_DSZ64_DR(RDI, a_src), \
      NOP, NOP_SEQWORD }

#define SHIFT_WRITEBACK_MERGED(save_reg) \
    { ZEROEXT_DSZ64_DR(save_reg, TMP0), ZEROEXT_DSZ64_DR(R15, TMP1), \
      ZEROEXT_DSZ64_DR(R9, TMP3), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(R10, TMP5), ZEROEXT_DSZ64_DR(R13, TMP7), \
      ZEROEXT_DSZ64_DR(RAX, TMP14), NOP_SEQWORD }

#define ROW3_WRITEBACK \
    { ZEROEXT_DSZ64_DR(R15, TMP0), ZEROEXT_DSZ64_DR(R9, TMP1), \
      ZEROEXT_DSZ64_DR(R10, TMP3), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(R13, TMP5), ZEROEXT_DSZ64_DR(RAX, TMP7), \
      NOP, NOP_SEQWORD }

/* ---- v3: schoolbook and carry chain as two blocks ---- */
#define MUL_BLOCK \
    { MUL_DSZ64_DRR(RCX, TMP10, RDX), \
      ZEROEXT_DSZ64_DR(TMP0, RDX), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(RDX, RDI), \
      MUL_DSZ64_DRR(RCX, TMP11, RDX), \
      ZEROEXT_DSZ64_DR(TMP2, RDX), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP3, RCX), \
      ZEROEXT_DSZ64_DR(RDX, RDI), \
      MUL_DSZ64_DRR(RCX, TMP12, RDX), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP4, RDX), \
      ZEROEXT_DSZ64_DR(TMP5, RCX), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP13, RDX), \
      ZEROEXT_DSZ64_DR(TMP6, RDX), \
      ZEROEXT_DSZ64_DR(TMP7, RCX), NOP_SEQWORD }

#define COMBINED_CHAIN \
    { ADD_DSZ64_DRR(TMP1, TMP1, TMP2), GENARITHFLAGS_RR(TMP1, TMP1), \
      ADC_DSZ64_DRR(TMP3, TMP3, TMP4), NOP_SEQWORD }, \
    { GENARITHFLAGS_RR(TMP3, TMP3), ADC_DSZ64_DRR(TMP5, TMP5, TMP6), \
      GENARITHFLAGS_RR(TMP5, TMP5), NOP_SEQWORD }, \
    { ADC_DSZ64_DRR(TMP7, TMP7, TMP9), ADD_DSZ64_DRR(TMP0, R15, TMP0), \
      GENARITHFLAGS_RR(TMP0, TMP0), NOP_SEQWORD }, \
    { ADC_DSZ64_DRR(TMP1, R9, TMP1), GENARITHFLAGS_RR(TMP1, TMP1), \
      ADC_DSZ64_DRR(TMP3, R10, TMP3), NOP_SEQWORD }, \
    { GENARITHFLAGS_RR(TMP3, TMP3), ADC_DSZ64_DRR(TMP5, R13, TMP5), \
      GENARITHFLAGS_RR(TMP5, TMP5), NOP_SEQWORD }, \
    { ADC_DSZ64_DRR(TMP7, RAX, TMP7), GENARITHFLAGS_RR(TMP7, TMP7), \
      ADC_DSZ64_DRR(TMP14, TMP9, TMP9), NOP_SEQWORD }

/* ---- v4: the two fused ---- */
#define FUSED_ROW \
    { MUL_DSZ64_DRR(TMP1, TMP10, RDX), ZEROEXT_DSZ64_DR(TMP0, RDX), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(TMP3, TMP11, RDX), ADD_DSZ64_DRR(TMP1, TMP1, RDX), \
      GENARITHFLAGS_RR(TMP1, TMP1), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(RDX, RDI), MUL_DSZ64_DRR(TMP5, TMP12, RDX), \
      ADC_DSZ64_DRR(TMP3, TMP3, RDX), NOP_SEQWORD }, \
    { GENARITHFLAGS_RR(TMP3, TMP3), ZEROEXT_DSZ64_DR(RDX, RDI), \
      MUL_DSZ64_DRR(TMP7, TMP13, RDX), NOP_SEQWORD }, \
    { ADC_DSZ64_DRR(TMP5, TMP5, RDX), GENARITHFLAGS_RR(TMP5, TMP5), \
      ADC_DSZ64_DRR(TMP7, TMP7, TMP9), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, R15, TMP0), GENARITHFLAGS_RR(TMP0, TMP0), \
      ADC_DSZ64_DRR(TMP1, R9, TMP1), NOP_SEQWORD }, \
    { GENARITHFLAGS_RR(TMP1, TMP1), ADC_DSZ64_DRR(TMP3, R10, TMP3), \
      GENARITHFLAGS_RR(TMP3, TMP3), NOP_SEQWORD }, \
    { ADC_DSZ64_DRR(TMP5, R13, TMP5), GENARITHFLAGS_RR(TMP5, TMP5), \
      ADC_DSZ64_DRR(TMP7, RAX, TMP7), NOP_SEQWORD }, \
    { GENARITHFLAGS_RR(TMP7, TMP7), ADC_DSZ64_DRR(TMP14, TMP9, TMP9), \
      NOP, NOP_SEQWORD }

static int install_v3(void) {
    ucode_t patch[] = {

    /* ─── PREP ─── */
    { ZEROEXT_DSZ64_DR(TMP10, RSI), ZEROEXT_DSZ64_DR(TMP11, R12),
      ZEROEXT_DSZ64_DR(TMP12, R11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R14), ZEROEXT_DSZ64_DR(R14, RDX),
      ZEROEXT_DSZ64_DR(TMP15, RBP), NOP_SEQWORD },
    { ZEROEXT_DSZ32_DI(TMP9, 0), ZEROEXT_DSZ32_DI(TMP14, 0),
      NOP, NOP_SEQWORD },

    /* ─── ROW 0 ─── */
    SCHOOLBOOK_ROW_START(RDI),
    MUL_BLOCK,
    COMBINED_CHAIN,
    SHIFT_WRITEBACK_MERGED(RSI),

    /* ─── ROW 1 ─── */
    SCHOOLBOOK_ROW_START(R14),
    MUL_BLOCK,
    COMBINED_CHAIN,
    SHIFT_WRITEBACK_MERGED(R12),

    /* ─── ROW 2 ─── */
    SCHOOLBOOK_ROW_START(TMP15),
    MUL_BLOCK,
    COMBINED_CHAIN,
    SHIFT_WRITEBACK_MERGED(R11),

    /* ─── ROW 3 ─── */
    SCHOOLBOOK_ROW_START(RBX),
    MUL_BLOCK,
    COMBINED_CHAIN,
    ROW3_WRITEBACK,

    /*
     * Post-schoolbook state:
     *   R15=p[3], R9=p[4], R10=p[5], R13=p[6], RAX=p[7], TMP14=p[8]
     *   RSI=p[0], R12=p[1], R11=p[2]
     */

    /* ─── REDUCTION ─── */
    /*
     * Reduction chains (col combine + r[0..3] add + final fold) packed
     * with the same intra-triad pattern. Structure:
     *
     *   M_red: 4 MULs by 38 (4 triads, unchanged from v2)
     *   COMBINED_RED_CHAIN: col combine (4 chain steps) + r[0..3] add
     *                       (5 chain steps with overflow capture) packed
     *                       at ~1.5 ops/triad ≈ 6 triads
     *   M_fold: MUL(38, top_carry) (1 triad)
     *   COMBINED_FOLD_CHAIN: 4-limb chain (3 propagating) ≈ 2 triads
     *   WB: writeback (2 triads)
     */
    /* M_red: multiply p[4..7] by 38, save his */
    { ADD_DSZ64_DRR(RAX, RAX, TMP14), MUL_DSZ64_DIR(RCX, 38, R9),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP0, RCX), MUL_DSZ64_DIR(RCX, 38, R10),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP1, RCX), MUL_DSZ64_DIR(RCX, 38, R13),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP2, RCX), MUL_DSZ64_DIR(RCX, 38, RAX),
      NOP, NOP_SEQWORD },

    /*
     * COMBINED_RED_CHAIN: col combine (TMP0..2, RCX) + r[0..3] add into
     * (TMP3..TMP6) with overflow into TMP7.
     *
     *   Col chain:
     *     TMP0 = TMP0 + R10           (hi4 + lo5 = col_1, no carry-in)
     *     TMP1 = TMP1 + R13 + CF      (hi5 + lo6 + carry = col_2)
     *     TMP2 = TMP2 + RAX + CF      (hi6 + lo7 + carry = col_3)
     *     RCX  = RCX  + TMP9 + CF     (hi7 + 0 + carry = col_4) — discard CF
     *   r chain (fresh, no carry-in):
     *     TMP3 = RSI + R9             (p[0] + lo4)
     *     TMP4 = R12 + TMP0 + CF
     *     TMP5 = R11 + TMP1 + CF
     *     TMP6 = R15 + TMP2 + CF
     *     TMP7 = RCX + TMP9 + CF      (top overflow)
     *
     *   Packed (9 chain ops + 8 GFLs in 6 triads):
     *     T1: { col_1_ADD,  GFL(TMP0), col_2_ADC }
     *     T2: { GFL(TMP1), col_3_ADC, GFL(TMP2) }
     *     T3: { col_4_ADC, r0_ADD,    GFL(TMP3) }   [col_4 CF discarded; r0 fresh ADD]
     *     T4: { r1_ADC,    GFL(TMP4), r2_ADC }
     *     T5: { GFL(TMP5), r3_ADC,    GFL(TMP6) }
     *     T6: { top_ADC,   NOP,       NOP }         [overflow capture]
     */
    { ADD_DSZ64_DRR(TMP0, TMP0, R10), GENARITHFLAGS_RR(TMP0, TMP0),
      ADC_DSZ64_DRR(TMP1, TMP1, R13), NOP_SEQWORD },
    { GENARITHFLAGS_RR(TMP1, TMP1), ADC_DSZ64_DRR(TMP2, TMP2, RAX),
      GENARITHFLAGS_RR(TMP2, TMP2), NOP_SEQWORD },
    { ADC_DSZ64_DRR(RCX, RCX, TMP9), ADD_DSZ64_DRR(TMP3, RSI, R9),
      GENARITHFLAGS_RR(TMP3, TMP3), NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP4, R12, TMP0), GENARITHFLAGS_RR(TMP4, TMP4),
      ADC_DSZ64_DRR(TMP5, R11, TMP1), NOP_SEQWORD },
    { GENARITHFLAGS_RR(TMP5, TMP5), ADC_DSZ64_DRR(TMP6, R15, TMP2),
      GENARITHFLAGS_RR(TMP6, TMP6), NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP7, RCX, TMP9), NOP, NOP, NOP_SEQWORD },

    /* Final fold: 38 * TMP7 → add to r[0], chain through r[1..3].
     * TMP7 is bounded (≤ 38 + 1 per analysis), so MUL hi = 0; only lo
     * counts. Pack MUL with first fold ADD's setup. */
    { MUL_DSZ64_DIR(RCX, 38, TMP7), NOP, NOP, NOP_SEQWORD },
    /* Fold chain (4 ops, 3 GFLs):
     *   TMP3 += TMP7 (=38*top_carry lo)
     *   TMP4 += 0 + CF
     *   TMP5 += 0 + CF
     *   TMP6 += 0 + CF
     * Packed:
     *   T1: { ADD(TMP3,TMP3,TMP7), GFL(TMP3), ADC(TMP4,TMP4,TMP9) }
     *   T2: { GFL(TMP4), ADC(TMP5,TMP5,TMP9), GFL(TMP5) }
     *   T3: { ADC(TMP6,TMP6,TMP9), NOP, NOP }
     */
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP7), GENARITHFLAGS_RR(TMP3, TMP3),
      ADC_DSZ64_DRR(TMP4, TMP4, TMP9), NOP_SEQWORD },
    { GENARITHFLAGS_RR(TMP4, TMP4), ADC_DSZ64_DRR(TMP5, TMP5, TMP9),
      GENARITHFLAGS_RR(TMP5, TMP5), NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP6, TMP6, TMP9), NOP, NOP, NOP_SEQWORD },

    /* Writeback r[0..3] → arch */
    { ZEROEXT_DSZ64_DR(R15, TMP3), ZEROEXT_DSZ64_DR(R9, TMP4),
      ZEROEXT_DSZ64_DR(R10, TMP5), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R13, TMP6), NOP, NOP, END_SEQWORD }

    };

    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    return (int)ARRAY_SZ(patch);
}

static int install_v4(void) {
    ucode_t patch[] = {

    /* ─── PREP ─── */
    { ZEROEXT_DSZ64_DR(TMP10, RSI), ZEROEXT_DSZ64_DR(TMP11, R12),
      ZEROEXT_DSZ64_DR(TMP12, R11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R14), ZEROEXT_DSZ64_DR(R14, RDX),
      ZEROEXT_DSZ64_DR(TMP15, RBP), NOP_SEQWORD },
    { ZEROEXT_DSZ32_DI(TMP9, 0), ZEROEXT_DSZ32_DI(TMP14, 0),
      NOP, NOP_SEQWORD },

    /* ─── ROW 0 ─── */
    SCHOOLBOOK_ROW_START(RDI),
    FUSED_ROW,
    SHIFT_WRITEBACK_MERGED(RSI),

    /* ─── ROW 1 ─── */
    SCHOOLBOOK_ROW_START(R14),
    FUSED_ROW,
    SHIFT_WRITEBACK_MERGED(R12),

    /* ─── ROW 2 ─── */
    SCHOOLBOOK_ROW_START(TMP15),
    FUSED_ROW,
    SHIFT_WRITEBACK_MERGED(R11),

    /* ─── ROW 3 ─── */
    SCHOOLBOOK_ROW_START(RBX),
    FUSED_ROW,
    ROW3_WRITEBACK,

    /*
     * Post-schoolbook state:
     *   R15=p[3], R9=p[4], R10=p[5], R13=p[6], RAX=p[7], TMP14=p[8]
     *   RSI=p[0], R12=p[1], R11=p[2]
     */

    /* ─── REDUCTION ─── */
    /*
     * Reduction chains (col combine + r[0..3] add + final fold) packed
     * with the same intra-triad pattern. Structure:
     *
     *   M_red: 4 MULs by 38 (4 triads, unchanged from v2)
     *   COMBINED_RED_CHAIN: col combine (4 chain steps) + r[0..3] add
     *                       (5 chain steps with overflow capture) packed
     *                       at ~1.5 ops/triad ≈ 6 triads
     *   M_fold: MUL(38, top_carry) (1 triad)
     *   COMBINED_FOLD_CHAIN: 4-limb chain (3 propagating) ≈ 2 triads
     *   WB: writeback (2 triads)
     */
    /* M_red: multiply p[4..7] by 38, save his */
    { ADD_DSZ64_DRR(RAX, RAX, TMP14), MUL_DSZ64_DIR(TMP0, 38, R9),
      NOP, NOP_SEQWORD },
    { MUL_DSZ64_DIR(TMP1, 38, R10), NOP, NOP, NOP_SEQWORD },
    { MUL_DSZ64_DIR(TMP2, 38, R13), NOP, NOP, NOP_SEQWORD },
    { MUL_DSZ64_DIR(RCX, 38, RAX), NOP, NOP, NOP_SEQWORD },

    /*
     * COMBINED_RED_CHAIN: col combine (TMP0..2, RCX) + r[0..3] add into
     * (TMP3..TMP6) with overflow into TMP7.
     *
     *   Col chain:
     *     TMP0 = TMP0 + R10           (hi4 + lo5 = col_1, no carry-in)
     *     TMP1 = TMP1 + R13 + CF      (hi5 + lo6 + carry = col_2)
     *     TMP2 = TMP2 + RAX + CF      (hi6 + lo7 + carry = col_3)
     *     RCX  = RCX  + TMP9 + CF     (hi7 + 0 + carry = col_4) — discard CF
     *   r chain (fresh, no carry-in):
     *     TMP3 = RSI + R9             (p[0] + lo4)
     *     TMP4 = R12 + TMP0 + CF
     *     TMP5 = R11 + TMP1 + CF
     *     TMP6 = R15 + TMP2 + CF
     *     TMP7 = RCX + TMP9 + CF      (top overflow)
     *
     *   Packed (9 chain ops + 8 GFLs in 6 triads):
     *     T1: { col_1_ADD,  GFL(TMP0), col_2_ADC }
     *     T2: { GFL(TMP1), col_3_ADC, GFL(TMP2) }
     *     T3: { col_4_ADC, r0_ADD,    GFL(TMP3) }   [col_4 CF discarded; r0 fresh ADD]
     *     T4: { r1_ADC,    GFL(TMP4), r2_ADC }
     *     T5: { GFL(TMP5), r3_ADC,    GFL(TMP6) }
     *     T6: { top_ADC,   NOP,       NOP }         [overflow capture]
     */
    { ADD_DSZ64_DRR(TMP0, TMP0, R10), GENARITHFLAGS_RR(TMP0, TMP0),
      ADC_DSZ64_DRR(TMP1, TMP1, R13), NOP_SEQWORD },
    { GENARITHFLAGS_RR(TMP1, TMP1), ADC_DSZ64_DRR(TMP2, TMP2, RAX),
      GENARITHFLAGS_RR(TMP2, TMP2), NOP_SEQWORD },
    { ADC_DSZ64_DRR(RCX, RCX, TMP9), ADD_DSZ64_DRR(TMP3, RSI, R9),
      GENARITHFLAGS_RR(TMP3, TMP3), NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP4, R12, TMP0), GENARITHFLAGS_RR(TMP4, TMP4),
      ADC_DSZ64_DRR(TMP5, R11, TMP1), NOP_SEQWORD },
    { GENARITHFLAGS_RR(TMP5, TMP5), ADC_DSZ64_DRR(TMP6, R15, TMP2),
      GENARITHFLAGS_RR(TMP6, TMP6), NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP7, RCX, TMP9), NOP, NOP, NOP_SEQWORD },

    /* Final fold: 38 * TMP7 → add to r[0], chain through r[1..3].
     * TMP7 is bounded (≤ 38 + 1 per analysis), so MUL hi = 0; only lo
     * counts. Pack MUL with first fold ADD's setup. */
    { MUL_DSZ64_DIR(RCX, 38, TMP7), NOP, NOP, NOP_SEQWORD },
    /* Fold chain (4 ops, 3 GFLs):
     *   TMP3 += TMP7 (=38*top_carry lo)
     *   TMP4 += 0 + CF
     *   TMP5 += 0 + CF
     *   TMP6 += 0 + CF
     * Packed:
     *   T1: { ADD(TMP3,TMP3,TMP7), GFL(TMP3), ADC(TMP4,TMP4,TMP9) }
     *   T2: { GFL(TMP4), ADC(TMP5,TMP5,TMP9), GFL(TMP5) }
     *   T3: { ADC(TMP6,TMP6,TMP9), NOP, NOP }
     */
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP7), GENARITHFLAGS_RR(TMP3, TMP3),
      ADC_DSZ64_DRR(TMP4, TMP4, TMP9), NOP_SEQWORD },
    { GENARITHFLAGS_RR(TMP4, TMP4), ADC_DSZ64_DRR(TMP5, TMP5, TMP9),
      GENARITHFLAGS_RR(TMP5, TMP5), NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP6, TMP6, TMP9), NOP, NOP, NOP_SEQWORD },

    /* Writeback r[0..3] → arch */
    { ZEROEXT_DSZ64_DR(R15, TMP3), ZEROEXT_DSZ64_DR(R9, TMP4),
      ZEROEXT_DSZ64_DR(R10, TMP5), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R13, TMP6), NOP, NOP, END_SEQWORD }

    };

    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    return (int)ARRAY_SZ(patch);
}


/* ---- the SETCC-dance form: asm_op_curve25519_solinas_mul.c,
   same register conventions, no ADC and no GENARITHFLAGS at all ---- */
#define SC_ROW_START(a_src) \
    { ZEROEXT_DSZ64_DR(RDX, a_src), ZEROEXT_DSZ64_DR(RDI, a_src), \
      NOP, NOP_SEQWORD }

#define SC_ROW_BODY \
    /* ── Phase A: schoolbook RDI × TMP10..TMP13 (13 triads) ── */ \
    { MUL_DSZ64_DRR(RCX, TMP10, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP0, RDX), ZEROEXT_DSZ64_DR(TMP1, RCX), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP11, RDX), ADD_DSZ64_DRR(TMP0, R15, TMP0), \
      SETCC_CONDB_DR(TMP3, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP2, TMP1, RDX), SETCC_CONDB_DR(TMP8, TMP2), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    /* writeback w0→R15 + early w1 start */ \
    { ZEROEXT_DSZ64_DR(RDX, RDI), ZEROEXT_DSZ64_DR(R15, TMP0), \
      ADD_DSZ64_DRR(TMP0, R9, TMP2), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP12, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* b3 MUL merge: save hi(b2), reload RDX, merge ADD into MUL */ \
    { ADD_DSZ64_DRR(TMP4, TMP1, RDX), SETCC_CONDB_DR(TMP5, TMP4), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP8), SETCC_CONDB_DR(TMP6, TMP4), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP13, RDX), ADD_DSZ64_DRR(TMP8, TMP5, TMP6), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP5, TMP1, RDX), SETCC_CONDB_DR(TMP6, TMP5), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP5, TMP5, TMP8), SETCC_CONDB_DR(TMP7, TMP5), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP8, TMP6, TMP7), ADD_DSZ64_DRR(TMP6, RCX, TMP8), \
      NOP, NOP_SEQWORD }, \
    /* ── Phase A': accumulate (8 triads) ── */ \
    /* w1 triple-pack (SETCC reads TMP0 flags from early start) */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R9, TMP0), \
      ADD_DSZ64_DRR(TMP0, R10, TMP4), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R10, TMP0), \
      ADD_DSZ64_DRR(TMP0, R13, TMP5), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R13, TMP0), \
      ADD_DSZ64_DRR(TMP0, RAX, TMP6), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP14, TMP1, TMP8), ZEROEXT_DSZ64_DR(RAX, TMP0), \
      NOP, NOP_SEQWORD }

#define SC_SHIFT_ROW(save_reg) \
    { ZEROEXT_DSZ64_DR(save_reg, R15), ZEROEXT_DSZ64_DR(R15, R9), \
      ZEROEXT_DSZ64_DR(R9, R10), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(R10, R13), ZEROEXT_DSZ64_DR(R13, RAX), \
      ZEROEXT_DSZ64_DR(RAX, TMP14), NOP_SEQWORD }

static int install_setcc(void) {
    ucode_t patch[] = {

    /* ═══ PREP: copy b values to TMP10-13, save a[1]→R14, a[2]→TMP15 ═══ */
    /*
     * At entry:
     *   RSI=b[0]  R12=b[1]  R11=b[2]  R14=b[3]  (b values in arch regs)
     *   RDI=a[0]  RDX=a[1]  RBP=a[2]  RBX=a[3]  (a values)
     *   R8=38  R15=0  R9=0  R10=0  R13=0  RAX=0
     *
     * After PREP:
     *   TMP10=b[0]  TMP11=b[1]  TMP12=b[2]  TMP13=b[3]
     *   R14=a[1] (overwritten from b[3])  TMP15=a[2]
     *   RBX=a[3] (unchanged)  RDI=a[0] (unchanged)
     */
    { ZEROEXT_DSZ64_DR(TMP10, RSI), ZEROEXT_DSZ64_DR(TMP11, R12),
      ZEROEXT_DSZ64_DR(TMP12, R11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R14), ZEROEXT_DSZ64_DR(R14, RDX),
      ZEROEXT_DSZ64_DR(TMP15, RBP), NOP_SEQWORD },

    /* ═══ ROW 0: a[0] × b[0..3] (23 triads) ═══ */
    SC_ROW_START(RDI),  /* RDI = a[0] from inline asm */
    SC_ROW_BODY,
    SC_SHIFT_ROW(RSI),             /* save product[0] → RSI */

    /* ═══ ROW 1: a[1] × b[0..3] (23 triads) ═══ */
    SC_ROW_START(R14),  /* a[1] saved in R14 by PREP */
    SC_ROW_BODY,
    SC_SHIFT_ROW(R12),             /* save product[1] → R12 */

    /* ═══ ROW 2: a[2] × b[0..3] (23 triads) ═══ */
    SC_ROW_START(TMP15),  /* a[2] saved in TMP15 by PREP */
    SC_ROW_BODY,
    SC_SHIFT_ROW(R11),             /* save product[2] → R11 */

    /* ═══ ROW 3: a[3] × b[0..3] (23 triads) ═══ */
    SC_ROW_START(RBX),  /* a[3] in RBX from inline asm */
    SC_ROW_BODY,
    /* No shift: R15=p[3], R9=p[4], R10=p[5], R13=p[6], RAX=p[7], TMP14=carry8
     * Saved: RSI=p[0], R12=p[1], R11=p[2]
     */

    /* ═══ SOLINAS REDUCTION (Fiat-style two-phase, 27 triads) ═══ */
    /*
     * Step 1: Multiply product[4..7] by 38 using MUL-by-immediate.
     * MUL_DSZ64_DIR(hi_out, imm, src_and_lo_out): src overwritten with lo.
     * Also fold TMP14 (carry8) into RAX before multiplying.
     *
     * After: R9=lo4, R10=lo5, R13=lo6, RAX=lo7
     *        TMP0=hi4, TMP1=hi5, TMP2=hi6, RCX=hi7
     */
    { ADD_DSZ64_DRR(RAX, RAX, TMP14), MUL_DSZ64_DIR(RCX, 38, R9),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP0, RCX), MUL_DSZ64_DIR(RCX, 38, R10),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP1, RCX), MUL_DSZ64_DIR(RCX, 38, R13),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP2, RCX), MUL_DSZ64_DIR(RCX, 38, RAX),
      NOP, NOP_SEQWORD },

    /*
     * Step 2, Phase 1: add lo(38*p[5..7]) to product[1..3]
     */
    /* w1: R12 += R10 (lo5), no carry in */
    { ADD_DSZ64_DRR(TMP4, R12, R10), SETCC_CONDB_DR(TMP3, TMP4),
      ZEROEXT_DSZ64_DR(R12, TMP4), NOP_SEQWORD },
    /* w2: R11 += R13 (lo6) + carry */
    { ADD_DSZ64_DRR(TMP4, R11, R13), SETCC_CONDB_DR(TMP5, TMP4),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP3), SETCC_CONDB_DR(TMP6, TMP4),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP5, TMP6), ZEROEXT_DSZ64_DR(R11, TMP4),
      ADD_DSZ64_DRR(TMP4, R15, RAX), NOP_SEQWORD },
    /* w3: R15 += RAX (lo7) + carry — triple-pack */
    { SETCC_CONDB_DR(TMP5, TMP4), ADD_DSZ64_DRR(TMP4, TMP4, TMP3),
      SETCC_CONDB_DR(TMP6, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP5, TMP6), ZEROEXT_DSZ64_DR(R15, TMP4),
      NOP, NOP_SEQWORD },
    /* overflow: carry from w3 + hi7(RCX) */
    { ADD_DSZ64_DRR(TMP7, TMP3, RCX), NOP, NOP, NOP_SEQWORD },

    /*
     * Step 2, Phase 2: add lo4(R9) + hi(38*p[4..6]) to result[0..3]
     */
    /* w0: RSI += R9 (lo4), no carry in */
    { ADD_DSZ64_DRR(TMP4, RSI, R9), SETCC_CONDB_DR(TMP3, TMP4),
      ZEROEXT_DSZ64_DR(RSI, TMP4), NOP_SEQWORD },
    /* w1: R12 += TMP0 (hi4) + carry */
    { ADD_DSZ64_DRR(TMP4, R12, TMP0), SETCC_CONDB_DR(TMP5, TMP4),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP3), SETCC_CONDB_DR(TMP6, TMP4),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP5, TMP6), ZEROEXT_DSZ64_DR(R12, TMP4),
      ADD_DSZ64_DRR(TMP4, R11, TMP1), NOP_SEQWORD },
    /* w2: R11 += TMP1 (hi5) + carry — triple-pack */
    { SETCC_CONDB_DR(TMP5, TMP4), ADD_DSZ64_DRR(TMP4, TMP4, TMP3),
      SETCC_CONDB_DR(TMP6, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP5, TMP6), ZEROEXT_DSZ64_DR(R11, TMP4),
      ADD_DSZ64_DRR(TMP4, R15, TMP2), NOP_SEQWORD },
    /* w3: R15 += TMP2 (hi6) + carry — triple-pack */
    { SETCC_CONDB_DR(TMP5, TMP4), ADD_DSZ64_DRR(TMP4, TMP4, TMP3),
      SETCC_CONDB_DR(TMP6, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP5, TMP6), ZEROEXT_DSZ64_DR(R15, TMP4),
      NOP, NOP_SEQWORD },

    /*
     * Step 3: Final fold — total_carry * 38 + result[0]
     */
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP7), NOP, NOP, NOP_SEQWORD },
    { MUL_DSZ64_DIR(RCX, 38, TMP3), NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RSI, TMP3), SETCC_CONDB_DR(TMP3, TMP4),
      ZEROEXT_DSZ64_DR(RSI, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, R12, TMP3), SETCC_CONDB_DR(TMP3, TMP4),
      ZEROEXT_DSZ64_DR(R12, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, R11, TMP3), SETCC_CONDB_DR(TMP3, TMP4),
      ZEROEXT_DSZ64_DR(R11, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R15, R15, TMP3), NOP, NOP, NOP_SEQWORD },

    /* Move to output convention: R15=r[0], R9=r[1], R10=r[2], R13=r[3] */
    { ZEROEXT_DSZ64_DR(R13, R15), ZEROEXT_DSZ64_DR(R15, RSI),
      ZEROEXT_DSZ64_DR(R9, R12), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R10, R11), NOP, NOP, END_SEQWORD }

    };

init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    return (int)ARRAY_SZ(patch);
}

static void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t r[4];
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_b   asm("rbx") = (uint64_t *)b;
    register uint64_t *_out asm("r15") = r;
    asm volatile(
        "push r15\n\t"
        "push rbp\n\t"
        "push rcx\n\t"
        "mov rsi, [rbx]\n\t"
        "mov r12, [rbx + 8]\n\t"
        "mov r11, [rbx + 16]\n\t"
        "mov r14, [rbx + 24]\n\t"
        "mov rdi, [rcx]\n\t"
        "mov rdx, [rcx + 8]\n\t"
        "mov rbp, [rcx + 16]\n\t"
        "mov rbx, [rcx + 24]\n\t"
        "mov r8, 38\n\t"
        "xor r15d, r15d\n\t"
        "xor r9d, r9d\n\t"
        "xor r10d, r10d\n\t"
        "xor r13d, r13d\n\t"
        "xor eax, eax\n\t"
        "vmwrite rcx, rdx\n\t"
        "pop rcx\n\t"
        "pop rbp\n\t"
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
    uint64_t diff[4]; __uint128_t b128;
    b128 = (__uint128_t)r[0] - CURVE25519_P[0];                                  diff[0] = (uint64_t)b128;
    b128 = (__uint128_t)r[1] - CURVE25519_P[1] - ((uint64_t)(b128 >> 64) & 1);   diff[1] = (uint64_t)b128;
    b128 = (__uint128_t)r[2] - CURVE25519_P[2] - ((uint64_t)(b128 >> 64) & 1);   diff[2] = (uint64_t)b128;
    b128 = (__uint128_t)r[3] - CURVE25519_P[3] - ((uint64_t)(b128 >> 64) & 1);   diff[3] = (uint64_t)b128;
    uint64_t borrow = (uint64_t)(b128 >> 64) & 1;
    uint64_t mask = (uint64_t)0 - borrow;
    out[0] = (r[0] & mask) | (diff[0] & ~mask);
    out[1] = (r[1] & mask) | (diff[1] & ~mask);
    out[2] = (r[2] & mask) | (diff[2] & ~mask);
    out[3] = (r[3] & mask) | (diff[3] & ~mask);
}

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void rand_mod_p(uint64_t out[4], uint64_t *rng) {
    for (;;) {
        for (int j = 0; j < 4; j++) out[j] = splitmix64(rng);
        out[3] &= 0x7FFFFFFFFFFFFFFFULL;
        int lt = 0;
        for (int j = 3; j >= 0; j--) {
            if (out[j] < CURVE25519_P[j]) { lt = 1; break; }
            if (out[j] > CURVE25519_P[j]) break;
        }
        if (lt) break;
    }
}

static int verify_all(void) {
    int pass = 0, fail = 0;
    printf("--- Known vectors ---\n");
    struct { const char *name; uint64_t a[4]; uint64_t b[4]; } vecs[] = {
        { "0*0",  {0, 0, 0, 0}, {0, 0, 0, 0} },
        { "1*1",  {1, 0, 0, 0}, {1, 0, 0, 0} },
        { "0*1",  {0, 0, 0, 0}, {1, 0, 0, 0} },
        { "2*3",  {2, 0, 0, 0}, {3, 0, 0, 0} },
        { "38*1", {38, 0, 0, 0}, {1, 0, 0, 0} },
        { "(p-1)*2",
          {UINT64_C(0xFFFFFFFFFFFFFFEC), UINT64_C(0xFFFFFFFFFFFFFFFF),
           UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x7FFFFFFFFFFFFFFF)},
          {2, 0, 0, 0} },
        { "big*big",
          {UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0xFFFFFFFFFFFFFFFF),
           UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x3FFFFFFFFFFFFFFF)},
          {UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0xFFFFFFFFFFFFFFFF),
           UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x3FFFFFFFFFFFFFFF)} },
    };
    int nvecs = sizeof(vecs) / sizeof(vecs[0]);
    for (int i = 0; i < nvecs; i++) {
        uint64_t nat[4], ucd[4];
        fe_mul_native(vecs[i].a, vecs[i].b, nat);
        fe_mul_ucode(vecs[i].a, vecs[i].b, ucd);
        int ok = !memcmp(nat, ucd, 32);
        if (!ok) {
            printf("  FAIL [%s]\n", vecs[i].name);
            for (int j = 0; j < 4; j++)
                printf("    [%d] nat=%016"PRIx64" ucd=%016"PRIx64"%s\n",
                    j, nat[j], ucd[j], nat[j] != ucd[j] ? " ***" : "");
        } else printf("  PASS [%s]\n", vecs[i].name);
        if (ok) pass++; else fail++;
    }

    printf("\n--- Random (10000) ---\n");
    uint64_t rng = 0xDEADBEEFCAFE1234ULL;
    int rp = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t a[4], b[4], nat[4], ucd[4];
        rand_mod_p(a, &rng); rand_mod_p(b, &rng);
        fe_mul_native(a, b, nat); fe_mul_ucode(a, b, ucd);
        if (!memcmp(nat, ucd, 32)) rp++;
        else {
            printf("  FAIL #%d\n", i);
            printf("    a={%016"PRIx64",%016"PRIx64",%016"PRIx64",%016"PRIx64"}\n",
                   a[0], a[1], a[2], a[3]);
            printf("    b={%016"PRIx64",%016"PRIx64",%016"PRIx64",%016"PRIx64"}\n",
                   b[0], b[1], b[2], b[3]);
            for (int j = 0; j < 4; j++)
                printf("    [%d] nat=%016"PRIx64" ucd=%016"PRIx64"%s\n",
                    j, nat[j], ucd[j], nat[j] != ucd[j] ? " ***" : "");
            break;
        }
    }
    printf("  %d / 10000 PASS\n", rp);
    pass += rp; if (rp < 10000) fail += (10000 - rp);

    printf("\n=== %d passed, %d failed ===\n\n", pass, fail);
    return fail;
}

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
#define REPS  60      /* per phase */
#define PHASES 6      /* setcc,v3,v4,setcc,v3,v4 — drift lands on all arms equally */

/* Time BATCH dependent fe_mul_ucode calls, REPS times, return the min. */
static uint64_t bench_ucode(void) {
    uint64_t sa[4] = {9,0,0,0}, sb[4] = {7,0,0,0}, ta[4], tb[4];
    uint64_t min = UINT64_MAX;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, 32); memcpy(tb, sb, 32);
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_ucode(ta, tb, ta);
        uint64_t dt = rdtsc_end() - t0;
        if (dt < min) min = dt;
    }
    return min / BATCH;
}

/* Same shape for a C reference, so the anchors are measured identically. */
static uint64_t bench_c(void (*f)(const uint64_t*, const uint64_t*, uint64_t*)) {
    uint64_t sa[4] = {9,0,0,0}, sb[4] = {7,0,0,0}, ta[4], tb[4];
    uint64_t min = UINT64_MAX;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, 32); memcpy(tb, sb, 32);
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) f(ta, tb, ta);
        uint64_t dt = rdtsc_end() - t0;
        if (dt < min) min = dt;
    }
    return min / BATCH;
}

/* Do the two arms agree on the same random inputs? */
static int cross_check(void) {
    uint64_t rng = 0x1234567887654321ULL;
    uint64_t a[4], b[4], r3[4], r4[4];
    int bad = 0;
    for (int i = 0; i < 500; i++) {
        rand_mod_p(a, &rng); rand_mod_p(b, &rng);
        install_v3(); fe_mul_ucode(a, b, r3);
        install_v4(); fe_mul_ucode(a, b, r4);
        if (memcmp(r3, r4, 32) != 0) bad++;
    }
    printf("--- cross-check v3 vs v4 on 500 random pairs: %s ---\n",
           bad ? "MISMATCH" : "identical");
    return bad;
}

int main(void) {
    printf("=== saturated 4x64 fe_mul: v3 vs v4, same process ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    if (frequency_guard()) return 2;

    int ns = install_setcc();
    printf("SETCC-dance form installed: %d triads\n", ns);
    if (verify_all()) { printf("SETCC form verification FAILED\n"); goto done; }

    int n3 = install_v3();
    printf("\nv3 installed: %d triads\n", n3);
    if (verify_all()) { printf("v3 verification FAILED\n"); goto done; }

    int n4 = install_v4();
    printf("\nv4 installed: %d triads\n", n4);
    if (verify_all()) { printf("v4 verification FAILED\n"); goto done; }

    if (cross_check()) goto done;

    /* C anchors: if these stray from their pinned references the run is
     * scaled and only the ratios between arms mean anything. */
    printf("\n--- C reference anchors (%d ops/batch, %d reps) ---\n", BATCH, REPS);
    printf("  naive -O3 4x64      %4" PRIu64 " cyc/op   (pinned reference: 132)\n",
           bench_c(fe_mul_native));
    printf("  fiat-crypto 4x64    %4" PRIu64 " cyc/op   (pinned reference: 224)\n",
           bench_c(fe_mul_fiat));

    uint64_t best[3] = { UINT64_MAX, UINT64_MAX, UINT64_MAX };
    const char *nm[3] = { "SETCC", "v3   ", "v4   " };
    int triads[3] = { ns, n3, n4 };
    printf("\n--- interleaved phases ---\n");
    for (int ph = 0; ph < PHASES; ph++) {
        int arm = ph % 3;
        if      (arm == 0) install_setcc();
        else if (arm == 1) install_v3();
        else               install_v4();
        uint64_t c = bench_ucode();
        printf("  phase %d  %s  %4" PRIu64 " cyc/op\n", ph, nm[arm], c);
        if (c < best[arm]) best[arm] = c;
    }

    printf("\n--- result (all three same process, same clock) ---\n");
    for (int a = 0; a < 3; a++)
        printf("  %s  %3d triads   %4" PRIu64 " cyc/op\n", nm[a], triads[a], best[a]);
    if (best[0] && best[2])
        printf("\n  ADC path (v4) vs SETCC path: %.1f%% %s, %d fewer triads\n",
               100.0 * ((double)best[0] - (double)best[2]) / (double)best[0],
               best[2] < best[0] ? "faster" : "slower", triads[0] - triads[2]);

done:
    init_match_and_patch(); do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
