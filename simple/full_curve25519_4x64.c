/*
 * full_curve25519_4x64.c — X25519 using 4×64 saturated microcode (v3 chained-ADC).
 *
 * Standalone benchmark of the 4×64 microcode approach end-to-end. fe_mul is
 * the v3 chained-ADC patch (75 triads, 200 cyc/op). fe_sq is fe_mul(a, a) —
 * no squaring symmetry (would need a separate patch we can't fit alongside
 * v3 under the 128-triad RAM cap).
 *
 * Cheap ops (fe_add, fe_sub, fe_mul121665) are native C in 4×64 saturated
 * form mirroring SUPERCOP amd64-64's lazy-reduction pattern.
 *
 * Field element: uint64_t fe4[4] (saturated, value < 2^256).
 *
 * Build: make PROG=full_curve25519_4x64
 * Run:   sudo taskset -c 0 ./full_curve25519_4x64_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

typedef uint64_t fe4[4];

/* p = 2^255 - 19 */
static const uint64_t CURVE25519_P[4] = {
    UINT64_C(0xFFFFFFFFFFFFFFED), UINT64_C(0xFFFFFFFFFFFFFFFF),
    UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x7FFFFFFFFFFFFFFF)
};

/* ════════════════════════════════════════════════════════════════════
 * MICROCODE PATCH (v3 chained-ADC fe_mul)
 * ════════════════════════════════════════════════════════════════════ */

#define SCHOOLBOOK_ROW_START(a_src) \
    { ZEROEXT_DSZ64_DR(RDX, a_src), ZEROEXT_DSZ64_DR(RDI, a_src), \
      NOP, NOP_SEQWORD }

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

