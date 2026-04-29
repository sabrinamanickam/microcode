/*
 * asm_op_p224_sq.c — Montgomery squaring for P-224 via microcode
 *
 * 2-iter-per-vmwrite patch (34 triads/iter, 71 total).
 * P-224: p = 2^224 - 2^96 + 1
 *   p[0] = 0x0000000000000001
 *   p[1] = 0xFFFFFFFF00000000
 *   p[2] = 0xFFFFFFFFFFFFFFFF
 *   p[3] = 0x00000000FFFFFFFF
 *   mu   = 0xFFFFFFFFFFFFFFFF  (= -1 mod 2^64)
 *
 * Build:  make PROG=asm_op_p224_sq
 * Run:    sudo taskset -c 0 ./asm_op_p224_sq_static
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
#include "../curvesC/p224_square.c"

static void fe_sq_fiat(const uint64_t *a, uint64_t *out) {
    fiat_p224_square(out, a);
}

/* ── fe_sq native C ──────────────────────────────────────────── */

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

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    uint64_t acc[5] = {0};
    mont_iteration(acc, a[0], a);
    mont_iteration(acc, a[1], a);
    mont_iteration(acc, a[2], a);
    mont_iteration(acc, a[3], a);
    cond_subtract(acc, out);
}

/* ── reference ───────────────────────────────────────────────── */

