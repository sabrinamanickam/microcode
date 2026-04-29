/*
 * asm_op_p224_mul.c — Montgomery multiplication for P-224 via microcode
 *
 * 2-iter-per-vmwrite patch (42 triads/iter, 88 total).
 * Only the inline asm differs from p224_sq: b loaded from a separate pointer.
 *
 * P-224: p = 2^224 - 2^96 + 1
 *   p[0] = 0x0000000000000001
 *   p[1] = 0xFFFFFFFF00000000
 *   p[2] = 0xFFFFFFFFFFFFFFFF
 *   p[3] = 0x00000000FFFFFFFF
 *   mu   = 0xFFFFFFFFFFFFFFFF  (= -1 mod 2^64)
 *
 * Build:  make PROG=asm_op_p224_mul
 * Run:    sudo taskset -c 0 ./asm_op_p224_mul_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

static const uint64_t P224_P[4] = {
    UINT64_C(0x0000000000000001), UINT64_C(0xFFFFFFFF00000000),
    UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x00000000FFFFFFFF)
};

/* ── fiat-crypto reference (the REAL GCC baseline CryptOpt compares against) ── */
#include "../curvesC/p224_mul.c"

static void fe_mul_fiat(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    fiat_p224_mul(out, a, b);
}

/* ── fe_mul native C ─────────────────────────────────────────── */

static inline void mont_iteration(uint64_t acc[5], uint64_t a_i,
                                   const uint64_t *b) {
    __uint128_t t;
    uint64_t c;
    t = (__uint128_t)a_i * b[0];
    uint64_t t0 = (uint64_t)t, t0h = (uint64_t)(t >> 64);
    t = (__uint128_t)a_i * b[1] + t0h;
    uint64_t t1 = (uint64_t)t, t1h = (uint64_t)(t >> 64);
    t = (__uint128_t)a_i * b[2] + t1h;
    uint64_t t2 = (uint64_t)t, t2h = (uint64_t)(t >> 64);
    t = (__uint128_t)a_i * b[3] + t2h;
    uint64_t t3 = (uint64_t)t, t4 = (uint64_t)(t >> 64);

    t = (__uint128_t)acc[0] + t0; acc[0] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[1] + t1 + c; acc[1] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[2] + t2 + c; acc[2] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[3] + t3 + c; acc[3] = (uint64_t)t; c = (uint64_t)(t >> 64);
    __uint128_t acc4_full = (__uint128_t)acc[4] + t4 + c;
    uint64_t acc4 = (uint64_t)acc4_full;
    uint64_t acc4_hi = (uint64_t)(acc4_full >> 64);

    /* mu = -1, so m = acc[0] * (-1) = -acc[0] mod 2^64 */
    uint64_t m = acc[0] * UINT64_C(0xFFFFFFFFFFFFFFFF);
    /* p[0] = 1, so m*p[0] = m (no MUL needed) */
    /* m * p[1] */
    t = (__uint128_t)m * UINT64_C(0xFFFFFFFF00000000);
    uint64_t mp1_lo = (uint64_t)t, mp1_hi = (uint64_t)(t >> 64);
    /* m * p[2] */
    t = (__uint128_t)m * UINT64_C(0xFFFFFFFFFFFFFFFF);
    uint64_t mp2_lo = (uint64_t)t, mp2_hi = (uint64_t)(t >> 64);
    /* m * p[3] */
    t = (__uint128_t)m * UINT64_C(0xFFFFFFFF);
    uint64_t mp3_lo = (uint64_t)t, mp3_hi = (uint64_t)(t >> 64);

    /* Chain: red[0]=m, red[1]=mp1_lo */
    t = (__uint128_t)mp1_hi + mp2_lo;
    uint64_t red2 = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)mp2_hi + mp3_lo + c;
    uint64_t red3 = (uint64_t)t; c = (uint64_t)(t >> 64);
    uint64_t red4 = mp3_hi + c;  /* no overflow: mp3_hi < 2^32 */

    /* Phase C: red[0]=m, red[1]=mp1_lo */
    t = (__uint128_t)acc[0] + m;  c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[1] + mp1_lo + c; acc[0] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[2] + red2 + c; acc[1] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[3] + red3 + c; acc[2] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc4 + red4 + c; acc[3] = (uint64_t)t;
    acc[4] = (uint64_t)(t >> 64) + acc4_hi;
}