static void install_mul_patch(void) {
    ucode_t patch[] = {
    { ZEROEXT_DSZ64_DR(TMP10, RSI), ZEROEXT_DSZ64_DR(TMP11, R12),
      ZEROEXT_DSZ64_DR(TMP12, R11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R14), ZEROEXT_DSZ64_DR(R14, RDX),
      ZEROEXT_DSZ64_DR(TMP15, RBP), NOP_SEQWORD },
    { ZEROEXT_DSZ32_DI(TMP9, 0), ZEROEXT_DSZ32_DI(TMP14, 0),
      NOP, NOP_SEQWORD },

    SCHOOLBOOK_ROW_START(RDI), MUL_BLOCK, COMBINED_CHAIN, SHIFT_WRITEBACK_MERGED(RSI),
    SCHOOLBOOK_ROW_START(R14), MUL_BLOCK, COMBINED_CHAIN, SHIFT_WRITEBACK_MERGED(R12),
    SCHOOLBOOK_ROW_START(TMP15), MUL_BLOCK, COMBINED_CHAIN, SHIFT_WRITEBACK_MERGED(R11),
    SCHOOLBOOK_ROW_START(RBX), MUL_BLOCK, COMBINED_CHAIN, ROW3_WRITEBACK,

    { ADD_DSZ64_DRR(RAX, RAX, TMP14), MUL_DSZ64_DIR(RCX, 38, R9), NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP0, RCX), MUL_DSZ64_DIR(RCX, 38, R10), NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP1, RCX), MUL_DSZ64_DIR(RCX, 38, R13), NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP2, RCX), MUL_DSZ64_DIR(RCX, 38, RAX), NOP, NOP_SEQWORD },

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

    { MUL_DSZ64_DIR(RCX, 38, TMP7), NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP7), GENARITHFLAGS_RR(TMP3, TMP3),
      ADC_DSZ64_DRR(TMP4, TMP4, TMP9), NOP_SEQWORD },
    { GENARITHFLAGS_RR(TMP4, TMP4), ADC_DSZ64_DRR(TMP5, TMP5, TMP9),
      GENARITHFLAGS_RR(TMP5, TMP5), NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP6, TMP6, TMP9), NOP, NOP, NOP_SEQWORD },

    { ZEROEXT_DSZ64_DR(R15, TMP3), ZEROEXT_DSZ64_DR(R9, TMP4),
      ZEROEXT_DSZ64_DR(R10, TMP5), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R13, TMP6), NOP, NOP, END_SEQWORD }
    };
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("4x64 mul patch: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ════════════════════════════════════════════════════════════════════
 * 4×64 FIELD ARITHMETIC
 * ════════════════════════════════════════════════════════════════════ */

/* fe_mul via microcode. Output unreduced relative to p (< 2^256). */
static void fe_mul(fe4 out, const fe4 a, const fe4 b) {
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
    out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = r[3];
}

static inline void fe_sq(fe4 out, const fe4 a) {
    fe_mul(out, a, a);
}

/* fe_add — mirrors SUPERCOP amd64-64 ladderstep.S's pattern.
 *   t = a + b (4-limb chain, gets CF)
 *   addt1 = (CF ? 38 : 0)
 *   t += addt1 with 4-limb carry chain (gets new CF)
 *   addt0 = (CF ? addt1 : 0)
 *   t[0] += addt0
 * Output: t < 2^256, value congruent to a+b mod p.
 */
static inline void fe_add(fe4 out, const fe4 a, const fe4 b) {
    __uint128_t s;
    uint64_t r0, r1, r2, r3, c;
    s = (__uint128_t)a[0] + b[0];        r0 = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (__uint128_t)a[1] + b[1] + c;    r1 = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (__uint128_t)a[2] + b[2] + c;    r2 = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (__uint128_t)a[3] + b[3] + c;    r3 = (uint64_t)s; c = (uint64_t)(s >> 64);
    uint64_t addt0 = 0, addt1 = c ? 38 : 0;
    s = (__uint128_t)r0 + addt1;         r0 = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (__uint128_t)r1 + addt0 + c;     r1 = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (__uint128_t)r2 + addt0 + c;     r2 = (uint64_t)s; c = (uint64_t)(s >> 64);
    s = (__uint128_t)r3 + addt0 + c;     r3 = (uint64_t)s; c = (uint64_t)(s >> 64);
    addt0 = c ? addt1 : 0;
    r0 += addt0;
    out[0] = r0; out[1] = r1; out[2] = r2; out[3] = r3;
}

/* fe_sub — mirrors SUPERCOP amd64-64 ladderstep.S's pattern.
 *   t = a - b (4-limb chain with borrow)
 *   subt1 = (borrow ? 38 : 0)
 *   t -= subt1 with 4-limb borrow chain (gets new borrow)
 *   subt0 = (borrow ? subt1 : 0)
 *   t[0] -= subt0
 */
static inline void fe_sub(fe4 out, const fe4 a, const fe4 b) {
    __int128 s;
    uint64_t r0, r1, r2, r3;
    uint64_t br;
    s = (__int128)a[0] - (__int128)b[0];                     r0 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)a[1] - (__int128)b[1] - br;                r1 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)a[2] - (__int128)b[2] - br;                r2 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)a[3] - (__int128)b[3] - br;                r3 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    uint64_t subt0 = 0, subt1 = br ? 38 : 0;
    s = (__int128)r0 - subt1;                                 r0 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)r1 - subt0 - br;                            r1 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)r2 - subt0 - br;                            r2 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)r3 - subt0 - br;                            r3 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    subt0 = br ? subt1 : 0;
    r0 -= subt0;
    out[0] = r0; out[1] = r1; out[2] = r2; out[3] = r3;
}

/* fe_mul121665(out, a) = a * 121665 mod p
 *   one MUL per limb (4 MULs total), then fold the high word back via *38.
 */
