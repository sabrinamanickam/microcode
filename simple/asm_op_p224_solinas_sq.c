/*
 * asm_op_p224_solinas_sq.c — P-224 squaring (Solinas, not Montgomery)
 *
 *   p = 2^224 - 2^96 + 1
 *
 * Pipeline:
 *   C:        4×64-bit input  →  4×56-bit unsaturated limbs (avoids 2*a overflow).
 *   Microcode (single vmwrite, 41 triads):
 *               (a) PREP:        copy a into TMP10..13.
 *               (b) Schoolbook:  10 MULs, progressive lo/hi accumulation,
 *                                 produces 8×56-bit product w56[0..7].
 *               (c) Repack:      chain TMP0 carry through SHL/OR/SHR triads,
 *                                 emit 7×64-bit product w[0..6].
 *   C:        solinas_reduce — NIST fold (s1+s2+s3-s4-s5 mod p).
 *
 * Build:  make PROG=asm_op_p224_solinas_sq
 * Run:    sudo taskset -c 0 ./asm_op_p224_solinas_sq_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* fiat-crypto baseline — the GCC -O3 reference CryptOpt is measured against. */
#include "../curvesC/p224_square.c"
static void fe_sq_fiat(const uint64_t *a, uint64_t *out) { fiat_p224_square(out, a); }

static const uint64_t P224_P[4] = {
    UINT64_C(0x0000000000000001), UINT64_C(0xFFFFFFFF00000000),
    UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x00000000FFFFFFFF)
};

/* NIST P-224 Solinas reduction (FIPS 186-4 D.2.1).
 * w[0..6] is a 448-bit product; we view it as 14 × 32-bit halves c[0..13]
 * (c[2k]=lo32(w[k]), c[2k+1]=hi32(w[k])) and sum s1+s2+s3-s4-s5 column-wise:
 *
 *   col[0] = c[0] - c[7] - c[11]
 *   col[1] = c[1] - c[8] - c[12]
 *   col[2] = c[2] - c[9] - c[13]
 *   col[3] = c[3] + c[7] + c[11] - c[10]
 *   col[4] = c[4] + c[8] + c[12] - c[11]
 *   col[5] = c[5] + c[9] + c[13] - c[12]
 *   col[6] = c[6] + c[10] - c[13]
 *
 * Then propagate carries through the 7 columns (signed 64-bit accumulator),
 * fold any 2^224 spill via 2^224 ≡ 2^96 - 1, and finish with a branchless
 * conditional subtract of p. */
