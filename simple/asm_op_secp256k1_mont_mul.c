/*
 * asm_op_secp256k1_mont_mul.c — Montgomery multiplication for secp256k1 via microcode
 *
 * 2-iter-per-vmwrite patch (45 triads/iter, 94 total).
 * secp256k1: p = 2^256 - 2^32 - 977
 *   p[0] = 0xFFFFFFFEFFFFFC2F, p[1..3] = 0xFFFFFFFFFFFFFFFF
 *   mu = -p[0]^{-1} mod 2^64 = 0xD838091DD2253531
 *
 * Only the inline asm differs from sq: b loaded from a separate pointer.
 *
 * Build:  make PROG=asm_op_secp256k1_mont_mul
 * Run:    sudo taskset -c 0 ./asm_op_secp256k1_mont_mul_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

static const uint64_t SECP256K1_P[4] = {
    UINT64_C(0xFFFFFFFEFFFFFC2F), UINT64_C(0xFFFFFFFFFFFFFFFF),
    UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0xFFFFFFFFFFFFFFFF)
};

#define SECP256K1_MU UINT64_C(0xD838091DD2253531)

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

    uint64_t m = acc[0] * SECP256K1_MU;
    t = (__uint128_t)m * SECP256K1_P[0];
    uint64_t mp0_lo = (uint64_t)t, mp0_hi = (uint64_t)(t >> 64);
    t = (__uint128_t)m * UINT64_C(0xFFFFFFFFFFFFFFFF);
    uint64_t mpR8_lo = (uint64_t)t, mpR8_hi = (uint64_t)(t >> 64);

    /* Chain */
    t = (__uint128_t)mp0_hi + mpR8_lo;
    uint64_t red1 = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)mpR8_hi + mpR8_lo + c;
    uint64_t red2 = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)mpR8_hi + mpR8_lo + c;
    uint64_t red3 = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)mpR8_hi + c;
    uint64_t red4 = (uint64_t)t;
    uint64_t red4_hi = (uint64_t)(t >> 64);

    /* Phase C */
    t = (__uint128_t)acc[0] + mp0_lo;  c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[1] + red1 + c; acc[0] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[2] + red2 + c; acc[1] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[3] + red3 + c; acc[2] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc4 + red4 + c; acc[3] = (uint64_t)t;
    acc[4] = (uint64_t)(t >> 64) + acc4_hi + red4_hi;
}

static inline void cond_subtract(const uint64_t acc[5], uint64_t *out) {
    uint64_t diff[4];
    __uint128_t b128;
    b128 = (__uint128_t)acc[0] - UINT64_C(0xFFFFFFFEFFFFFC2F);
    diff[0] = (uint64_t)b128;
    b128 = (__uint128_t)acc[1] - UINT64_C(0xFFFFFFFFFFFFFFFF) - ((uint64_t)(b128 >> 64) & 1);
    diff[1] = (uint64_t)b128;
    b128 = (__uint128_t)acc[2] - UINT64_C(0xFFFFFFFFFFFFFFFF) - ((uint64_t)(b128 >> 64) & 1);
    diff[2] = (uint64_t)b128;
    b128 = (__uint128_t)acc[3] - UINT64_C(0xFFFFFFFFFFFFFFFF) - ((uint64_t)(b128 >> 64) & 1);
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
        uint64_t m = acc[0] * SECP256K1_MU;
        c = 0;
        for (int j = 0; j < 4; j++) {
            c += (__uint128_t)m * SECP256K1_P[j] + acc[j];
            acc[j] = (uint64_t)c; c >>= 64;
        }
        word4 += (uint64_t)c;
        acc[0]=acc[1]; acc[1]=acc[2]; acc[2]=acc[3];
        acc[3]=(uint64_t)word4; acc[4]=(uint64_t)(word4>>64);
    }
    cond_subtract(acc, out);
}

/* ── microcode (2-iter-per-vmwrite, 45 triads/iter, 94 total) ── */

/*
 * Arch regs (persist across calls):
 *   R15=acc[0]  R9=acc[1]  R10=acc[2]  R13=acc[3]  RAX=acc[4]
 *   RSI=b[0]    R12=b[1]   R11=b[2]    R14=b[3]
 *   R8=0xFFFFFFFFFFFFFFFF (p[1..3])
 *   RBX=0xFFFFFFFEFFFFFC2F (p[0])
 *   RBP=0xD838091DD2253531 (mu)
 *   RDI=a_i (set by caller before each vmwrite)
 *
 * TMP regs (set at start of each iteration):
 *   TMP10-13 = b[0..3] (reloaded from RSI,R12,R11,R14)
 *   TMP9 = mu (reloaded from RBP)
 *   TMP15 = a[i+1] (from RDX, saved by PREP)
 */