static inline void fe_mul121665(fe4 out, const fe4 a) {
    __uint128_t t;
    uint64_t r0, r1, r2, r3, hi;
    t = (__uint128_t)a[0] * 121665;                   r0 = (uint64_t)t;
    t = (__uint128_t)a[1] * 121665 + (uint64_t)(t >> 64); r1 = (uint64_t)t;
    t = (__uint128_t)a[2] * 121665 + (uint64_t)(t >> 64); r2 = (uint64_t)t;
    t = (__uint128_t)a[3] * 121665 + (uint64_t)(t >> 64); r3 = (uint64_t)t;
    hi = (uint64_t)(t >> 64);
    /* Fold hi * 38 back into low 4 limbs */
    uint64_t fold = hi * 38;
    t = (__uint128_t)r0 + fold;                        r0 = (uint64_t)t;
    t = (__uint128_t)r1 + (uint64_t)(t >> 64);          r1 = (uint64_t)t;
    t = (__uint128_t)r2 + (uint64_t)(t >> 64);          r2 = (uint64_t)t;
    r3 += (uint64_t)(t >> 64);
    out[0] = r0; out[1] = r1; out[2] = r2; out[3] = r3;
}

/* Constant-time conditional swap. */
static inline void fe_cswap(fe4 a, fe4 b, uint64_t swap) {
    uint64_t mask = 0 - swap;  /* 0 if swap==0, all-1s if swap==1 */
    for (int i = 0; i < 4; i++) {
        uint64_t t = mask & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

static inline void fe_copy(fe4 out, const fe4 a) {
    out[0] = a[0]; out[1] = a[1]; out[2] = a[2]; out[3] = a[3];
}

/* fe_frombytes: 32 little-endian bytes → 4×64 limbs.
 * The high bit (bit 255) is masked off per RFC 7748 §5. */
static void fe_frombytes(fe4 out, const uint8_t in[32]) {
    out[0] =  (uint64_t)in[0]
           | ((uint64_t)in[1]  << 8)
           | ((uint64_t)in[2]  << 16)
           | ((uint64_t)in[3]  << 24)
           | ((uint64_t)in[4]  << 32)
           | ((uint64_t)in[5]  << 40)
           | ((uint64_t)in[6]  << 48)
           | ((uint64_t)in[7]  << 56);
    out[1] =  (uint64_t)in[8]
           | ((uint64_t)in[9]  << 8)
           | ((uint64_t)in[10] << 16)
           | ((uint64_t)in[11] << 24)
           | ((uint64_t)in[12] << 32)
           | ((uint64_t)in[13] << 40)
           | ((uint64_t)in[14] << 48)
           | ((uint64_t)in[15] << 56);
    out[2] =  (uint64_t)in[16]
           | ((uint64_t)in[17] << 8)
           | ((uint64_t)in[18] << 16)
           | ((uint64_t)in[19] << 24)
           | ((uint64_t)in[20] << 32)
           | ((uint64_t)in[21] << 40)
           | ((uint64_t)in[22] << 48)
           | ((uint64_t)in[23] << 56);
    out[3] =  (uint64_t)in[24]
           | ((uint64_t)in[25] << 8)
           | ((uint64_t)in[26] << 16)
           | ((uint64_t)in[27] << 24)
           | ((uint64_t)in[28] << 32)
           | ((uint64_t)in[29] << 40)
           | ((uint64_t)in[30] << 48)
           | (((uint64_t)in[31] & 0x7f) << 56);
}

/* Fully reduce x mod p, then output 32 little-endian bytes. */
static void fe_reduce(fe4 out, const fe4 in) {
    /* in < 2^256; could be in [p, 2^256). Subtract p if in >= p. */
    uint64_t r0 = in[0], r1 = in[1], r2 = in[2], r3 = in[3];
    /* Add 19 (= 2^256 mod p) — if there's a carry, output = (in + 19) mod 2^256;
     * else output = in. Then conditionally subtract 19.
     * Bernstein's "freeze" pattern (in 4×64): conditionally subtract p. */
    uint64_t d0, d1, d2, d3, br;
    __int128 s;
    s = (__int128)r0 - (__int128)CURVE25519_P[0];                d0 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)r1 - (__int128)CURVE25519_P[1] - br;           d1 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)r2 - (__int128)CURVE25519_P[2] - br;           d2 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    s = (__int128)r3 - (__int128)CURVE25519_P[3] - br;           d3 = (uint64_t)s; br = (uint64_t)((s >> 64) & 1);
    /* if br=1, in < p; keep in. if br=0, in >= p; use diff. */
    uint64_t mask = 0 - br;  /* all-1s if br=1 (in<p) */
    out[0] = (r0 & mask) | (d0 & ~mask);
    out[1] = (r1 & mask) | (d1 & ~mask);
    out[2] = (r2 & mask) | (d2 & ~mask);
    out[3] = (r3 & mask) | (d3 & ~mask);
}