static void fe_sq_reference(const uint64_t *a, uint64_t *out) {
    uint64_t acc[5] = {0};
    for (int i = 0; i < 4; i++) {
        __uint128_t c = 0;
        for (int j = 0; j < 4; j++) {
            c += (__uint128_t)a[i] * a[j] + acc[j];
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

/* ── microcode (2-iter-per-vmwrite, 34 triads/iter, 71 total) ── */

/*
 * Arch regs (persist across calls):
 *   R15=acc[0]  R9=acc[1]  R10=acc[2]  R13=acc[3]  RAX=acc[4]
 *   RSI=b[0]    R12=b[1]   R11=b[2]    R14=b[3]  (used directly as MUL srcA)
 *   R8=0xFFFFFFFFFFFFFFFF  (mu AND p[2] — same value!)
 *   RBX=0xFFFFFFFF00000000 (p[1])
 *   RDI=a_i (set by caller before each vmwrite)
 *
 * TMP regs:
 *   TMP9 = p[3] = 0xFFFFFFFF (SHR in PREP, persists — MUL preserves srcA)
 *   TMP15 = a[i+1] (from RDX, saved by PREP)
 *
 * Optimizations vs 76-triad version:
 *   - Arch regs as MUL srcA: eliminates PREP b→TMP copies (saves 1 PREP triad)
 *   - T1 (RDX=RDI, SHR) merged into PREP/switch (saves 1 triad/iter)
 *   - Phase B: PhC w0 in B1, mp1/mp2/mp3_lo saved via MUL srcB RAW,
 *     B8+B9 merged (saves 1 triad/iter in Phase B)
 *   - C8: acc[4] = carry + TMP14 merged via slot 1→2 RAW
 *   Total: 76 → 71 triads
 */

#define MONT_ITER \
    /* ── PHASE A: schoolbook (10 triads) ── */ \
    /* [Merge 1] MUL b0 + save both outputs via srcB/hi RAW */ \
    { MUL_DSZ64_DRR(RCX, RSI, RDX), ZEROEXT_DSZ64_DR(TMP0, RDX), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    /* reload + MUL b1 (slot 0→1) + Phase A' w0 merge */ \
    { ZEROEXT_DSZ64_DR(RDX, RDI), MUL_DSZ64_DRR(RCX, R12, RDX), \
      ADD_DSZ64_DRR(TMP0, R15, TMP0), NOP_SEQWORD }, \
    /* w0 carry + w1 base + w1 carry */ \
    { SETCC_CONDB_DR(TMP3, TMP0), ADD_DSZ64_DRR(TMP2, TMP1, RDX), \
      SETCC_CONDB_DR(TMP8, TMP2), NOP_SEQWORD }, \
    /* save hi(b1) + reload + writeback w0 */ \
    { ZEROEXT_DSZ64_DR(TMP1, RCX), ZEROEXT_DSZ64_DR(RDX, RDI), \
      ZEROEXT_DSZ64_DR(R15, TMP0), NOP_SEQWORD }, \
    /* early w1 start + MUL b2 */ \
    { ADD_DSZ64_DRR(TMP0, R9, TMP2), MUL_DSZ64_DRR(RCX, R11, RDX), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP4, TMP1, RDX), SETCC_CONDB_DR(TMP5, TMP4), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP8), SETCC_CONDB_DR(TMP6, TMP4), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, R14, RDX), ADD_DSZ64_DRR(TMP8, TMP5, TMP6), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP5, TMP1, RDX), SETCC_CONDB_DR(TMP6, TMP5), \
      ADD_DSZ64_DRR(TMP5, TMP5, TMP8), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP7, TMP5), ADD_DSZ64_DRR(TMP8, TMP6, TMP7), \
      ADD_DSZ64_DRR(TMP6, RCX, TMP8), NOP_SEQWORD }, \
    \
    /* ── PHASE A': accumulate (8 triads) ── */ \
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
      ZEROEXT_DSZ64_DR(RDX, R15), NOP_SEQWORD }, \
    \
    /* ── PHASE B: reduction (8 triads) ── */ \
    /* mu*acc[0]=m + Phase C w0 start (acc[0]+m via srcB RAW) */ \
    { MUL_DSZ64_DRR(RCX, R8, RDX), ADD_DSZ64_DRR(TMP0, R15, RDX), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* save m + MUL p1 + save mp1_lo via slot 1→2 srcB RAW */ \
    { ZEROEXT_DSZ64_DR(TMP6, RDX), MUL_DSZ64_DRR(RCX, RBX, RDX), \
      ZEROEXT_DSZ64_DR(TMP7, RDX), NOP_SEQWORD }, \
    /* save mp1_hi + reload m */ \
    { ZEROEXT_DSZ64_DR(TMP3, RCX), ZEROEXT_DSZ64_DR(RDX, TMP6), \
      NOP, NOP_SEQWORD }, \
    /* MUL p2 + save mp2_lo/hi via srcB/hi RAW */ \
    { MUL_DSZ64_DRR(RCX, R8, RDX), ZEROEXT_DSZ64_DR(TMP4, RDX), \
      ZEROEXT_DSZ64_DR(TMP5, RCX), NOP_SEQWORD }, \
    /* reload m + MUL p3 + save mp3_lo via slot 1→2 srcB RAW */ \
    { ZEROEXT_DSZ64_DR(RDX, TMP6), MUL_DSZ64_DRR(RCX, TMP9, RDX), \
      ZEROEXT_DSZ64_DR(TMP6, RDX), NOP_SEQWORD }, \
    /* save mp3_hi + red[2] = mp1_hi + mp2_lo */ \
    { ZEROEXT_DSZ64_DR(TMP2, RCX), ADD_DSZ64_DRR(TMP3, TMP3, TMP4), \
      SETCC_CONDB_DR(TMP1, TMP3), NOP_SEQWORD }, \
    /* red[3]: carry-first (mp2_hi+carry can't overflow since p2=all-ones) */ \
    { ADD_DSZ64_DRR(TMP5, TMP5, TMP1), ADD_DSZ64_DRR(TMP5, TMP5, TMP6), \
      SETCC_CONDB_DR(TMP1, TMP5), NOP_SEQWORD }, \
    /* red[4] + Phase C w1 start (merged) */ \
    { ADD_DSZ64_DRR(TMP2, TMP2, TMP1), ADD_DSZ64_DRR(TMP0, R9, TMP7), \
      NOP, NOP_SEQWORD }, \
    \
    /* ── PHASE C: add red + shift (8 triads, acc[4] merged into C8) ── */ \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP8), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP6, TMP1, TMP8), ZEROEXT_DSZ64_DR(R15, TMP0), \
      ADD_DSZ64_DRR(TMP0, R10, TMP3), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP6), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP6, TMP1, TMP8), ZEROEXT_DSZ64_DR(R9, TMP0), \
      ADD_DSZ64_DRR(TMP0, R13, TMP5), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP6), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP6, TMP1, TMP8), ZEROEXT_DSZ64_DR(R10, TMP0), \
      ADD_DSZ64_DRR(TMP0, RAX, TMP2), NOP_SEQWORD }, \
    { SETCC_CONDB_DR(TMP1, TMP0), ADD_DSZ64_DRR(TMP0, TMP0, TMP6), \
      SETCC_CONDB_DR(TMP8, TMP0), NOP_SEQWORD }, \
    /* C8: writeback acc[3] + compute carry + acc[4] via slot 1→2 RAW */ \
    { ZEROEXT_DSZ64_DR(R13, TMP0), ADD_DSZ64_DRR(TMP0, TMP1, TMP8), \
      ADD_DSZ64_DRR(RAX, TMP0, TMP14), NOP_SEQWORD }