#define MONT_ITER \
    /* ── PHASE A: schoolbook a_i(RDI) x b(TMP10-13) (13 triads) ── */ \
    { ZEROEXT_DSZ64_DR(RDX, RDI), NOP, NOP, NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP10, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP0, RDX), ZEROEXT_DSZ64_DR(TMP1, RCX), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    /* MUL b1*ai + Phase A' w0 merge */ \
    { MUL_DSZ64_DRR(RCX, TMP11, RDX), ADD_DSZ64_DRR(TMP0, R15, TMP0), \
      SETCC_CONDB_DR(TMP3, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP2, TMP1, RDX), SETCC_CONDB_DR(TMP8, TMP2), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    /* [5] writeback w0→R15 + early-start Phase A' w1 */ \
    { ZEROEXT_DSZ64_DR(RDX, RDI), ZEROEXT_DSZ64_DR(R15, TMP0), \
      ADD_DSZ64_DRR(TMP0, R9, TMP2), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP12, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* [2] save hi(b2) in slot 2 (WAR on TMP1 safe) */ \
    { ADD_DSZ64_DRR(TMP4, TMP1, RDX), SETCC_CONDB_DR(TMP5, TMP4), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    /* [2] reload RDX in slot 2 */ \
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP8), SETCC_CONDB_DR(TMP6, TMP4), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    /* [2] MUL b3*ai + combine w2 carries in slot 1 */ \
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
    /* [5] w1 triple-pack (SETCC reads TMP0 flags from Phase A T6, survives T7-T13) */ \
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
    /* [1] w4 combine + Phase B setup in slot 2 */ \
    { ADD_DSZ64_DRR(TMP14, TMP1, TMP8), ZEROEXT_DSZ64_DR(RAX, TMP0), \
      ZEROEXT_DSZ64_DR(RDX, R15), NOP_SEQWORD }, \
    \
    /* ── PHASE B: m = R15*TMP9(mu), m*RBX(p0), m*R8(p1=p2=p3) (12 triads) ── */ \
    { MUL_DSZ64_DRR(RCX, TMP9, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* save m (cross-triad read of RDX) */ \
    { ZEROEXT_DSZ64_DR(TMP6, RDX), NOP, NOP, NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, RBX, RDX), NOP, NOP, NOP_SEQWORD }, \
    /* save m*p0 hi/lo, reload m */ \
    { ZEROEXT_DSZ64_DR(TMP8, RDX), ZEROEXT_DSZ64_DR(TMP3, RCX), \
      ZEROEXT_DSZ64_DR(RDX, TMP6), NOP_SEQWORD }, \
    /* [3] m*R8 + Phase C w0 discard in slots 1-2 (TMP8=carry via WAR) */ \
    { MUL_DSZ64_DRR(RCX, R8, RDX), ADD_DSZ64_DRR(TMP0, R15, TMP8), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* save mR8 hi/lo */ \
    { ZEROEXT_DSZ64_DR(TMP4, RDX), ZEROEXT_DSZ64_DR(TMP5, RCX), \
      NOP, NOP_SEQWORD }, \
    /* [4] red[1] + start red[2] base in slot 2 */ \
    { ADD_DSZ64_DRR(TMP7, TMP3, TMP4), SETCC_CONDB_DR(TMP3, TMP7), \
      ADD_DSZ64_DRR(TMP6, TMP5, TMP4), NOP_SEQWORD }, \
    /* [4] red[2] triple-pack */ \
    { SETCC_CONDB_DR(TMP1, TMP6), ADD_DSZ64_DRR(TMP6, TMP6, TMP3), \
      SETCC_CONDB_DR(TMP2, TMP6), NOP_SEQWORD }, \
    /* combine carry2 + save red[2]->RDI + start red[3] base */ \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP2), ZEROEXT_DSZ64_DR(RDI, TMP6), \
      ADD_DSZ64_DRR(TMP6, TMP5, TMP4), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP1, TMP6), ADD_DSZ64_DRR(TMP6, TMP6, TMP3), \
      SETCC_CONDB_DR(TMP2, TMP6), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP2), ADD_DSZ64_DRR(TMP5, TMP5, TMP3), \
      SETCC_CONDB_DR(TMP4, TMP5), NOP_SEQWORD }, \
    /* After: TMP8=w0_carry, TMP7=red[1], RDI=red[2], TMP6=red[3], TMP5=red[4], TMP4=carry_red4 */ \
    \
    /* ── PHASE C: add red to acc, shift (10 triads) ── */ \
    /* w1 start (w0 carry already in TMP8 from Phase B) */ \
    { ADD_DSZ64_DRR(TMP0, R9, TMP7), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP, NOP_SEQWORD }, \
    /* w1 +cin from w0 carry (TMP8) */ \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP8), SETCC_CONDB_DR(TMP8, TMP0), \
      NOP, NOP_SEQWORD }, \
    /* w1 combine + w2 start */ \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R15, TMP0), \
      ADD_DSZ64_DRR(TMP0, R10, RDI), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R9, TMP0), \
      ADD_DSZ64_DRR(TMP0, R13, TMP6), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R10, TMP0), \
      ADD_DSZ64_DRR(TMP0, RAX, TMP5), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(R13, TMP0), ADD_DSZ64_DRR(TMP0, TMP1, TMP8), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP14), ADD_DSZ64_DRR(RAX, TMP0, TMP4), \
      NOP, NOP_SEQWORD }