static void fe_tobytes(uint8_t out[32], const fe4 in) {
    fe4 t;
    fe_reduce(t, in);
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) {
            out[8*i + j] = (uint8_t)(t[i] >> (8*j));
        }
    }
}

/* Inversion: a^(p-2) mod p via the standard ref10 addition chain. */
static void fe_invert(fe4 out, const fe4 z) {
    fe4 z2, z9, z11, t, t0, t1, t2, t3;
    int i;

    fe_sq(z2, z);
    fe_sq(t, z2);
    fe_sq(t, t);
    fe_mul(z9, t, z);
    fe_mul(z11, z9, z2);
    fe_sq(t, z11);
    fe_mul(t0, t, z9);

    fe_sq(t1, t0);
    for (i = 1; i < 5; i++) fe_sq(t1, t1);
    fe_mul(t1, t1, t0);

    fe_sq(t2, t1);
    for (i = 1; i < 10; i++) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);

    fe_sq(t3, t2);
    for (i = 1; i < 20; i++) fe_sq(t3, t3);
    fe_mul(t3, t3, t2);

    for (i = 0; i < 10; i++) fe_sq(t3, t3);
    fe_mul(t1, t3, t1);

    fe_sq(t2, t1);
    for (i = 1; i < 50; i++) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);

    fe_sq(t3, t2);
    for (i = 1; i < 100; i++) fe_sq(t3, t3);
    fe_mul(t3, t3, t2);

    for (i = 0; i < 50; i++) fe_sq(t3, t3);
    fe_mul(t1, t3, t1);

    fe_sq(t1, t1);
    fe_sq(t1, t1);
    fe_sq(t1, t1);
    fe_sq(t1, t1);
    fe_sq(t1, t1);
    fe_mul(out, t1, z11);
}

/* ════════════════════════════════════════════════════════════════════
 * X25519 LADDER (RFC 7748)
 * ════════════════════════════════════════════════════════════════════ */

static void scalar_clamp(uint8_t s[32]) {
    s[0]  &= 248;
    s[31] &= 127;
    s[31] |= 64;
}

