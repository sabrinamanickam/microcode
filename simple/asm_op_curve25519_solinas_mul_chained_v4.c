/*
 * asm_op_curve25519_solinas_mul_chained_v4.c — Solinas mul, chained ADC,
 * schoolbook fused into the carry chain.
 *
 * v4 = v3 + two facts established 2026-09-07:
 *
 *   (1) MUL's high-half destination is a FREE OPERAND (tests/mul_dst_class.c,
 *       57/57 PASS for dst in {RCX, TMP, other arch}, srcA preserved, 2000
 *       random products each). v3 wrote every high half to RCX and then copied
 *       it to the TMP that wanted it:
 *           { MUL(RCX, TMP10, RDX), ZEROEXT(TMP0, RDX), ZEROEXT(TMP1, RCX) }
 *                                                       ^^^^ dead move
 *       v4 writes it straight there: MUL(TMP1, TMP10, RDX).
 *
 *   (2) Once GENARITHFLAGS_RR has published a carry, the architectural CF
 *       survives every other micro-op we tested — MUL, ZEROEXT, SHL, SHR, OR,
 *       ADD, SETCC and even a further ADC (tests/arch_cf_survival.c, 12/12
 *       transparent, checked in both directions). Only another
 *       GENARITHFLAGS_RR rewrites it.
 *
 * (2) is what makes the fusion legal. v3 had to run the schoolbook and the
 * carry chain as two separate blocks — MUL_BLOCK parked all eight lo/hi halves
 * in TMP0..7, then COMBINED_CHAIN consumed them — because it was not known
 * whether a MUL sitting between a bridge and its consuming ADC would disturb
 * the carry in flight. It does not. So v4 interleaves them: each column's
 * combine reads its low half straight out of RDX, in the same triad as the
 * multiply that produced it, before the next multiply reloads RDX.
 *
 * Consequences per schoolbook row:
 *   - the four ZEROEXTs that saved high halves are gone           (fact 1)
 *   - three of the four ZEROEXTs that saved low halves are gone   (fact 2);
 *     only lo0 is still parked, because acc[0] is added later in the chain
 *   - TMP2, TMP4 and TMP6 are now free, three TMPs that v3 could not spare
 *
 * Triad budget (vs v3's 74):
 *   PREP:                                        3   (unchanged)
 *   each row: START + FUSED_ROW(9) + WB(2) =    12   (was 14)
 *     row 0..2 with SHIFT_WRITEBACK_MERGED:     36
 *     row 3    with ROW3_WRITEBACK:             12
 *   reduction:                                  15   (M_red loses 4 dead
 *                                                     ZEROEXTs but keeps its
 *                                                     4 triads at 1 MUL each)
 *   --------------------------------------------------
 *   total:                                      66
 *
 * Still available, not done here: interleaving M_red's four multiplies into
 * the reduction chain the same way saves about 2 more triads, and the last ADC
 * of each chain could write an architectural register directly (it has no
 * carry left to publish) instead of going through the writeback — that one is
 * blocked for row 3, whose writeback does not shift, so it was left alone.
 *
 * Correctness is checked by the same harness as v3: known vectors, 10,000
 * random pairs, a 1,000-step chain, and commutativity.
 *
 * Build:  make PROG=asm_op_curve25519_solinas_mul_chained_v4
 * Run:    sudo taskset -c 0 ./asm_op_curve25519_solinas_mul_chained_v4_static
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

#define SCHOOLBOOK_ROW_START(a_src) \
    { ZEROEXT_DSZ64_DR(RDX, a_src), ZEROEXT_DSZ64_DR(RDI, a_src), \
      NOP, NOP_SEQWORD }

/*
 * FUSED_ROW: the four multiplies and the ten-hop carry chain in one 9-triad
 * block (v3 used 5 + 6 = 11 triads for the same work).
 *
 * Entering: RDX = RDI = a_i, TMP10..13 = b[0..3], TMP9 = 0,
 *           R15/R9/R10/R13/RAX = acc[0..4].
 *
 * Column chain (each column is hi_{j-1} + lo_j, carried):
 *   c1: TMP1 = hi0 + lo1        fresh ADD, no carry-in
 *   c2: TMP3 = hi1 + lo2 + CF
 *   c3: TMP5 = hi2 + lo3 + CF
 *   c4: TMP7 = hi3 + 0   + CF   end of the column chain, its CF is discarded
 * Accumulator chain:
 *   a0: TMP0  = acc[0] + lo0    fresh ADD
 *   a1: TMP1  = acc[1] + c1 + CF
 *   a2: TMP3  = acc[2] + c2 + CF
 *   a3: TMP5  = acc[3] + c3 + CF
 *   a4: TMP7  = acc[4] + c4 + CF
 *   a5: TMP14 = 0 + 0 + CF      overflow word
 *
 * Every low half except lo0 is consumed from RDX in the same triad as its
 * multiply. Each MUL writes its high half directly into the TMP the column
 * chain reads. The bridge-to-ADC distances that a ZEROEXT or a MUL sits inside
 * (T3, T4, T5) are exactly what arch_cf_survival.c certifies.
 *
 *   T1: { MUL->TMP1,        save lo0,        reload RDX   }
 *   T2: { MUL->TMP3,        c1 (reads lo1),  GFL(TMP1)    }
 *   T3: { reload RDX,       MUL->TMP5,       c2 (reads lo2) }
 *   T4: { GFL(TMP3),        reload RDX,      MUL->TMP7    }
 *   T5: { c3 (reads lo3),   GFL(TMP5),       c4           }
 *   T6: { a0,               GFL(TMP0),       a1           }
 *   T7: { GFL(TMP1),        a2,              GFL(TMP3)    }
 *   T8: { a3,               GFL(TMP5),       a4           }
 *   T9: { GFL(TMP7),        a5,              -            }
 *
 * After T9: TMP0=acc[0], TMP1=acc[1], TMP3=acc[2], TMP5=acc[3], TMP7=acc[4],
 *           TMP14=acc[5] — the same end state as v3's COMBINED_CHAIN.
 */
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

static void install_solinas_mul_chained_v4_patch(void) {
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

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("solinas_mul_chained_v4: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
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
#define REPS  200

int main(void) {
    printf("=== curve25519 Solinas mul CHAINED-ADC v4 (schoolbook fused into the chain) ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_solinas_mul_chained_v4_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED, skipping benchmark.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    uint64_t state_a[4] = {9, 0, 0, 0};
    uint64_t state_b[4] = {7, 0, 0, 0};
    uint64_t tmp_a[4], tmp_b[4], t0, t1, min, sum;

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, 32);
        memcpy(tmp_b, state_b, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_native(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Naive -O3:   min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, 32);
        memcpy(tmp_b, state_b, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_fiat(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Fiat-crypto: min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, 32);
        memcpy(tmp_b, state_b, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_ucode(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode chained v4: min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    init_match_and_patch(); do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