static void install_secp256k1_mont_mul_patch(void) {
    ucode_t patch[] = {
    /* PREP: reload b→TMPs, save a[i+1] from RDX→TMP15, load mu→TMP9 */
    { ZEROEXT_DSZ64_DR(TMP10, RSI), ZEROEXT_DSZ64_DR(TMP11, R12),
      ZEROEXT_DSZ64_DR(TMP12, R11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R14), ZEROEXT_DSZ64_DR(TMP15, RDX),
      ZEROEXT_DSZ64_DR(TMP9, RBP), NOP_SEQWORD },
    MONT_ITER,       /* iteration i   (a_i in RDI) */
    { ZEROEXT_DSZ64_DR(RDI, TMP15), NOP, NOP, NOP_SEQWORD },  /* switch */
    MONT_ITER,       /* iteration i+1 (a_{i+1} now in RDI) */
    { NOP, NOP, NOP, END_SEQWORD }
    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("secp256k1_mont_mul: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
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
        "push rbp\n\t"

        /* Load b[0..3] from rbx → arch regs that persist across vmwrites */
        "mov rsi, [rbx]\n\t"
        "mov r12, [rbx + 8]\n\t"
        "mov r11, [rbx + 16]\n\t"
        "mov r14, [rbx + 24]\n\t"

        /* p constants */
        "mov r8, -1\n\t"                          /* p[1..3] = all ones */
        "mov rbx, 0xFFFFFFFEFFFFFC2F\n\t"         /* p[0] (overwrites b pointer) */
        "mov rbp, 0xD838091DD2253531\n\t"         /* mu */

        /* Zero accumulator */
        "xor r15d, r15d\n\t"
        "xor r9d, r9d\n\t"
        "xor r10d, r10d\n\t"
        "xor r13d, r13d\n\t"
        "xor eax, eax\n\t"

        /* Iterations 0-1: a[0]→RDI, a[1]→RDX */
        "mov rcx, [rsp + 8]\n\t"
        "mov rdi, [rcx]\n\t"
        "mov rdx, [rcx + 8]\n\t"
        "vmwrite rcx, rdx\n\t"

        /* Iterations 2-3: a[2]→RDI, a[3]→RDX */
        "mov rcx, [rsp + 8]\n\t"
        "mov rdi, [rcx + 16]\n\t"
        "mov rdx, [rcx + 24]\n\t"
        "vmwrite rcx, rdx\n\t"

        /* Store acc[0..4] */
        "pop rbp\n\t"
        "pop rcx\n\t"
        "pop rcx\n\t"
        "mov [rcx],      r15\n\t"
        "mov [rcx + 8],  r9\n\t"
        "mov [rcx + 16], r10\n\t"
        "mov [rcx + 24], r13\n\t"
        "mov [rcx + 32], rax\n\t"

        : "+r"(_a), "+r"(_b), "+r"(_acc)
        :
        : "rax", "rbp", "rdx", "rsi", "rdi",
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
        int lt = 0;
        for (int j = 3; j >= 0; j--) {
            if (out[j] < SECP256K1_P[j]) { lt = 1; break; }
            if (out[j] > SECP256K1_P[j]) break;
        }
        if (lt) break;
    }
}

static int verify_all(void) {
    int pass = 0, fail = 0;

    printf("--- Known vectors ---\n");
    /* Montgomery form of 1: R mod p = 2^256 mod p = 2^32 + 977 = 0x1000003D1 */
    uint64_t zero[4] = {0};
    uint64_t one_m[4] = {UINT64_C(0x1000003D1), 0, 0, 0};
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
    uint64_t rng = 0xB256CAFE12345678ULL;
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
    printf("=== secp256k1 Montgomery multiply: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_secp256k1_mont_mul_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED, skipping benchmark.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    /* Use 1_mont and a generator-like point for benchmark operands */
    uint64_t sa[4] = {UINT64_C(0x1000003D1), 0, 0, 0};
    uint64_t sb[4] = {UINT64_C(0x59F2815B16F81798), UINT64_C(0x029BFCDB2DCE28D9),
                      UINT64_C(0x55A06295CE870B07), UINT64_C(0x79BE667EF9DCBBAC)};
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
    printf("Native -O3:  min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n", min/BATCH, sum/REPS/BATCH);

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