static void solinas_reduce(const uint64_t w[7], uint64_t out[4]) {
    uint32_t c[14];
    for (int i = 0; i < 7; i++) {
        c[2*i]   = (uint32_t)w[i];
        c[2*i+1] = (uint32_t)(w[i] >> 32);
    }

    int64_t col[7];
    col[0] = (int64_t)c[0] - (int64_t)c[7] - (int64_t)c[11];
    col[1] = (int64_t)c[1] - (int64_t)c[8] - (int64_t)c[12];
    col[2] = (int64_t)c[2] - (int64_t)c[9] - (int64_t)c[13];
    col[3] = (int64_t)c[3] + (int64_t)c[7] + (int64_t)c[11] - (int64_t)c[10];
    col[4] = (int64_t)c[4] + (int64_t)c[8] + (int64_t)c[12] - (int64_t)c[11];
    col[5] = (int64_t)c[5] + (int64_t)c[9] + (int64_t)c[13] - (int64_t)c[12];
    col[6] = (int64_t)c[6] + (int64_t)c[10] - (int64_t)c[13];

    /* Carry propagation across 32-bit positions packed into 4×64-bit r[]. */
    int64_t carry = 0, r[4];

    carry = col[0];
    r[0]  = carry & 0xFFFFFFFF; carry >>= 32;
    carry += col[1];
    r[0] |= (carry & 0xFFFFFFFF) << 32; carry >>= 32;
    carry += col[2];
    r[1]  = carry & 0xFFFFFFFF; carry >>= 32;
    carry += col[3];
    r[1] |= (carry & 0xFFFFFFFF) << 32; carry >>= 32;
    carry += col[4];
    r[2]  = carry & 0xFFFFFFFF; carry >>= 32;
    carry += col[5];
    r[2] |= (carry & 0xFFFFFFFF) << 32; carry >>= 32;
    carry += col[6];
    r[3]  = carry & 0xFFFFFFFF; carry >>= 32;

    /* Fold 2^224 spill: r += carry*(2^96 - 1) = -carry at pos0 + carry at pos96.
     * Iterates ≤3 times in practice; loop bound caps worst-case. */
    for (int round = 0; round < 3 && carry != 0; round++) {
        __int128_t adj;
        adj = (__int128_t)(uint64_t)r[0] - carry;
        r[0] = (int64_t)(uint64_t)adj;
        adj >>= 64;

        adj += (__int128_t)(uint64_t)r[1] + (carry << 32);
        r[1] = (int64_t)(uint64_t)adj;
        adj >>= 64;

        adj += (uint64_t)r[2];
        r[2] = (int64_t)(uint64_t)adj;
        adj >>= 64;

        adj += (uint64_t)r[3];
        r[3] = (int64_t)(uint64_t)adj;
        carry = (int64_t)(adj >> 32);
        r[3] &= 0xFFFFFFFF;
    }

    /* Negative spill: add p until non-negative. */
    while (carry < 0) {
        __int128_t adj;
        adj = (__int128_t)(uint64_t)r[0] + P224_P[0];
        r[0] = (int64_t)(uint64_t)adj;
        adj >>= 64;
        adj += (__int128_t)(uint64_t)r[1] + P224_P[1];
        r[1] = (int64_t)(uint64_t)adj;
        adj >>= 64;
        adj += (__int128_t)(uint64_t)r[2] + P224_P[2];
        r[2] = (int64_t)(uint64_t)adj;
        adj >>= 64;
        adj += (__int128_t)(uint64_t)r[3] + P224_P[3];
        r[3] = (int64_t)(uint64_t)adj;
        carry = (int64_t)(adj >> 32);
        r[3] &= 0xFFFFFFFF;
    }

    /* Positive spill: subtract p until in range. */
    while (carry > 0) {
        __int128_t adj;
        adj = (__int128_t)(uint64_t)r[0] - P224_P[0];
        r[0] = (int64_t)(uint64_t)adj;
        adj >>= 64;
        adj += (__int128_t)(uint64_t)r[1] - (int64_t)P224_P[1];
        r[1] = (int64_t)(uint64_t)adj;
        adj >>= 64;
        adj += (__int128_t)(uint64_t)r[2] - (int64_t)P224_P[2];
        r[2] = (int64_t)(uint64_t)adj;
        adj >>= 64;
        adj += (__int128_t)(uint64_t)r[3] - (int64_t)P224_P[3];
        r[3] = (int64_t)(uint64_t)adj;
        carry = (int64_t)(adj >> 32);
        r[3] &= 0xFFFFFFFF;
    }

    /* Branchless final reduction: compute r-p, keep r if r<p (borrow=1) else r-p. */
    {
        uint64_t d[4], b;
        __uint128_t borrow;
        borrow = (__uint128_t)(uint64_t)r[0] - P224_P[0];
        d[0] = (uint64_t)borrow; b = (uint64_t)(borrow >> 64) & 1;
        borrow = (__uint128_t)(uint64_t)r[1] - P224_P[1] - b;
        d[1] = (uint64_t)borrow; b = (uint64_t)(borrow >> 64) & 1;
        borrow = (__uint128_t)(uint64_t)r[2] - P224_P[2] - b;
        d[2] = (uint64_t)borrow; b = (uint64_t)(borrow >> 64) & 1;
        borrow = (__uint128_t)(uint64_t)r[3] - P224_P[3] - b;
        d[3] = (uint64_t)borrow; b = (uint64_t)(borrow >> 64) & 1;

        uint64_t mask = (uint64_t)0 - b;
        out[0] = ((uint64_t)r[0] & mask) | (d[0] & ~mask);
        out[1] = ((uint64_t)r[1] & mask) | (d[1] & ~mask);
        out[2] = ((uint64_t)r[2] & mask) | (d[2] & ~mask);
        out[3] = ((uint64_t)r[3] & mask) | (d[3] & ~mask);
    }
}

