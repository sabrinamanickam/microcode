/*
 * asm_op_curve25519_solinas_mul_chained.c — Solinas mul via chained ADC + GFL_RR.
 *
 * Field: GF(2^255 - 19), saturated 4 x 64-bit limbs.
 *
 * This is a redesign of asm_op_curve25519_solinas_mul.c that replaces the
 * triple-pack SETCC carry dance with the chained-ADC primitive discovered
 * 2026-05-22:
 *
 *   ADD/ADC TMP = a + b      (sets TMP-CF for the result)
 *   GENARITHFLAGS_RR(TMP, TMP)   (bridges TMP-CF → arch CF)
 *   ADC TMP_next = c + d         (reads arch CF, sets new TMP-CF)
 *   GENARITHFLAGS_RR(TMP_next, TMP_next)
 *   ... continues across as many limbs as needed.
 *
 * Algorithm: 4-row schoolbook with one CHAIN per row through 5 acc limbs.
 *
 * Layout same as original after PREP:
 *   a's: RDI=a[0], R14=a[1], TMP15=a[2], RBX=a[3]
 *   b's: TMP10=b[0], TMP11=b[1], TMP12=b[2], TMP13=b[3]
 *   acc:  R15=acc[0], R9=acc[1], R10=acc[2], R13=acc[3], RAX=acc[4], TMP14=acc[5]
 *   const: R8=38 (reduction)
 *   ZERO: TMP9 = 0 (new — pre-loaded for chained-ADC "+0" propagations)
 *
 * Build:  make PROG=asm_op_curve25519_solinas_mul_chained
 * Run:    sudo taskset -c 0 ./asm_op_curve25519_solinas_mul_chained_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* p = 2^255 - 19 */
static const uint64_t CURVE25519_P[4] = {
    UINT64_C(0xFFFFFFFFFFFFFFED), UINT64_C(0xFFFFFFFFFFFFFFFF),
    UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x7FFFFFFFFFFFFFFF)
};

#include "../curvesC/curve25519_solinas_mul.c"

static void fe_mul_fiat(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    fiat_curve25519_solinas_mul(out, a, b);
}

/* ── native reference ── */
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

/*
 * ── microcode patch (chained ADC) ──
 *
 * SCHOOLBOOK_ROW_CHAINED: One row of the 4×4 schoolbook, adds a × b[0..3]
 * to the 6-word accumulator (R15,R9,R10,R13,RAX,TMP14) using chained ADC.
 *
 * Strategy: do all 4 MULs first, saving lo/hi to TMPs. Then run a single
 * 5-limb chained-ADC accumulation per "column contribution":
 *   - chain 1: add lo0..hi3 to acc[0..4] (5 limbs)
 *   - But we have 8 products from 4 MULs. Group as:
 *       col 0: lo0           → acc[0]
 *       col 1: hi0 + lo1     → acc[1]   (combined inside the chain)
 *       col 2: hi1 + lo2     → acc[2]
 *       col 3: hi2 + lo3     → acc[3]
 *       col 4: hi3           → acc[4]
 *
 * For each col k>0 the value is (hi_{k-1} + lo_k). We compute the combined
 * column value INTO a TMP first, tracking its own carry, then add to acc
 * via a single chain. This keeps acc-side chain to 2-source per limb.
 *
 * After the 4-MUL block:
 *   TMP0..TMP7 hold lo0,hi0,lo1,hi1,lo2,hi2,lo3,hi3.
 * Then compute (parallel of original Phase A):
 *   T0  := lo0
 *   T1  := hi0 + lo1   (c1 = carry)
 *   T2  := hi1 + lo2 + c1 (c2 = carry, value 0..1 actually since hi+lo+c≤2^65)
 *   T3  := hi2 + lo3 + c2
 *   T4  := hi3 + c3
 * Each step is 2 ops (ADD/ADC + GFL_RR).
 *
 * Then add column values to acc with chained ADC over 5 limbs + 1 overflow.
 */

/* SCHOOLBOOK_ROW_START: load RDI and RDX with a_src (same as original) */
#define SCHOOLBOOK_ROW_START(a_src) \
    { ZEROEXT_DSZ64_DR(RDX, a_src), ZEROEXT_DSZ64_DR(RDI, a_src), \
      NOP, NOP_SEQWORD }