static void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t e[32];
    memcpy(e, scalar, 32);
    scalar_clamp(e);

    fe4 x1, x2, z2, x3, z3;
    fe4 A, AA, B, BB, E, C, D, DA, CB, t0;

    fe_frombytes(x1, point);
    fe_copy(x2, (const uint64_t[]){1, 0, 0, 0});
    memset(z2, 0, sizeof(fe4));
    fe_copy(x3, x1);
    fe_copy(z3, (const uint64_t[]){1, 0, 0, 0});

    uint64_t swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (e[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(A, x2, z2);
        fe_sq(AA, A);
        fe_sub(B, x2, z2);
        fe_sq(BB, B);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul(DA, D, A);
        fe_mul(CB, C, B);

        fe_add(t0, DA, CB);
        fe_sq(x3, t0);

        fe_sub(t0, DA, CB);
        fe_sq(z3, t0);
        fe_mul(z3, x1, z3);

        fe_mul(x2, AA, BB);

        fe_mul121665(t0, E);
        fe_add(t0, AA, t0);
        fe_mul(z2, E, t0);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
}

/* ════════════════════════════════════════════════════════════════════
 * VERIFICATION + BENCH
 * ════════════════════════════════════════════════════════════════════ */

static void hex_to_bytes(const char *hex, uint8_t *out, int len) {
    for (int i = 0; i < len; i++) {
        unsigned int val;
        sscanf(hex + 2*i, "%02x", &val);
        out[i] = (uint8_t)val;
    }
}

static void print_hex(const char *label, const uint8_t *data, int len) {
    printf("  %s: ", label);
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
    printf("\n");
}

static int memcmp_hex(const uint8_t *data, const char *hex, int len) {
    uint8_t expected[32];
    hex_to_bytes(hex, expected, len);
    return memcmp(data, expected, len);
}

static int test_rfc7748(void) {
    int pass = 0, fail = 0;
    uint8_t scalar[32], point[32], r[32];

    printf("=== RFC 7748 Test Vectors (4×64 microcode) ===\n\n");

    /* Vector 1 */
    printf("--- Vector 1 ---\n");
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);
    x25519(r, scalar, point);
    print_hex("got     ", r, 32);
    printf("  expect: c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552\n");
    if (memcmp_hex(r, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    /* Vector 2 */
    printf("\n--- Vector 2 ---\n");
    hex_to_bytes("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", scalar, 32);
    hex_to_bytes("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", point, 32);
    x25519(r, scalar, point);
    print_hex("got     ", r, 32);
    printf("  expect: 95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957\n");
    if (memcmp_hex(r, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    /* Iterated test (1 round) */
    printf("\n--- Iterated test (1 round, scalar=u=9) ---\n");
    uint8_t k[32] = {0}, u[32] = {0};
    k[0] = 9; u[0] = 9;
    x25519(r, k, u);
    print_hex("got     ", r, 32);
    printf("  expect: 422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079\n");
    if (memcmp_hex(r, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    /* Iterated test (1000 rounds) — full RFC 7748 §5.2 verification */
    printf("\n--- Iterated test (1000 rounds) ---\n");
    uint8_t kn[32], un[32];
    memcpy(kn, k, 32);
    memcpy(un, u, 32);
    for (int i = 0; i < 1000; i++) {
        x25519(r, kn, un);
        memcpy(un, kn, 32);
        memcpy(kn, r, 32);
    }
    print_hex("got     ", kn, 32);
    printf("  expect: 684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51\n");
    if (memcmp_hex(kn, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    printf("\n=== RFC 7748: %d / %d passed ===\n\n", pass, pass + fail);
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

#define BENCH_REPS 100

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(void) {
    printf("=== X25519 via 4×64 chained-ADC microcode ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_mul_patch();
    printf("(fe_sq uses fe_mul(a,a) wrapper — no separate sq patch)\n\n");

    int fail = test_rfc7748();
    if (fail) {
        printf("Verification FAILED, aborting bench.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    /* Bench: time one X25519 op many times, report min/median */
    uint8_t scalar[32], point[32], out[32];
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);

    uint64_t samples[BENCH_REPS];
    /* warmup */
    for (int i = 0; i < 5; i++) x25519(out, scalar, point);

    for (int r = 0; r < BENCH_REPS; r++) {
        uint64_t t0 = rdtsc_start();
        x25519(out, scalar, point);
        uint64_t t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    qsort(samples, BENCH_REPS, sizeof(samples[0]), cmp_u64);

    printf("--- Bench (%d reps) ---\n", BENCH_REPS);
    printf("min: %" PRIu64 " cyc\n", samples[0]);
    printf("median: %" PRIu64 " cyc\n", samples[BENCH_REPS/2]);
    printf("p90: %" PRIu64 " cyc\n", samples[BENCH_REPS*9/10]);
    printf("\nFor reference:\n");
    printf("  5×51 microcode (production): ~312000 cyc\n");
    printf("  amd64-64 (SUPERCOP, asm):    ~272000 cyc\n");

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