/* Native C Solinas squaring — kept as oracle for verification and as a sanity
 * comparison row in the benchmark. SAFE_ADD tracks __uint128_t overflow via
 * post-add comparison, since column 3 has 4 products and can overflow 128 bits. */
static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
    uint64_t w[7];
    __uint128_t acc = 0, tmp;
    uint64_t extra;
#define SAFE_ADD(prod) do { tmp=acc; acc+=(prod); if(acc<tmp) extra++; } while(0)

    acc = (__uint128_t)a0 * a0;
    w[0] = (uint64_t)acc; acc >>= 64;

    extra = 0;
    SAFE_ADD((__uint128_t)a0 * a1); SAFE_ADD((__uint128_t)a1 * a0);
    w[1] = (uint64_t)acc; acc = (acc >> 64) | ((__uint128_t)extra << 64);

    extra = 0;
    SAFE_ADD((__uint128_t)a0 * a2); SAFE_ADD((__uint128_t)a1 * a1);
    SAFE_ADD((__uint128_t)a2 * a0);
    w[2] = (uint64_t)acc; acc = (acc >> 64) | ((__uint128_t)extra << 64);

    extra = 0;
    SAFE_ADD((__uint128_t)a0 * a3); SAFE_ADD((__uint128_t)a1 * a2);
    SAFE_ADD((__uint128_t)a2 * a1); SAFE_ADD((__uint128_t)a3 * a0);
    w[3] = (uint64_t)acc; acc = (acc >> 64) | ((__uint128_t)extra << 64);

    extra = 0;
    SAFE_ADD((__uint128_t)a1 * a3); SAFE_ADD((__uint128_t)a2 * a2);
    SAFE_ADD((__uint128_t)a3 * a1);
    w[4] = (uint64_t)acc; acc = (acc >> 64) | ((__uint128_t)extra << 64);

    extra = 0;
    SAFE_ADD((__uint128_t)a2 * a3); SAFE_ADD((__uint128_t)a3 * a2);
    w[5] = (uint64_t)acc; acc = (acc >> 64) | ((__uint128_t)extra << 64);

    acc += (__uint128_t)a3 * a3;
    w[6] = (uint64_t)acc;

#undef SAFE_ADD
    solinas_reduce(w, out);
}

/* Single-vmwrite patch: PREP (2 triads) + 10-MUL schoolbook (32 triads) +
 * 56→64 repack (7 triads) = 41 triads.
 *
 *   srcA  (preserved by MUL): RDI=a0, RSI=a1, R12=a2, R11=a3.
 *   srcB  (from PREP):         TMP10..TMP13 = a0..a3.
 *   Accum: TMP0=lo, TMP4=hi, TMP8=lo-carry, TMP1=hi-carry,
 *          TMP9=mask scratch, TMP15=SETCC capture.
 *   After schoolbook: R15..R14 hold 8×56-bit limbs w56[0..7].
 *   After repack:     R15..RAX hold 7×64-bit limbs w[0..6]; R14 unused.
 *   R8 is never written by the patch — caller uses it to carry the output ptr. */