/*
 * SCHOOLBOOK_ROW_CHAINED: chained-ADC body for one schoolbook row.
 *
 * Inputs:  RDX = a[i] (also in RDI for refresh), TMP10-13 = b[0..3]
 *          accumulator in R15,R9,R10,R13,RAX,TMP14
 *          TMP9 = 0 (pre-loaded zero register)
 * Clobbers: RCX, RDX, TMP0..TMP8
 */
#define SCHOOLBOOK_ROW_CHAINED \
    /* ─── 4 MULs, saving lo/hi to TMP0..TMP7 ─── */ \
    /* MUL 0: a × b[0]  →  RCX:RDX = hi:lo */ \
    { MUL_DSZ64_DRR(RCX, TMP10, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP0, RDX),   /* save lo0 */ \
      ZEROEXT_DSZ64_DR(TMP1, RCX),   /* save hi0 */ \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    /* MUL 1: a × b[1] */ \
    { MUL_DSZ64_DRR(RCX, TMP11, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP2, RDX),   /* save lo1 */ \
      ZEROEXT_DSZ64_DR(TMP3, RCX),   /* save hi1 */ \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    /* MUL 2: a × b[2] */ \
    { MUL_DSZ64_DRR(RCX, TMP12, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP4, RDX),   /* save lo2 */ \
      ZEROEXT_DSZ64_DR(TMP5, RCX),   /* save hi2 */ \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    /* MUL 3: a × b[3] */ \
    { MUL_DSZ64_DRR(RCX, TMP13, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP6, RDX),   /* save lo3 */ \
      ZEROEXT_DSZ64_DR(TMP7, RCX),   /* save hi3 */ \
      NOP, NOP_SEQWORD }, \
    /* ─── form column values T0..T4 with chained carries ─── */ \
    /* T1 := hi0 + lo1, GFL */ \
    { ADD_DSZ64_DRR(TMP1, TMP1, TMP2), GENARITHFLAGS_RR(TMP1, TMP1), \
      NOP, NOP_SEQWORD }, \
    /* T2 := hi1 + lo2 + CF, GFL */ \
    { ADC_DSZ64_DRR(TMP3, TMP3, TMP4), GENARITHFLAGS_RR(TMP3, TMP3), \
      NOP, NOP_SEQWORD }, \
    /* T3 := hi2 + lo3 + CF, GFL */ \
    { ADC_DSZ64_DRR(TMP5, TMP5, TMP6), GENARITHFLAGS_RR(TMP5, TMP5), \
      NOP, NOP_SEQWORD }, \
    /* T4 := hi3 + CF (ADC with zero), GFL not needed since this is the top */ \
    { ADC_DSZ64_DRR(TMP7, TMP7, TMP9), NOP, NOP, NOP_SEQWORD }, \
    /* ─── add T0(=TMP0), T1(=TMP1), T2(=TMP3), T3(=TMP5), T4(=TMP7) ─── \
     * to acc[0..4].  Chain dest MUST be TMP (arch-dest GFL_RR leaks CF \
     * per arch_gfl_chain.c, 2026-05-22). Each col TMP holds the col value \
     * as src then gets overwritten with new acc value. Writeback to arch \
     * follows in 2 triads. */ \
    /* acc[0]: TMP0 = R15 + TMP0 */ \
    { ADD_DSZ64_DRR(TMP0, R15, TMP0), GENARITHFLAGS_RR(TMP0, TMP0), \
      NOP, NOP_SEQWORD }, \
    /* acc[1]: TMP1 = R9 + TMP1 + CF */ \
    { ADC_DSZ64_DRR(TMP1, R9, TMP1), GENARITHFLAGS_RR(TMP1, TMP1), \
      NOP, NOP_SEQWORD }, \
    /* acc[2]: TMP3 = R10 + TMP3 + CF */ \
    { ADC_DSZ64_DRR(TMP3, R10, TMP3), GENARITHFLAGS_RR(TMP3, TMP3), \
      NOP, NOP_SEQWORD }, \
    /* acc[3]: TMP5 = R13 + TMP5 + CF */ \
    { ADC_DSZ64_DRR(TMP5, R13, TMP5), GENARITHFLAGS_RR(TMP5, TMP5), \
      NOP, NOP_SEQWORD }, \
    /* acc[4]: TMP7 = RAX + TMP7 + CF */ \
    { ADC_DSZ64_DRR(TMP7, RAX, TMP7), GENARITHFLAGS_RR(TMP7, TMP7), \
      NOP, NOP_SEQWORD }, \
    /* acc[5] (overflow word): TMP14 = 0 + 0 + CF (fresh per row, not accumulating) */ \
    { ADC_DSZ64_DRR(TMP14, TMP9, TMP9), NOP, NOP, NOP_SEQWORD }, \
    /* writeback chain results back to arch (SHIFT operates on arch regs) */ \
    { ZEROEXT_DSZ64_DR(R15, TMP0), ZEROEXT_DSZ64_DR(R9, TMP1), \
      ZEROEXT_DSZ64_DR(R10, TMP3), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(R13, TMP5), ZEROEXT_DSZ64_DR(RAX, TMP7), \
      NOP, NOP_SEQWORD }

/* SHIFT same as original */
#define SHIFT_ROW(save_reg) \
    { ZEROEXT_DSZ64_DR(save_reg, R15), ZEROEXT_DSZ64_DR(R15, R9), \
      ZEROEXT_DSZ64_DR(R9, R10), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(R10, R13), ZEROEXT_DSZ64_DR(R13, RAX), \
      ZEROEXT_DSZ64_DR(RAX, TMP14), NOP_SEQWORD }

static void install_solinas_mul_chained_patch(void) {
    ucode_t patch[] = {

    /* ─── PREP ─── */
    { ZEROEXT_DSZ64_DR(TMP10, RSI), ZEROEXT_DSZ64_DR(TMP11, R12),
      ZEROEXT_DSZ64_DR(TMP12, R11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R14), ZEROEXT_DSZ64_DR(R14, RDX),
      ZEROEXT_DSZ64_DR(TMP15, RBP), NOP_SEQWORD },
    /* TMP9 = 0  (zero source for chained-ADC "+0") */
    /* Also zero TMP14 (overflow word) — it must start at 0 each call */
    { ZEROEXT_DSZ32_DI(TMP9, 0), ZEROEXT_DSZ32_DI(TMP14, 0),
      NOP, NOP_SEQWORD },

    /* ─── ROW 0 ─── */
    SCHOOLBOOK_ROW_START(RDI),
    SCHOOLBOOK_ROW_CHAINED,
    SHIFT_ROW(RSI),

    /* ─── ROW 1 ─── */
    SCHOOLBOOK_ROW_START(R14),
    SCHOOLBOOK_ROW_CHAINED,
    SHIFT_ROW(R12),

    /* ─── ROW 2 ─── */
    SCHOOLBOOK_ROW_START(TMP15),
    SCHOOLBOOK_ROW_CHAINED,
    SHIFT_ROW(R11),

    /* ─── ROW 3 ─── */
    SCHOOLBOOK_ROW_START(RBX),
    SCHOOLBOOK_ROW_CHAINED,

    /*
     * Post-schoolbook state:
     *   R15=p[3], R9=p[4], R10=p[5], R13=p[6], RAX=p[7], TMP14=p[8] (=0 if math works)
     *   RSI=p[0], R12=p[1], R11=p[2]
     */

    /* ─── SOLINAS REDUCTION (chained ADC) ─── */
    /* Step 1: multiply p[4..7] by 38 → keep hi/lo */
    /*   R9 ← lo(38*p[4]), TMP0 ← hi(38*p[4])
     *   R10 ← lo(38*p[5]), TMP1 ← hi(38*p[5])
     *   R13 ← lo(38*p[6]), TMP2 ← hi(38*p[6])
     *   RAX ← lo(38*p[7]), RCX ← hi(38*p[7])
     * Fold TMP14 (carry8) into RAX before multiplying — same as original.
     */
    { ADD_DSZ64_DRR(RAX, RAX, TMP14), MUL_DSZ64_DIR(RCX, 38, R9),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP0, RCX), MUL_DSZ64_DIR(RCX, 38, R10),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP1, RCX), MUL_DSZ64_DIR(RCX, 38, R13),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP2, RCX), MUL_DSZ64_DIR(RCX, 38, RAX),
      NOP, NOP_SEQWORD },
    /* now: R9=lo4, R10=lo5, R13=lo6, RAX=lo7, TMP0=hi4, TMP1=hi5, TMP2=hi6, RCX=hi7 */

    /*
     * Form the 5-limb value (38*p_upper) at columns 0..4:
     *   col 0:  lo4
     *   col 1:  hi4 + lo5
     *   col 2:  hi5 + lo6
     *   col 3:  hi6 + lo7
     *   col 4:  hi7
     */
    /* col 1: TMP0 = hi4 + lo5, GFL */
    { ADD_DSZ64_DRR(TMP0, TMP0, R10), GENARITHFLAGS_RR(TMP0, TMP0),
      NOP, NOP_SEQWORD },
    /* col 2: TMP1 = hi5 + lo6 + CF, GFL */
    { ADC_DSZ64_DRR(TMP1, TMP1, R13), GENARITHFLAGS_RR(TMP1, TMP1),
      NOP, NOP_SEQWORD },
    /* col 3: TMP2 = hi6 + lo7 + CF, GFL */
    { ADC_DSZ64_DRR(TMP2, TMP2, RAX), GENARITHFLAGS_RR(TMP2, TMP2),
      NOP, NOP_SEQWORD },
    /* col 4: RCX = hi7 + 0 + CF (no GFL — last carry will be folded later) */
    { ADC_DSZ64_DRR(RCX, RCX, TMP9), NOP, NOP, NOP_SEQWORD },

    /* Now col 0..4 live in: R9 (lo4), TMP0, TMP1, TMP2, RCX.
     * Route r[0..3] through TMPs (TMP3, TMP4, TMP5, TMP6) — arch dests
     * leak CF (arch_gfl_chain.c, 2026-05-22), so the whole chain stays
     * in TMPs and only writes to arch at the very end.
     */
    /* TMP3 = RSI + R9 (= p[0] + lo4) */
    { ADD_DSZ64_DRR(TMP3, RSI, R9), GENARITHFLAGS_RR(TMP3, TMP3),
      NOP, NOP_SEQWORD },
    /* TMP4 = R12 + TMP0 + CF */
    { ADC_DSZ64_DRR(TMP4, R12, TMP0), GENARITHFLAGS_RR(TMP4, TMP4),
      NOP, NOP_SEQWORD },
    /* TMP5 = R11 + TMP1 + CF */
    { ADC_DSZ64_DRR(TMP5, R11, TMP1), GENARITHFLAGS_RR(TMP5, TMP5),
      NOP, NOP_SEQWORD },
    /* TMP6 = R15 + TMP2 + CF */
    { ADC_DSZ64_DRR(TMP6, R15, TMP2), GENARITHFLAGS_RR(TMP6, TMP6),
      NOP, NOP_SEQWORD },
    /* Capture final carry (col 4 + CF) into TMP7 */
    { ADC_DSZ64_DRR(TMP7, RCX, TMP9), NOP, NOP, NOP_SEQWORD },

    /*
     * Final fold: total_carry * 38 added to r[0], chain through r[1..3]
     */
    { MUL_DSZ64_DIR(RCX, 38, TMP7), NOP, NOP, NOP_SEQWORD },
    /* TMP3 += TMP7 (=38*total_carry lo) */
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP7), GENARITHFLAGS_RR(TMP3, TMP3),
      NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP4, TMP4, TMP9), GENARITHFLAGS_RR(TMP4, TMP4),
      NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP5, TMP5, TMP9), GENARITHFLAGS_RR(TMP5, TMP5),
      NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP6, TMP6, TMP9), NOP, NOP, NOP_SEQWORD },

    /* Writeback: r[0..3] from TMP3..TMP6 → R15, R9, R10, R13 */
    { ZEROEXT_DSZ64_DR(R15, TMP3), ZEROEXT_DSZ64_DR(R9, TMP4),
      ZEROEXT_DSZ64_DR(R10, TMP5), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R13, TMP6), NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("solinas_mul_chained: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* fe_mul via microcode (same wrapper as original) */
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
    /* Conditional subtract of p */
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

/* ── verification (same as original) ── */
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

/* ── timing ── */
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
    printf("=== curve25519 Solinas mul CHAINED-ADC: microcode vs native ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_solinas_mul_chained_patch();

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
    printf("Microcode chained: min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    init_match_and_patch(); do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