static inline void cond_subtract(const uint64_t acc[5], uint64_t *out) {
    uint64_t diff[4];
    __uint128_t b128;
    b128 = (__uint128_t)acc[0] - UINT64_C(0x0000000000000001);
    diff[0] = (uint64_t)b128;
    b128 = (__uint128_t)acc[1] - UINT64_C(0xFFFFFFFF00000000) - ((uint64_t)(b128 >> 64) & 1);
    diff[1] = (uint64_t)b128;
    b128 = (__uint128_t)acc[2] - UINT64_C(0xFFFFFFFFFFFFFFFF) - ((uint64_t)(b128 >> 64) & 1);
    diff[2] = (uint64_t)b128;
    b128 = (__uint128_t)acc[3] - UINT64_C(0x00000000FFFFFFFF) - ((uint64_t)(b128 >> 64) & 1);
    diff[3] = (uint64_t)b128;
    b128 = (__uint128_t)acc[4] - 0 - ((uint64_t)(b128 >> 64) & 1);
    uint64_t mask = (uint64_t)0 - ((uint64_t)(b128 >> 64) & 1);
    out[0] = (acc[0] & mask) | (diff[0] & ~mask);
    out[1] = (acc[1] & mask) | (diff[1] & ~mask);
    out[2] = (acc[2] & mask) | (diff[2] & ~mask);
    out[3] = (acc[3] & mask) | (diff[3] & ~mask);
}

static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t acc[5] = {0};
    mont_iteration(acc, a[0], b);
    mont_iteration(acc, a[1], b);
    mont_iteration(acc, a[2], b);
    mont_iteration(acc, a[3], b);
    cond_subtract(acc, out);
}

/* ── reference ───────────────────────────────────────────────── */

static void fe_mul_reference(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t acc[5] = {0};
    for (int i = 0; i < 4; i++) {
        __uint128_t c = 0;
        for (int j = 0; j < 4; j++) {
            c += (__uint128_t)a[i] * b[j] + acc[j];
            acc[j] = (uint64_t)c; c >>= 64;
        }
        __uint128_t word4 = (__uint128_t)acc[4] + (uint64_t)c;
        uint64_t m = acc[0] * UINT64_C(0xFFFFFFFFFFFFFFFF);
        c = 0;
        for (int j = 0; j < 4; j++) {
            c += (__uint128_t)m * P224_P[j] + acc[j];
            acc[j] = (uint64_t)c; c >>= 64;
        }
        word4 += (uint64_t)c;
        acc[0]=acc[1]; acc[1]=acc[2]; acc[2]=acc[3];
        acc[3]=(uint64_t)word4; acc[4]=(uint64_t)(word4>>64);
    }
    cond_subtract(acc, out);
}

/* ── microcode (2-iter-per-vmwrite, 42 triads/iter, 88 total) ── */

/*
 * Arch regs (persist across calls):
 *   R15=acc[0]  R9=acc[1]  R10=acc[2]  R13=acc[3]  RAX=acc[4]
 *   RSI=b[0]    R12=b[1]   R11=b[2]    R14=b[3]
 *   R8=0xFFFFFFFFFFFFFFFF  (mu AND p[2] — same value!)
 *   RBX=0xFFFFFFFF00000000 (p[1])
 *   RDI=a_i (set by caller before each vmwrite)
 *
 * TMP regs (set at start of each iteration):
 *   TMP10-13 = b[0..3] (reloaded from RSI,R12,R11,R14)
 *   TMP9 = p[3] = 0xFFFFFFFF (precomputed in Phase A T1 as SHR(R8,32))
 *   TMP15 = a[i+1] (from RDX, saved by PREP)
 *
 * p[0] = 1 -> m * p[0] = m, no MUL needed.
 * TMP9 preserved across Phase B because MUL(RCX, TMP9, RDX) doesn't clobber srcA.
 */