static void install_p224_solinas_sq_patch(void) {
    ucode_t patch[] = {

    /* PREP: copy a0..a3 into TMP10..TMP13 and seed RDX=a0 for the first MUL.
     * NOTAND clears TMP0 (the lo accumulator). */
    { ZEROEXT_DSZ64_DR(TMP10, RDI),
      ZEROEXT_DSZ64_DR(TMP11, RSI),
      ZEROEXT_DSZ64_DR(TMP12, R12), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R11),
      NOTAND_DSZ64_DRR(TMP0, TMP0, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RDI),   NOP_SEQWORD },

    /* w56[0] = a0² */
    { MUL_DSZ64_DRR(RCX, RDI, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 56),
      SHL_DSZ64_DRI(TMP9, TMP0, 8), NOP_SEQWORD },
    { SHR_DSZ64_DRI(R15, TMP9, 8),
      SHL_DSZ64_DRI(TMP1, TMP4, 8),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },

    /* w56[1] = 2·a0·a1 */
    { ADD_DSZ64_DRR(RDX, TMP11, TMP11),
      MUL_DSZ64_DRR(RCX, RDI, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(TMP4, RCX, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 56), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP0, 8),
      SHR_DSZ64_DRI(R13, TMP9, 8),
      SHL_DSZ64_DRI(TMP1, TMP4, 8), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      ADD_DSZ64_DRR(RDX, TMP12, TMP12),
      NOP, NOP_SEQWORD },

    /* w56[2] = 2·a0·a2 + a1² */
    { MUL_DSZ64_DRR(RCX, RDI, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP15),
      ZEROEXT_DSZ64_DR(RDX, TMP11),
      NOP, NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RSI, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, TMP4, RCX),
      ADD_DSZ64_DRR(TMP4, TMP4, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 56), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP0, 8),
      SHR_DSZ64_DRI(R9, TMP9, 8),
      SHL_DSZ64_DRI(TMP1, TMP4, 8), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      ADD_DSZ64_DRR(RDX, TMP13, TMP13),
      NOP, NOP_SEQWORD },

    /* w56[3] = 2·a0·a3 + 2·a1·a2 */
    { MUL_DSZ64_DRR(RCX, RDI, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP15),
      ADD_DSZ64_DRR(RDX, TMP12, TMP12),
      NOP, NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RSI, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, TMP4, RCX),
      ADD_DSZ64_DRR(TMP4, TMP4, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 56), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP0, 8),
      SHR_DSZ64_DRI(R10, TMP9, 8),
      SHL_DSZ64_DRI(TMP1, TMP4, 8), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      ADD_DSZ64_DRR(RDX, TMP13, TMP13),
      NOP, NOP_SEQWORD },

    /* w56[4] = 2·a1·a3 + a2² */
    { MUL_DSZ64_DRR(RCX, RSI, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP15),
      ZEROEXT_DSZ64_DR(RDX, TMP12),
      NOP, NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, TMP4, RCX),
      ADD_DSZ64_DRR(TMP4, TMP4, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 56), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP0, 8),
      SHR_DSZ64_DRI(RBX, TMP9, 8),
      SHL_DSZ64_DRI(TMP1, TMP4, 8), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      ADD_DSZ64_DRR(RDX, TMP13, TMP13),
      NOP, NOP_SEQWORD },

    /* w56[5] = 2·a2·a3 */
    { MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 56),
      SHL_DSZ64_DRI(TMP9, TMP0, 8), NOP_SEQWORD },
    { SHR_DSZ64_DRI(RBP, TMP9, 8),
      SHL_DSZ64_DRI(TMP1, TMP4, 8),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },

    /* w56[6] = a3²; final triad emits w56[7] (the leftover hi carry). */
    { ZEROEXT_DSZ64_DR(RDX, TMP13),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(TMP4, RCX, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 56), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP0, 8),
      SHR_DSZ64_DRI(RAX, TMP9, 8),
      SHL_DSZ64_DRI(TMP1, TMP4, 8), NOP_SEQWORD },
    { OR_DSZ64_DRR(R14, TMP8, TMP1),
      NOP, NOP, NOP_SEQWORD },

    /* 56→64 repack: each triad consumes one source w56 limb and emits one
     * 64-bit limb. TMP0 chains the slack carry across triads (top bits of the
     * current source minus the bits already placed); TMP1 is SHL scratch. */
    { SHR_DSZ64_DRI(TMP0, R13, 8),                 /* w[0] */
      SHL_DSZ64_DRI(TMP1, R13, 56),
      OR_DSZ64_DRR (R15, R15, TMP1), NOP_SEQWORD },

    { SHL_DSZ64_DRI(TMP1, R9, 48),                 /* w[1] */
      OR_DSZ64_DRR (R13, TMP0, TMP1),
      SHR_DSZ64_DRI(TMP0, R9, 16),  NOP_SEQWORD },

    { SHL_DSZ64_DRI(TMP1, R10, 40),                /* w[2] */
      OR_DSZ64_DRR (R9,  TMP0, TMP1),
      SHR_DSZ64_DRI(TMP0, R10, 24), NOP_SEQWORD },

    { SHL_DSZ64_DRI(TMP1, RBX, 32),                /* w[3] */
      OR_DSZ64_DRR (R10, TMP0, TMP1),
      SHR_DSZ64_DRI(TMP0, RBX, 32), NOP_SEQWORD },

    { SHL_DSZ64_DRI(TMP1, RBP, 24),                /* w[4] */
      OR_DSZ64_DRR (RBX, TMP0, TMP1),
      SHR_DSZ64_DRI(TMP0, RBP, 40), NOP_SEQWORD },

    { SHL_DSZ64_DRI(TMP1, RAX, 16),                /* w[5] */
      OR_DSZ64_DRR (RBP, TMP0, TMP1),
      SHR_DSZ64_DRI(TMP0, RAX, 48), NOP_SEQWORD },

    { SHL_DSZ64_DRI(TMP1, R14, 8),                 /* w[6] (last) */
      OR_DSZ64_DRR (RAX, TMP0, TMP1),
      NOP,                          END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("p224_solinas_sq: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* fe_sq via microcode: pack a→56-bit, vmwrite (schoolbook + repack), then reduce. */
static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    /* 4×64 → 4×56-bit. a[3] < 2^32 keeps a56[3] within 56 bits. */
    uint64_t a56[4];
    a56[0] = a[0] & 0xFFFFFFFFFFFFFFULL;
    a56[1] = (a[0] >> 56) | ((a[1] << 8)  & 0xFFFFFFFFFFFFFFULL);
    a56[2] = (a[1] >> 48) | ((a[2] << 16) & 0xFFFFFFFFFFFFFFULL);
    a56[3] = (a[2] >> 40) |  (a[3] << 24);

    /* The patch needs srcA in RDI/RSI/R12/R11. Output ptr rides in R8 (the
     * patch never writes R8). RBP is callee-saved and the patch clobbers it,
     * so we push/pop it manually. */
    uint64_t w[7];
    {
        register uint64_t *_a56 asm("rcx") = a56;
        register uint64_t *_w   asm("r8")  = w;

        asm volatile(
            "push rbp\n\t"
            "mov rdi, [rcx]\n\t"
            "mov rsi, [rcx + 8]\n\t"
            "mov r12, [rcx + 16]\n\t"
            "mov r11, [rcx + 24]\n\t"
            "vmwrite rcx, rdx\n\t"
            /* Patch outputs: R15..RAX = w[0..6]. */
            "mov [r8],      r15\n\t"
            "mov [r8 + 8],  r13\n\t"
            "mov [r8 + 16], r9\n\t"
            "mov [r8 + 24], r10\n\t"
            "mov [r8 + 32], rbx\n\t"
            "mov [r8 + 40], rbp\n\t"
            "mov [r8 + 48], rax\n\t"
            "pop rbp\n\t"
            : "+r"(_a56), "+r"(_w)
            :
            : "rax", "rbx", "rdx", "rsi", "rdi", "rbp",
              "r9", "r10", "r11", "r12", "r13", "r14", "r15",
              "memory", "cc"
        );
    }

    solinas_reduce(w, out);
}

/* ── verification (native is the oracle) ─────────────────────── */

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void rand_mod_p(uint64_t out[4], uint64_t *rng) {
    for (;;) {
        for (int j = 0; j < 4; j++) out[j] = splitmix64(rng);
        out[3] &= UINT64_C(0xFFFFFFFF);
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

    struct { const char *name; uint64_t a[4]; uint64_t expected[4]; int has_exp; } vecs[] = {
        { "0^2",     {0,0,0,0}, {0,0,0,0}, 1 },
        { "1^2",     {1,0,0,0}, {1,0,0,0}, 1 },
        { "2^2",     {2,0,0,0}, {4,0,0,0}, 1 },
        { "3^2",     {3,0,0,0}, {9,0,0,0}, 1 },
        { "(p-1)^2", {0, UINT64_C(0xFFFFFFFF00000000),
                      UINT64_C(0xFFFFFFFFFFFFFFFF),
                      UINT64_C(0x00000000FFFFFFFF)}, {1,0,0,0}, 1 },
        { "2^32",    {UINT64_C(0x100000000), 0, 0, 0}, {0, 1, 0, 0}, 1 },
        { "2^96",    {0, UINT64_C(0x100000000), 0, 0}, {0, 0, 0, 1}, 1 },
        { "max32",   {UINT64_C(0xFFFFFFFF), 0, 0, 0}, {0}, 0 },
        { "large",   {UINT64_C(0xFEDCBA9876543210), UINT64_C(0x1234567890ABCDEF),
                      UINT64_C(0xAAAABBBBCCCCDDDD), UINT64_C(0x12345678)}, {0}, 0 },
    };
    int nvecs = sizeof(vecs)/sizeof(vecs[0]);

    printf("--- Known vectors ---\n");
    for (int i = 0; i < nvecs; i++) {
        uint64_t nat[4], ucd[4];
        fe_sq_native(vecs[i].a, nat);
        fe_sq_ucode (vecs[i].a, ucd);
        int ok = !memcmp(nat, ucd, 32);
        if (vecs[i].has_exp) ok &= !memcmp(nat, vecs[i].expected, 32);
        if (ok) { printf("  PASS [%s]\n", vecs[i].name); pass++; }
        else {
            printf("  FAIL [%s]\n", vecs[i].name);
            for (int j = 0; j < 4; j++)
                printf("    [%d] nat=%016"PRIx64" ucd=%016"PRIx64"%s\n",
                    j, nat[j], ucd[j], nat[j]!=ucd[j]?" ***":"");
            fail++;
        }
    }

    printf("\n--- Random (10000) ---\n");
    uint64_t rng = 0xA224CAFE12345678ULL;
    int rp = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t a[4], nat[4], ucd[4];
        rand_mod_p(a, &rng);
        fe_sq_native(a, nat);
        fe_sq_ucode (a, ucd);
        if (!memcmp(nat, ucd, 32)) rp++;
        else {
            printf("  FAIL #%d  a={%016"PRIx64",%016"PRIx64",%016"PRIx64",%016"PRIx64"}\n",
                   i, a[0], a[1], a[2], a[3]);
            for (int j = 0; j < 4; j++)
                printf("    [%d] nat=%016"PRIx64" ucd=%016"PRIx64"%s\n",
                    j, nat[j], ucd[j], nat[j]!=ucd[j]?" ***":"");
            break;
        }
    }
    printf("  %d / 10000 PASS\n", rp);
    pass += rp; if (rp < 10000) fail += (10000 - rp);

    printf("\n--- Chain (1000 iterated sq from 3) ---\n");
    uint64_t ni[4] = {3,0,0,0}, ui[4] = {3,0,0,0};
    for (int i = 0; i < 1000; i++) { fe_sq_native(ni, ni); fe_sq_ucode(ui, ui); }
    int rok = !memcmp(ni, ui, 32);
    printf("  -> %s\n", rok ? "PASS" : "FAIL");
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

/* BENCH is a macro (not a function) so the inner loop's `fn(tmp,tmp)` stays a
 * direct call that GCC -O3 fully inlines. A function-pointer parameter would
 * break inlining and cost ~5-10 cycles/op for indirect-call overhead. */
#define BENCH(label, fn) do {                                                  \
    uint64_t tmp[4], _min = UINT64_MAX, _sum = 0;                              \
    for (int _r = 0; _r < REPS; _r++) {                                        \
        memcpy(tmp, state, 32);                                                \
        uint64_t _t0 = rdtsc_start();                                          \
        for (int _i = 0; _i < BATCH; _i++) fn(tmp, tmp);                       \
        uint64_t _t1 = rdtsc_end();                                            \
        uint64_t _dt = _t1 - _t0; _sum += _dt; if (_dt < _min) _min = _dt;     \
    }                                                                          \
    printf("%-18s min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",              \
           label, _min/BATCH, _sum/REPS/BATCH);                                \
} while (0)

int main(void) {
    printf("=== P-224 squaring: fiat-crypto vs microcode ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_p224_solinas_sq_patch();

    if (verify_all()) {
        printf("Verification FAILED, skipping benchmark.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    uint64_t state[4] = {3, 0, 0, 0};
    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    /* Headline: fiat-crypto is the GCC baseline CryptOpt is measured against. */
    BENCH("Fiat-crypto",       fe_sq_fiat);
    BENCH("Microcode",         fe_sq_ucode);
    /* Sanity row: hand-written native Solinas (not the optimization target). */
    BENCH("Native (Solinas)",  fe_sq_native);

    init_match_and_patch(); do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