static void install_p224_sq_patch(void) {
    ucode_t patch[] = {
    /* PREP: save a[i+1], load a_i→RDX, compute p[3]→TMP9 */
    { ZEROEXT_DSZ64_DR(TMP15, RDX), ZEROEXT_DSZ64_DR(RDX, RDI),
      SHR_DSZ64_DRI(TMP9, R8, 32), NOP_SEQWORD },
    MONT_ITER,       /* iteration i   (a_i already in RDI+RDX) */
    /* Switch: load a[i+1]→RDI, then RDI→RDX via slot 0→1 RAW */
    { ZEROEXT_DSZ64_DR(RDI, TMP15), ZEROEXT_DSZ64_DR(RDX, RDI),
      NOP, NOP_SEQWORD },
    MONT_ITER,       /* iteration i+1 */
    /* END */
    { NOP, NOP, NOP, END_SEQWORD }
    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("p224_sq: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ── fe_sq via microcode ─────────────────────────────────────── */

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    uint64_t acc[5];
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_acc asm("r15") = acc;

    asm volatile(
        "push r15\n\t"
        "push rcx\n\t"

        /* b = a for squaring -> arch regs that persist across vmwrites */
        "mov rsi, [rcx]\n\t"       /* b[0] = a[0] */
        "mov r12, [rcx + 8]\n\t"   /* b[1] = a[1] */
        "mov r11, [rcx + 16]\n\t"  /* b[2] = a[2] */
        "mov r14, [rcx + 24]\n\t"  /* b[3] = a[3] */

        /* p constants — NO RBP needed! */
        "mov r8, -1\n\t"                          /* mu AND p[2] = all ones */
        "mov rbx, 0xFFFFFFFF00000000\n\t"          /* p[1] */
        /* p[3] = 0xFFFFFFFF computed on-the-fly as SHR(R8,32) in TMP9 */

        /* Zero accumulator */
        "xor r15d, r15d\n\t"       /* acc[0] */
        "xor r9d, r9d\n\t"         /* acc[1] */
        "xor r10d, r10d\n\t"       /* acc[2] */
        "xor r13d, r13d\n\t"       /* acc[3] */
        "xor eax, eax\n\t"         /* acc[4] */

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

        /* Store results */
        "pop rcx\n\t"
        "pop rcx\n\t"
        "mov [rcx],      r15\n\t"
        "mov [rcx + 8],  r9\n\t"
        "mov [rcx + 16], r10\n\t"
        "mov [rcx + 24], r13\n\t"
        "mov [rcx + 32], rax\n\t"

        : "+r"(_a), "+r"(_acc)
        :
        : "rax", "rbx", "rdx", "rsi", "rdi",
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
    struct { const char *name; uint64_t a[4]; } vecs[] = {
        { "0^2", {0,0,0,0} },
        { "1_mont^2", {UINT64_C(0xFFFFFFFF00000000), UINT64_C(0xFFFFFFFFFFFFFFFF), 0, 0} },
        { "small^2", {3, 0, 0, 0} },
    };
    for (int i = 0; i < 3; i++) {
        uint64_t ref[4], nat[4], ucd[4];
        fe_sq_reference(vecs[i].a, ref);
        fe_sq_native(vecs[i].a, nat);
        fe_sq_ucode(vecs[i].a, ucd);
        int ok = !memcmp(ref, nat, 32) && !memcmp(ref, ucd, 32);
        if (!ok) {
            printf("  FAIL [%s]\n", vecs[i].name);
            for (int j=0;j<4;j++)
                printf("    [%d] ref=%016"PRIx64" nat=%016"PRIx64" ucd=%016"PRIx64"%s%s\n",
                    j, ref[j], nat[j], ucd[j],
                    ref[j]!=nat[j]?" nat***":"", ref[j]!=ucd[j]?" ucd***":"");
        } else printf("  PASS [%s]\n", vecs[i].name);
        if (ok) pass++; else fail++;
    }

    printf("\n--- Random (10000) ---\n");
    uint64_t rng = 0xA224CAFE12345678ULL;
    int rp = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t a[4], ref[4], nat[4], ucd[4];
        rand_mod_p(a, &rng);
        fe_sq_reference(a, ref); fe_sq_native(a, nat); fe_sq_ucode(a, ucd);
        if (!memcmp(ref, nat, 32) && !memcmp(ref, ucd, 32)) rp++;
        else {
            printf("  FAIL #%d  a={%016lx,%016lx,%016lx,%016lx}\n",
                   i, a[0], a[1], a[2], a[3]);
            for (int j=0;j<4;j++)
                printf("    [%d] ref=%016lx nat=%016lx ucd=%016lx%s%s\n",
                    j, ref[j], nat[j], ucd[j],
                    ref[j]!=nat[j]?" nat***":"", ref[j]!=ucd[j]?" ucd***":"");
            break;
        }
    }
    printf("  %d / 10000 PASS\n", rp);
    pass += rp; if (rp < 10000) fail += (10000 - rp);

    printf("\n--- Chain (1000 iterated sq) ---\n");
    uint64_t ri[4]={UINT64_C(0xFFFFFFFF00000000), UINT64_C(0xFFFFFFFFFFFFFFFF), 0, 0};
    uint64_t ni[4], ui[4];
    memcpy(ni, ri, 32); memcpy(ui, ri, 32);
    for (int i = 0; i < 1000; i++) {
        fe_sq_reference(ri, ri); fe_sq_native(ni, ni); fe_sq_ucode(ui, ui);
    }
    int rok = !memcmp(ri,ni,32) && !memcmp(ri,ui,32);
    printf("  %s\n", rok?"PASS":"FAIL");
    if (rok) pass++; else fail++;

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
    printf("=== P-224 Montgomery squaring: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_p224_sq_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED, skipping benchmark.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    uint64_t state[4] = {UINT64_C(0xFFFFFFFF00000000), UINT64_C(0xFFFFFFFFFFFFFFFF), 0, 0};
    uint64_t tmp[4], t0, t1, min, sum;

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_native(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Naive -O3:   min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n", min/BATCH, sum/REPS/BATCH);

    /* fiat-crypto (the real GCC baseline CryptOpt compares against) */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_fiat(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Fiat-crypto: min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n", min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_ucode(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode:   min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n", min/BATCH, sum/REPS/BATCH);

    init_match_and_patch(); do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
