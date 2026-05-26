/*
 * asm_op_curve25519_solinas_mul_chained_v3.c — Solinas mul, chained ADC, packed harder.
 *
 * v3 = v2 + intra-triad chain packing:
 *   Each chain step `ADC ; GFL` was 1 triad with NOP slot 2 in v2.
 *   With intra-triad CF-bridge confirmed (project_intra_triad_adc_gfl.md,
 *   2026-05-22), we pack chains as
 *     T_k:   { ADC_i,        GFL_i,        ADC_{i+1} }
 *     T_k+1: { GFL_{i+1},    ADC_{i+2},    GFL_{i+2} }
 *     T_k+2: { ADC_{i+3},    GFL_{i+3},    ADC_{i+4} }
 *     ...
 *   shrinking long chains by ~33%.
 *
 * Triad budget (vs v2's 95):
 *   PREP:                              3   (unchanged)
 *   each row: START + MUL5 + COMBINED_CHAIN(6) + SHIFT/WB(2) = 14 each
 *     row 0..2 with SHIFT_WRITEBACK_MERGED: 14*3 = 42
 *     row 3    with ROW3_WRITEBACK:           14
 *   reduction (chain-packed):         15
 *   --------------------------------------------------
 *   total:                            74
 *
 * Build:  make PROG=asm_op_curve25519_solinas_mul_chained_v3
 * Run:    sudo taskset -c 0 ./asm_op_curve25519_solinas_mul_chained_v3_static
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

/* MUL block: 4 MULs in 5 triads (same as v2). */
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

/*
 * COMBINED_CHAIN: col combine + acc add merged into one 6-triad chain.
 *
 * Steps (10 chain ops, 9 GFLs total):
 *   c1: TMP1 = TMP1 + TMP2          (hi0 + lo1 = col_1, no carry-in)
 *   c2: TMP3 = TMP3 + TMP4 + CF     (hi1 + lo2 + carry = col_2)
 *   c3: TMP5 = TMP5 + TMP6 + CF     (hi2 + lo3 + carry = col_3)
 *   c4: TMP7 = TMP7 + TMP9 + CF     (hi3 + 0 + carry = col_4) — col_4 doesn't propagate
 *   ──── transition: acc[0] uses fresh ADD ────
 *   a0: TMP0 = R15 + TMP0           (R15 + lo0)
 *   a1: TMP1 = R9  + TMP1 + CF      (R9 + col_1 + carry)
 *   a2: TMP3 = R10 + TMP3 + CF
 *   a3: TMP5 = R13 + TMP5 + CF
 *   a4: TMP7 = RAX + TMP7 + CF
 *   a5: TMP14 = TMP9 + TMP9 + CF    (overflow word)
 *
 * Packed pattern (3-slot triads):
 *   T1: { c1_ADD,  GFL(TMP1), c2_ADC }
 *   T2: { GFL(TMP3), c3_ADC,  GFL(TMP5) }
 *   T3: { c4_ADC,  a0_ADD,   GFL(TMP0) }      [col_4 ADC + transition to acc chain]
 *   T4: { a1_ADC,  GFL(TMP1), a2_ADC }
 *   T5: { GFL(TMP3), a3_ADC,  GFL(TMP5) }
 *   T6: { a4_ADC,  GFL(TMP7), a5_ADC }
 *
 * After T6:
 *   TMP0 = acc[0]_new, TMP1 = acc[1]_new, TMP3 = acc[2]_new,
 *   TMP5 = acc[3]_new, TMP7 = acc[4]_new, TMP14 = acc[5]_overflow.
 *
 * Note: T3 slot 0 col_4 ADC's CF would propagate, but slot 1 ADD overwrites
 * arch CF (col_4's CF is discarded — same as v1/v2 behavior; col_4 is the
 * end of the col chain). Slot 1 ADD sets fresh TMP-CF on TMP0; slot 2 GFL
 * bridges. Slot 2 GFL is then read by T4 slot 0 ADC.
 */
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

static void install_solinas_mul_chained_v3_patch(void) {
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

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("solinas_mul_chained_v3: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
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
    printf("=== curve25519 Solinas mul CHAINED-ADC v3 (intra-triad chain packing) ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_solinas_mul_chained_v3_patch();

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
    printf("Microcode chained v3: min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    init_match_and_patch(); do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