#define MONT_ITER \
    /* ── PHASE A: schoolbook a_i(RDI) x b(TMP10-13) (13 triads) ── */ \
    /* Precompute p[3] = SHR(R8, 32) = 0xFFFFFFFF in TMP9 (free slot) */ \
    { ZEROEXT_DSZ64_DR(RDX, RDI), SHR_DSZ64_DRI(TMP9, R8, 32), \
      NOP, NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP10, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP0, RDX), ZEROEXT_DSZ64_DR(TMP1, RCX), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    /* MUL b1*ai + Phase A' w0 merge */ \
    { MUL_DSZ64_DRR(RCX, TMP11, RDX), ADD_DSZ64_DRR(TMP0, R15, TMP0), \
      SETCC_CONDB_DR(TMP3, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP2, TMP1, RDX), SETCC_CONDB_DR(TMP8, TMP2), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    /* writeback w0->R15 + early-start Phase A' w1 */ \
    { ZEROEXT_DSZ64_DR(RDX, RDI), ZEROEXT_DSZ64_DR(R15, TMP0), \
      ADD_DSZ64_DRR(TMP0, R9, TMP2), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP12, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP4, TMP1, RDX), SETCC_CONDB_DR(TMP5, TMP4), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP8), SETCC_CONDB_DR(TMP6, TMP4), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    /* MUL b3*ai + combine w2 carries in slot 1 */ \
    { MUL_DSZ64_DRR(RCX, TMP13, RDX), ADD_DSZ64_DRR(TMP8, TMP5, TMP6), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP5, TMP1, RDX), SETCC_CONDB_DR(TMP6, TMP5), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP5, TMP5, TMP8), SETCC_CONDB_DR(TMP7, TMP5), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP8, TMP6, TMP7), ADD_DSZ64_DRR(TMP6, RCX, TMP8), \
      NOP, NOP_SEQWORD }, \
    \
    /* ── PHASE A': add product to acc (8 triads) ── */ \
    /* w1 triple-pack */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R9, TMP0), \
      ADD_DSZ64_DRR(TMP0, R10, TMP4), NOP_SEQWORD }, \
    /* w2 triple-pack */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R10, TMP0), \
      ADD_DSZ64_DRR(TMP0, R13, TMP5), NOP_SEQWORD }, \
    /* w3 triple-pack */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R13, TMP0), \
      ADD_DSZ64_DRR(TMP0, RAX, TMP6), NOP_SEQWORD }, \
    /* w4 triple-pack */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* w4 combine + Phase B setup (ZEROEXT RDX<-R15) in slot 2 */ \
    { ADD_DSZ64_DRR(TMP14, TMP1, TMP8), ZEROEXT_DSZ64_DR(RAX, TMP0), \
      ZEROEXT_DSZ64_DR(RDX, R15), NOP_SEQWORD }, \
    \
    /* ── PHASE B: m*mu, m*p[1], m*p[2], m*p[3], chain (11 triads) ── */ \
    /* mu MUL: m = acc[0] * R8 (mu = -1 = R8) */ \
    { MUL_DSZ64_DRR(RCX, R8, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* save m + m*p[1] merged (both read RDX, MUL in slot 1) */ \
    { ZEROEXT_DSZ64_DR(TMP6, RDX), MUL_DSZ64_DRR(RCX, RBX, RDX), \
      NOP, NOP_SEQWORD }, \
    /* save lo=red[1], hi, reload m */ \
    { ZEROEXT_DSZ64_DR(TMP7, RDX), ZEROEXT_DSZ64_DR(TMP3, RCX), \
      ZEROEXT_DSZ64_DR(RDX, TMP6), NOP_SEQWORD }, \
    /* m * p[2] = m * R8 + w0 discard (TMP8 = w0 carry via WAR) */ \
    { MUL_DSZ64_DRR(RCX, R8, RDX), ADD_DSZ64_DRR(TMP0, R15, TMP6), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* save m*p2 lo/hi, reload m */ \
    { ZEROEXT_DSZ64_DR(TMP4, RDX), ZEROEXT_DSZ64_DR(TMP5, RCX), \
      ZEROEXT_DSZ64_DR(RDX, TMP6), NOP_SEQWORD }, \
    /* m * p[3] = m * TMP9 */ \
    { MUL_DSZ64_DRR(RCX, TMP9, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* save m*p3 lo/hi + start red[2] chain */ \
    { ZEROEXT_DSZ64_DR(TMP6, RDX), ZEROEXT_DSZ64_DR(TMP2, RCX), \
      ADD_DSZ64_DRR(TMP3, TMP3, TMP4), NOP_SEQWORD }, \
    /* red[2] carry + red[3] base */ \
    { SETCC_CONDB_DR(TMP1, TMP3), ADD_DSZ64_DRR(TMP5, TMP5, TMP6), \
      SETCC_CONDB_DR(TMP0, TMP5), NOP_SEQWORD }, \
    /* red[3] += carry2 */ \
    { ADD_DSZ64_DRR(TMP5, TMP5, TMP1), SETCC_CONDB_DR(TMP1, TMP5), \
      NOP, NOP_SEQWORD }, \
    /* carry3 + red[4] + start Phase C w1 in slot 2 */ \
    { ADD_DSZ64_DRR(TMP1, TMP0, TMP1), ADD_DSZ64_DRR(TMP2, TMP2, TMP1), \
      ADD_DSZ64_DRR(TMP0, R9, TMP7), NOP_SEQWORD }, \
    \
    /* ── PHASE C: add red to acc, shift (9 triads) ── */ \
    /* w1 triple-pack (w0 carry in TMP8 from Phase B) */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP8), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* w1 combine + writeback R15 + w2 start */ \
    { ADD_DSZ64_DRR(TMP6, TMP1, TMP8), ZEROEXT_DSZ64_DR(R15, TMP0), \
      ADD_DSZ64_DRR(TMP0, R10, TMP3), NOP_SEQWORD }, \
    /* w2 triple-pack */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP6), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* w2 combine + writeback R9 + w3 start */ \
    { ADD_DSZ64_DRR(TMP6, TMP1, TMP8), ZEROEXT_DSZ64_DR(R9, TMP0), \
      ADD_DSZ64_DRR(TMP0, R13, TMP5), NOP_SEQWORD }, \
    /* w3 triple-pack */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP6), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* w3 combine + writeback R10 + w4 start */ \
    { ADD_DSZ64_DRR(TMP6, TMP1, TMP8), ZEROEXT_DSZ64_DR(R10, TMP0), \
      ADD_DSZ64_DRR(TMP0, RAX, TMP2), NOP_SEQWORD }, \
    /* w4 triple-pack */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP6), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* w4 combine */ \
    { ZEROEXT_DSZ64_DR(R13, TMP0), ADD_DSZ64_DRR(TMP0, TMP1, TMP8), \
      NOP, NOP_SEQWORD }, \
    /* acc[4] = carry_w4 + Phase A' carry (no red4_carry, red[4] < 2^32) */ \
    { ADD_DSZ64_DRR(RAX, TMP0, TMP14), NOP, NOP, NOP_SEQWORD }

static void install_p224_mul_patch(void) {
    ucode_t patch[] = {
    /* PREP: reload b->TMPs, save a[i+1] from RDX->TMP15 */
    { ZEROEXT_DSZ64_DR(TMP10, RSI), ZEROEXT_DSZ64_DR(TMP11, R12),
      ZEROEXT_DSZ64_DR(TMP12, R11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R14), ZEROEXT_DSZ64_DR(TMP15, RDX),
      NOP, NOP_SEQWORD },
    MONT_ITER,
    { ZEROEXT_DSZ64_DR(RDI, TMP15), NOP, NOP, NOP_SEQWORD },
    MONT_ITER,
    { NOP, NOP, NOP, END_SEQWORD }
    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("p224_mul: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ── fe_mul via microcode ─────────────────────────────────────── */

static void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t acc[5];
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_b   asm("rbx") = (uint64_t *)b;
    register uint64_t *_acc asm("r15") = acc;

    asm volatile(
        "push r15\n\t"
        "push rcx\n\t"

        /* Load b[0..3] from rbx -> arch regs BEFORE overwriting rbx */
        "mov rsi, [rbx]\n\t"
        "mov r12, [rbx + 8]\n\t"
        "mov r11, [rbx + 16]\n\t"
        "mov r14, [rbx + 24]\n\t"

        /* p constants — NO RBP needed! */
        "mov r8, -1\n\t"                          /* mu AND p[2] = all ones */
        "mov rbx, 0xFFFFFFFF00000000\n\t"          /* p[1] */
        /* p[3] = 0xFFFFFFFF computed on-the-fly as SHR(R8,32) in TMP9 */

        /* Zero accumulator */
        "xor r15d, r15d\n\t"
        "xor r9d, r9d\n\t"
        "xor r10d, r10d\n\t"
        "xor r13d, r13d\n\t"
        "xor eax, eax\n\t"

        /* Iterations 0-1: a[0]->RDI, a[1]->RDX */
        "mov rcx, [rsp]\n\t"
        "mov rdi, [rcx]\n\t"
        "mov rdx, [rcx + 8]\n\t"
        "vmwrite rcx, rdx\n\t"

        /* Iterations 2-3: a[2]->RDI, a[3]->RDX */
        "mov rcx, [rsp]\n\t"
        "mov rdi, [rcx + 16]\n\t"
        "mov rdx, [rcx + 24]\n\t"
        "vmwrite rcx, rdx\n\t"

        /* Store acc[0..4] */
        "pop rcx\n\t"
        "pop rcx\n\t"
        "mov [rcx],      r15\n\t"
        "mov [rcx + 8],  r9\n\t"
        "mov [rcx + 16], r10\n\t"
        "mov [rcx + 24], r13\n\t"
        "mov [rcx + 32], rax\n\t"

        : "+r"(_a), "+r"(_b), "+r"(_acc)
        :
        : "rax", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14",
          "memory", "cc"
    );

    cond_subtract(acc, out);
}

/* ── verification ────────────────────────────────────────────── */

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void rand_mod_p(uint64_t out[4], uint64_t *rng) {
    for (;;) {
        for (int j = 0; j < 4; j++) out[j] = splitmix64(rng);
        /* p[3] = 0xFFFFFFFF, so top limb must be < 0xFFFFFFFF */
        if (out[3] > UINT64_C(0xFFFFFFFF)) {
            out[3] &= UINT64_C(0xFFFFFFFF);  /* truncate to 32 bits */
        }
        int lt = 0;
        for (int j = 3; j >= 0; j--) {
            if (out[j] < P224_P[j]) { lt = 1; break; }
            if (out[j] > P224_P[j]) break;
        }
        if (lt) break;
    }
}

static int verify_all(void) {
    int pass = 0, fail = 0;

    printf("--- Known vectors ---\n");
    /* Montgomery form of 1: R mod p = 2^256 mod p
     * 1_mont = {0xFFFFFFFF00000000, 0xFFFFFFFFFFFFFFFF, 0, 0} */
    uint64_t zero[4] = {0};
    uint64_t one_m[4] = {UINT64_C(0xFFFFFFFF00000000), UINT64_C(0xFFFFFFFFFFFFFFFF), 0, 0};
    uint64_t small[4] = {3, 0, 0, 0};

    struct { const char *name; const uint64_t *a; const uint64_t *b; const uint64_t *exp; int has; } vecs[] = {
        { "0*0",       zero,  zero,  zero, 1 },
        { "0*small",   zero,  small, zero, 1 },
        { "small*0",   small, zero,  zero, 1 },
        { "1m*small",  one_m, small, NULL, 0 },
        { "small*small", small, small, NULL, 0 },
    };

    for (int i = 0; i < 5; i++) {
        uint64_t ref[4], nat[4], ucd[4];
        fe_mul_reference(vecs[i].a, vecs[i].b, ref);
        fe_mul_native(vecs[i].a, vecs[i].b, nat);
        fe_mul_ucode(vecs[i].a, vecs[i].b, ucd);
        int ok = !memcmp(ref, nat, 32) && !memcmp(ref, ucd, 32);
        if (vecs[i].has) ok = ok && !memcmp(ref, vecs[i].exp, 32);
        if (!ok) {
            printf("  FAIL [%s]\n", vecs[i].name);
            for (int j=0;j<4;j++)
                printf("    [%d] ref=%016"PRIx64" nat=%016"PRIx64" ucd=%016"PRIx64"%s%s\n",
                    j, ref[j], nat[j], ucd[j],
                    ref[j]!=nat[j]?" nat***":"", ref[j]!=ucd[j]?" ucd***":"");
        } else printf("  PASS [%s]\n", vecs[i].name);
        if (ok) pass++; else fail++;
    }

    printf("\n--- Random mul (10000) ---\n");
    uint64_t rng = 0xB224CAFE12345678ULL;
    int rp = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t a[4], b[4], ref[4], nat[4], ucd[4];
        rand_mod_p(a, &rng);
        rand_mod_p(b, &rng);
        fe_mul_reference(a, b, ref);
        fe_mul_native(a, b, nat);
        fe_mul_ucode(a, b, ucd);
        if (!memcmp(ref, nat, 32) && !memcmp(ref, ucd, 32)) rp++;
        else {
            printf("  FAIL #%d\n", i);
            for (int j=0;j<4;j++)
                printf("    [%d] ref=%016lx nat=%016lx ucd=%016lx%s%s\n",
                    j, ref[j], nat[j], ucd[j],
                    ref[j]!=nat[j]?" nat***":"", ref[j]!=ucd[j]?" ucd***":"");
            break;
        }
    }
    printf("  %d / 10000 PASS\n", rp);
    pass += rp; if (rp < 10000) fail += (10000 - rp);

    printf("\n--- Commutativity (1000) ---\n");
    rp = 0;
    for (int i = 0; i < 1000; i++) {
        uint64_t a[4], b[4], ab[4], ba[4];
        rand_mod_p(a, &rng);
        rand_mod_p(b, &rng);
        fe_mul_ucode(a, b, ab);
        fe_mul_ucode(b, a, ba);
        if (!memcmp(ab, ba, 32)) rp++;
        else { printf("  FAIL #%d: a*b != b*a\n", i); break; }
    }
    printf("  %d / 1000 PASS\n", rp);
    pass += rp; if (rp < 1000) fail += (1000 - rp);

    printf("\n--- mul(a,a) == sq(a) (1000) ---\n");
    rp = 0;
    for (int i = 0; i < 1000; i++) {
        uint64_t a[4], mul_aa[4], sq_a[4];
        rand_mod_p(a, &rng);
        fe_mul_ucode(a, a, mul_aa);
        fe_mul_native(a, a, sq_a);  /* native sq as reference */
        if (!memcmp(mul_aa, sq_a, 32)) rp++;
        else { printf("  FAIL #%d: mul(a,a) != sq(a)\n", i); break; }
    }
    printf("  %d / 1000 PASS\n", rp);
    pass += rp; if (rp < 1000) fail += (1000 - rp);

    printf("\n=== %d passed, %d failed ===\n\n", pass, fail);
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

#define BATCH 10000
#define REPS  200

int main(void) {
    printf("=== P-224 Montgomery multiply: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_p224_mul_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED, skipping benchmark.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    /* Benchmark with 1_mont and a second distinct value */
    uint64_t sa[4] = {UINT64_C(0xFFFFFFFF00000000), UINT64_C(0xFFFFFFFFFFFFFFFF), 0, 0};
    uint64_t sb[4] = {UINT64_C(0x6B17D1F2E12C4247), UINT64_C(0xF8BCE6E563A440F2),
                      UINT64_C(0x7037D812DEB33A0F), UINT64_C(0x4FE342E2)};
    uint64_t ta[4], tb[4], t0, t1, min, sum;

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, 32); memcpy(tb, sb, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_native(ta, tb, ta);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Naive -O3:   min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n", min/BATCH, sum/REPS/BATCH);

    /* fiat-crypto (the real GCC baseline CryptOpt compares against) */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, 32); memcpy(tb, sb, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_fiat(ta, tb, ta);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Fiat-crypto: min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n", min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, 32); memcpy(tb, sb, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_ucode(ta, tb, ta);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode:   min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n", min/BATCH, sum/REPS/BATCH);

    init_match_and_patch(); do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
