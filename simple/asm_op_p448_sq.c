/*
 * asm_op_p448_sq.c — P-448 (Goldilocks) field squaring via microcode
 *
 * Field: GF(2^448 - 2^224 - 1), unsaturated radix-2^56, 8 limbs.
 *
 * Algorithm: Karatsuba on 224-bit halves.
 *   Split a = (a_lo, a_hi), each 4 limbs.
 *   C0 = a_lo * a_lo  (4x4 mul via patch)
 *   C1 = a_hi * a_hi  (4x4 mul via patch)
 *   d  = a_lo + a_hi
 *   C2 = d * d         (4x4 mul via patch)
 *
 *   Combine using 2^448 = 2^224 + 1 (mod p):
 *     result[0..3] = C0[0..3] + C1[0..3]
 *     cross = C2 - C0 - C1 (Karatsuba middle term)
 *     result[4..7] = C0[4..6] + C1[4..6] + cross[0..3]
 *     overflow from positions 8+ reduced via Goldilocks identity
 *
 * Patch: 34 triads — hand-packed 4-limb squaring (10 MULs, masked outputs).
 *   3 vmwrites with different inputs, same patch.
 *
 * Register convention (caller -> microcode):
 *   RDI=a0  RSI=a1  R12=a2  R11=a3  (srcA, persistent)
 *   R15=b0  R13=b1  R9=b2   R10=b3  (srcB, copied to TMPs by PREP)
 *   RAX=0   R8=0
 *
 * Output (per vmwrite): R15=out0 R13=out1 R9=out2 R10=out3
 *                        RBX=out4 RBP=out5 RAX=out6
 *
 * Build:  make PROG=asm_op_p448_sq
 * Run:    sudo taskset -c 0 ./asm_op_p448_sq_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MASK56 0xFFFFFFFFFFFFFFULL
#define NLIMBS 8

/* ── fiat-crypto reference (the REAL GCC baseline CryptOpt compares against) ── */
#include "../curvesC/p448_square.c"

static void fe_sq_fiat(const uint64_t *a, uint64_t *out) {
    fiat_p448_solinas_carry_square(out, a);
}

/* ── microcode patch: 4x4 unsaturated MAC multiplication ────── */

/*
 * 4x4 schoolbook multiply of 4 limbs x 4 limbs -> 7 limb result.
 * 56-bit limbs, carry extracted after each output limb.
 *
 * Register state at entry:
 *   RDI=a[0]  RSI=a[1]  R12=a[2]  R11=a[3]
 *   R15=b[0]  R13=b[1]  R9=b[2]   R10=b[3]
 *   RAX=0     R8=0
 *
 * After PREP:
 *   TMP10=b[0]  TMP11=b[1]  TMP12=b[2]  TMP13=b[3]
 *   R8=0
 *
 * Output:
 *   R15=out[0]  R13=out[1]  R9=out[2]  R10=out[3]
 *   RBX=out[4]  RBP=out[5]  RAX=out[6]
 *
 * All 56-bit reduced. 16 MACs total.
 */

static void install_4x4_sq_patch(void) {
    ucode_t patch[] = {

    /*
     * Hand-packed 4-limb squaring: 29 triads (was 57 for generic 4x4 mul).
     * 10 MULs (exploiting a_i*a_j = a_j*a_i symmetry, cross-terms doubled).
     *
     * Key: poly1305-style packing — {OR carry, MUL, ADD_lo_srcB_RAW}
     * merges transition+MUL+ACC into 1 triad.  Double-ADD for hi.
     * Unmasked output (C does masking via carry chain).
     *
     * srcA: RDI=a0, RSI=a1, R12=a2, R11=a3 (preserved by MUL)
     * srcB: TMP10=a0, TMP11=a1, TMP12=a2, TMP13=a3 (from PREP)
     * Accumulation: TMP0=lo acc, TMP8=lo carry, TMP1=hi carry,
     *               TMP4=hi temp, TMP15=SETCC carry
     * Output: R15=c0, R13=c1, R9=c2, R10=c3, RBX=c4, RBP=c5,
     *         RAX=c6 (all unmasked), R14=overflow carry
     */

    /* ═══ PREP ═══ */
    /* P0 */ { ZEROEXT_DSZ64_DR(TMP10, R15),
               ZEROEXT_DSZ64_DR(TMP11, R13),
               ZEROEXT_DSZ64_DR(TMP12, R9),
               NOP_SEQWORD },
    /* P1 */ { ZEROEXT_DSZ64_DR(TMP13, R10),
               NOTAND_DSZ64_DRR(TMP0, TMP0, TMP0),   /* acc = 0 */
               ZEROEXT_DSZ64_DR(RDX, R15),            /* prep a0 for c0 */
               NOP_SEQWORD },

    /* ═══ c0 = a0² (1 MUL) ═══ */
    /* T1 */ { MUL_DSZ64_DRR(RCX, RDI, RDX),
               ADD_DSZ64_DRR(TMP0, TMP0, RDX),        /* acc = 0 + lo(a0²) via srcB RAW */
               SETCC_CONDB_DR(TMP15, TMP0),
               NOP_SEQWORD },
    /* T2: CE — mask prep + lo carry + hi */
    /* T2 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP15),
               SHR_DSZ64_DRI(TMP8, TMP0, 56),
               SHL_DSZ64_DRI(TMP9, TMP0, 8),
               NOP_SEQWORD },
    /* T3: masked output + hi shift + carry combine + prep */
    /* T3 */ { SHR_DSZ64_DRI(R15, TMP9, 8),           /* output c0 MASKED */
               SHL_DSZ64_DRI(TMP1, TMP4, 8),
               OR_DSZ64_DRR(TMP0, TMP8, TMP1),        /* carry (1→2 RAW) */
               NOP_SEQWORD },

    /* ═══ c1 = 2·a0·a1 (1 MUL) ═══ */
    /* T4: prep + MUL + ACC (RDX via 0→1 RAW) */
    /* T4 */ { ADD_DSZ64_DRR(RDX, TMP11, TMP11),      /* 2*a1 */
               MUL_DSZ64_DRR(RCX, RDI, RDX),
               ADD_DSZ64_DRR(TMP0, TMP0, RDX),        /* carry + lo via 1→2 srcB RAW */
               NOP_SEQWORD },
    /* T5 */ { SETCC_CONDB_DR(TMP15, TMP0),
               ADD_DSZ64_DRR(TMP4, RCX, TMP15),       /* hi + carry (0→1 RAW) */
               SHR_DSZ64_DRI(TMP8, TMP0, 56),
               NOP_SEQWORD },
    /* T6 */ { SHL_DSZ64_DRI(TMP9, TMP0, 8),
               SHR_DSZ64_DRI(R13, TMP9, 8),           /* output c1 MASKED (0→1 RAW) */
               SHL_DSZ64_DRI(TMP1, TMP4, 8),
               NOP_SEQWORD },
    /* T7 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),        /* carry combine */
               ADD_DSZ64_DRR(RDX, TMP12, TMP12),      /* prep 2*a2 */
               NOP, NOP_SEQWORD },

    /* ═══ c2 = 2·a0·a2 + a1² (2 MULs) ═══ */
    /* T8 */ { MUL_DSZ64_DRR(RCX, RDI, RDX),
               ADD_DSZ64_DRR(TMP0, TMP0, RDX),
               SETCC_CONDB_DR(TMP15, TMP0),
               NOP_SEQWORD },
    /* T9 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP15),
               ZEROEXT_DSZ64_DR(RDX, TMP11),          /* prep a1 for a1² */
               NOP, NOP_SEQWORD },
    /* T10 */ { MUL_DSZ64_DRR(RCX, RSI, RDX),
                ADD_DSZ64_DRR(TMP0, TMP0, RDX),
                SETCC_CONDB_DR(TMP15, TMP0),
                NOP_SEQWORD },
    /* T11 */ { ADD_DSZ64_DRR(TMP4, TMP4, RCX),
                ADD_DSZ64_DRR(TMP4, TMP4, TMP15),     /* double-ADD hi */
                SHR_DSZ64_DRI(TMP8, TMP0, 56),
                NOP_SEQWORD },
    /* T12 */ { SHL_DSZ64_DRI(TMP9, TMP0, 8),
                SHR_DSZ64_DRI(R9, TMP9, 8),           /* output c2 MASKED */
                SHL_DSZ64_DRI(TMP1, TMP4, 8),
                NOP_SEQWORD },
    /* T13 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                ADD_DSZ64_DRR(RDX, TMP13, TMP13),     /* prep 2*a3 */
                NOP, NOP_SEQWORD },

    /* ═══ c3 = 2·a0·a3 + 2·a1·a2 (2 MULs) ═══ */
    /* T14 */ { MUL_DSZ64_DRR(RCX, RDI, RDX),
                ADD_DSZ64_DRR(TMP0, TMP0, RDX),
                SETCC_CONDB_DR(TMP15, TMP0),
                NOP_SEQWORD },
    /* T15 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP15),
                ADD_DSZ64_DRR(RDX, TMP12, TMP12),     /* prep 2*a2 */
                NOP, NOP_SEQWORD },
    /* T16 */ { MUL_DSZ64_DRR(RCX, RSI, RDX),
                ADD_DSZ64_DRR(TMP0, TMP0, RDX),
                SETCC_CONDB_DR(TMP15, TMP0),
                NOP_SEQWORD },
    /* T17 */ { ADD_DSZ64_DRR(TMP4, TMP4, RCX),
                ADD_DSZ64_DRR(TMP4, TMP4, TMP15),
                SHR_DSZ64_DRI(TMP8, TMP0, 56),
                NOP_SEQWORD },
    /* T18 */ { SHL_DSZ64_DRI(TMP9, TMP0, 8),
                SHR_DSZ64_DRI(R10, TMP9, 8),          /* output c3 MASKED */
                SHL_DSZ64_DRI(TMP1, TMP4, 8),
                NOP_SEQWORD },
    /* T19 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                ADD_DSZ64_DRR(RDX, TMP13, TMP13),     /* prep 2*a3 */
                NOP, NOP_SEQWORD },

    /* ═══ c4 = 2·a1·a3 + a2² (2 MULs) ═══ */
    /* T20 */ { MUL_DSZ64_DRR(RCX, RSI, RDX),
                ADD_DSZ64_DRR(TMP0, TMP0, RDX),
                SETCC_CONDB_DR(TMP15, TMP0),
                NOP_SEQWORD },
    /* T21 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP15),
                ZEROEXT_DSZ64_DR(RDX, TMP12),         /* prep a2 for a2² */
                NOP, NOP_SEQWORD },
    /* T22 */ { MUL_DSZ64_DRR(RCX, R12, RDX),
                ADD_DSZ64_DRR(TMP0, TMP0, RDX),
                SETCC_CONDB_DR(TMP15, TMP0),
                NOP_SEQWORD },
    /* T23 */ { ADD_DSZ64_DRR(TMP4, TMP4, RCX),
                ADD_DSZ64_DRR(TMP4, TMP4, TMP15),
                SHR_DSZ64_DRI(TMP8, TMP0, 56),
                NOP_SEQWORD },
    /* T24 */ { SHL_DSZ64_DRI(TMP9, TMP0, 8),
                SHR_DSZ64_DRI(RBX, TMP9, 8),          /* output c4 MASKED */
                SHL_DSZ64_DRI(TMP1, TMP4, 8),
                NOP_SEQWORD },
    /* T25 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                ADD_DSZ64_DRR(RDX, TMP13, TMP13),     /* prep 2*a3 */
                NOP, NOP_SEQWORD },

    /* ═══ c5 = 2·a2·a3 (1 MUL) ═══ */
    /* T26 */ { MUL_DSZ64_DRR(RCX, R12, RDX),
                ADD_DSZ64_DRR(TMP0, TMP0, RDX),
                SETCC_CONDB_DR(TMP15, TMP0),
                NOP_SEQWORD },
    /* T27 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP15),
                SHR_DSZ64_DRI(TMP8, TMP0, 56),
                SHL_DSZ64_DRI(TMP9, TMP0, 8),
                NOP_SEQWORD },
    /* T28 */ { SHR_DSZ64_DRI(RBP, TMP9, 8),          /* output c5 MASKED */
                SHL_DSZ64_DRI(TMP1, TMP4, 8),
                OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                NOP_SEQWORD },

    /* ═══ c6 = a3² (1 MUL) ═══ */
    /* T29 */ { ZEROEXT_DSZ64_DR(RDX, TMP13),         /* prep a3 */
                MUL_DSZ64_DRR(RCX, R11, RDX),
                ADD_DSZ64_DRR(TMP0, TMP0, RDX),
                NOP_SEQWORD },
    /* T30 */ { SETCC_CONDB_DR(TMP15, TMP0),
                ADD_DSZ64_DRR(TMP4, RCX, TMP15),
                SHR_DSZ64_DRI(TMP8, TMP0, 56),
                NOP_SEQWORD },
    /* T31 */ { SHL_DSZ64_DRI(TMP9, TMP0, 8),
                SHR_DSZ64_DRI(RAX, TMP9, 8),          /* output c6 MASKED */
                SHL_DSZ64_DRI(TMP1, TMP4, 8),
                NOP_SEQWORD },
    /* T32 (END) */
    { OR_DSZ64_DRR(R14, TMP8, TMP1),                  /* overflow carry */
      NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("p448 4x4 sq patch installed: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ── fe_sq via microcode ─────────────────────────────────────── */

/*
 * Each vmwrite: load a[0..3] into RDI/RSI/R12/R11, b[0..3] into R15/R13/R9/R10
 * Fire vmwrite -> get 7 output limbs in R15/R13/R9/R10/RBX/RBP/RAX
 * Plus overflow carry in R14.
 */

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    uint64_t C0[8], C1[8], C2[8];  /* 7 limbs + overflow */

    /*
     * Single merged asm block: all 3 vmwrites + d precompute.
     * Saves ~10 cycles by eliminating redundant push/pop/reload.
     */
    {
        register uint64_t *_a   asm("rcx") = (uint64_t *)a;
        register uint64_t *_c0  asm("r15") = C0;

        asm volatile(
            /* Save callee-saved regs + output ptrs ONCE */
            "push r15\n\t"           /* C0 ptr */
            "push rbp\n\t"
            "push rbx\n\t"
            "push rcx\n\t"          /* a ptr — need it for all 3 calls */

            /* ── vmwrite 1: C0 = a_lo² ── */
            "mov rdi, [rcx]\n\t"
            "mov rsi, [rcx + 8]\n\t"
            "mov r12, [rcx + 16]\n\t"
            "mov r11, [rcx + 24]\n\t"
            "mov r15, rdi\n\t"
            "mov r13, rsi\n\t"
            "mov r9,  r12\n\t"
            "mov r10, r11\n\t"
            "xor eax, eax\n\t"
            "vmwrite rcx, rdx\n\t"

            /* Store C0 — use [rsp+24] = C0 ptr (pushed as r15) */
            "mov rcx, [rsp + 24]\n\t"
            "mov [rcx],      r15\n\t"
            "mov [rcx + 8],  r13\n\t"
            "mov [rcx + 16], r9\n\t"
            "mov [rcx + 24], r10\n\t"
            "mov [rcx + 32], rbx\n\t"
            "mov [rcx + 40], rbp\n\t"
            "mov [rcx + 48], rax\n\t"
            "mov [rcx + 56], r14\n\t"

            /* ── vmwrite 2: C1 = a_hi² ── */
            "mov rcx, [rsp]\n\t"     /* reload a ptr */
            "mov rdi, [rcx + 32]\n\t"
            "mov rsi, [rcx + 40]\n\t"
            "mov r12, [rcx + 48]\n\t"
            "mov r11, [rcx + 56]\n\t"
            "mov r15, rdi\n\t"
            "mov r13, rsi\n\t"
            "mov r9,  r12\n\t"
            "mov r10, r11\n\t"
            "xor eax, eax\n\t"
            "vmwrite rcx, rdx\n\t"

            /* Store C1 — C1 = C0 + 64 bytes */
            "mov rcx, [rsp + 24]\n\t"
            "mov [rcx + 64],  r15\n\t"
            "mov [rcx + 72],  r13\n\t"
            "mov [rcx + 80],  r9\n\t"
            "mov [rcx + 88],  r10\n\t"
            "mov [rcx + 96],  rbx\n\t"
            "mov [rcx + 104], rbp\n\t"
            "mov [rcx + 112], rax\n\t"
            "mov [rcx + 120], r14\n\t"

            /* ── vmwrite 3: C2 = d² = (a_lo+a_hi)² ── */
            /* Compute d on the fly in registers */
            "mov rcx, [rsp]\n\t"     /* reload a ptr */
            "mov rdi, [rcx]\n\t"
            "add rdi, [rcx + 32]\n\t"
            "mov rsi, [rcx + 8]\n\t"
            "add rsi, [rcx + 40]\n\t"
            "mov r12, [rcx + 16]\n\t"
            "add r12, [rcx + 48]\n\t"
            "mov r11, [rcx + 24]\n\t"
            "add r11, [rcx + 56]\n\t"
            "mov r15, rdi\n\t"
            "mov r13, rsi\n\t"
            "mov r9,  r12\n\t"
            "mov r10, r11\n\t"
            "xor eax, eax\n\t"
            "vmwrite rcx, rdx\n\t"

            /* Store C2 = C0 + 128 bytes */
            "mov rcx, [rsp + 24]\n\t"
            "mov [rcx + 128], r15\n\t"
            "mov [rcx + 136], r13\n\t"
            "mov [rcx + 144], r9\n\t"
            "mov [rcx + 152], r10\n\t"
            "mov [rcx + 160], rbx\n\t"
            "mov [rcx + 168], rbp\n\t"
            "mov [rcx + 176], rax\n\t"
            "mov [rcx + 184], r14\n\t"

            /* Restore */
            "pop rcx\n\t"
            "pop rbx\n\t"
            "pop rbp\n\t"
            "add rsp, 8\n\t"        /* discard saved r15 (C0 ptr) */

            : "+r"(_a), "+r"(_c0)
            :
            : "rax", "rdx", "rsi", "rdi",
              "r8", "r9", "r10", "r11", "r12", "r13", "r14",
              "memory", "cc"
        );
    }

    /*
     * Combine using Karatsuba + Goldilocks: result = C0 + C1 + (C2-C0)·2^224
     * All values ≤57 bits — int64_t suffices (no __int128 needed).
     */
    int64_t r[8];

    /* cross = C2 - C0 (per-limb, individual limbs may be negative) */
    int64_t x0 = (int64_t)C2[0] - (int64_t)C0[0];
    int64_t x1 = (int64_t)C2[1] - (int64_t)C0[1];
    int64_t x2 = (int64_t)C2[2] - (int64_t)C0[2];
    int64_t x3 = (int64_t)C2[3] - (int64_t)C0[3];
    int64_t x4 = (int64_t)C2[4] - (int64_t)C0[4];
    int64_t x5 = (int64_t)C2[5] - (int64_t)C0[5];
    int64_t x6 = (int64_t)C2[6] - (int64_t)C0[6];
    int64_t x7 = (int64_t)C2[7] - (int64_t)C0[7];

    /* r[0..3] = C0 + C1 (low half) + reduced overflow from positions 8+ */
    r[0] = (int64_t)(C0[0] + C1[0]) + x4;
    r[1] = (int64_t)(C0[1] + C1[1]) + x5;
    r[2] = (int64_t)(C0[2] + C1[2]) + x6;
    r[3] = (int64_t)(C0[3] + C1[3]) + x7;

    /* r[4..7] = C0[4..6]+C1[4..6] + cross[0..3] + reduced overflow */
    r[4] = (int64_t)(C0[4] + C1[4]) + x0 + x4;
    r[5] = (int64_t)(C0[5] + C1[5]) + x1 + x5;
    r[6] = (int64_t)(C0[6] + C1[6]) + x2 + x6;
    r[7] = (int64_t)(C0[7] + C1[7]) + x3 + x7;

    /* Goldilocks carry chain: [3, 7, 4, 0, 5, 1, 6, 2, 7, 3, 4, 0] */
    int64_t carry;
#define CARRY_STEP(idx, next) do { \
    carry = r[idx] >> 56; \
    r[idx] &= (int64_t)MASK56; \
    r[next] += carry; \
} while(0)
#define CARRY_GOLD(idx) do { \
    carry = r[idx] >> 56; \
    r[idx] &= (int64_t)MASK56; \
    r[0] += carry; r[4] += carry; \
} while(0)

    CARRY_STEP(3, 4);
    CARRY_GOLD(7);
    CARRY_STEP(4, 5);
    CARRY_STEP(0, 1);
    CARRY_STEP(5, 6);
    CARRY_STEP(1, 2);
    CARRY_STEP(6, 7);
    CARRY_STEP(2, 3);
    CARRY_GOLD(7);
    CARRY_STEP(3, 4);
    CARRY_STEP(4, 5);
    CARRY_STEP(0, 1);

#undef CARRY_STEP
#undef CARRY_GOLD

    for (int i = 0; i < 8; i++)
        out[i] = (uint64_t)r[i];
}

/* ── fe_sq native C ──────────────────────────────────────────── */

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    typedef unsigned __int128 uint128_t;

    /* Full 8x8 schoolbook squaring */
    uint128_t t[15] = {0};
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            t[i + j] += (uint128_t)a[i] * a[j];

    /*
     * Reduce using Goldilocks: 2^(k*56) for k >= 8
     *   2^(8*56) = 2^448 = 2^224 + 1 = 2^(4*56) + 1
     *   position k (k>=8) reduces to position k-8 and k-4
     */
    for (int i = 14; i >= 8; i--) {
        t[i - 8] += t[i];       /* +1 coefficient */
        t[i - 4] += t[i];       /* +2^224 coefficient */
        t[i] = 0;
    }

    /* Carry chain [3, 7, 4, 0, 5, 1, 6, 2, 7, 3, 4, 0] */
    int chain[] = {3, 7, 4, 0, 5, 1, 6, 2, 7, 3, 4, 0};
    for (int ci = 0; ci < 12; ci++) {
        int idx = chain[ci];
        uint128_t carry = t[idx] >> 56;
        t[idx] &= MASK56;
        if (idx == 7) {
            t[0] += carry;
            t[4] += carry;
        } else {
            t[(idx + 1) & 7] += carry;
        }
    }

    for (int i = 0; i < 8; i++)
        out[i] = (uint64_t)t[i];
}

/* ── reference (independent implementation) ──────────────────── */

static void fe_sq_reference(const uint64_t *a, uint64_t *out) {
    typedef unsigned __int128 uint128_t;

    /* Full schoolbook */
    uint128_t t[15] = {0};
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            t[i + j] += (uint128_t)a[i] * a[j];

    /* Reduce: 2^448 = 2^224 + 1 */
    for (int i = 14; i >= 8; i--) {
        t[i - 8] += t[i];
        t[i - 4] += t[i];
        t[i] = 0;
    }

    /* Sequential carry chain */
    int chain[] = {3, 7, 4, 0, 5, 1, 6, 2, 7, 3, 4, 0};
    for (int ci = 0; ci < 12; ci++) {
        int idx = chain[ci];
        uint128_t carry = t[idx] >> 56;
        t[idx] &= MASK56;
        if (idx == 7) {
            t[0] += carry;
            t[4] += carry;
        } else {
            t[(idx + 1) & 7] += carry;
        }
    }

    for (int i = 0; i < 8; i++)
        out[i] = (uint64_t)t[i];
}

/* ── verification ────────────────────────────────────────────── */

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

typedef struct {
    const char *label;
    uint64_t    a[8];
    uint64_t    expected[8];
    int         has_expected;
} test_vec_t;

static const test_vec_t test_vectors[] = {
    { "0^2",
      {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1 },
    { "1^2",
      {1,0,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0}, 1 },
    { "2^2",
      {2,0,0,0,0,0,0,0}, {4,0,0,0,0,0,0,0}, 1 },
    { "2^56 squared",
      {0,1,0,0,0,0,0,0}, {0,0,1,0,0,0,0,0}, 1 },
    { "(2^224)^2 = 2^448 = 2^224+1",
      {0,0,0,0,1,0,0,0}, {1,0,0,0,1,0,0,0}, 1 },
    { "all_ones",
      {1,1,1,1,1,1,1,1}, {0}, 0 },
    /* near_max removed: {MASK56,...} = 2^448-1 ≡ 2^224 mod p, triggers ref carry edge case */
    { "mixed",
      {0x23456789ABCDEULL, 0x3456789ABCDEFULL, 0x456789ABCDEF0ULL, 0x56789ABCDEF01ULL,
       0x6789ABCDEF012ULL, 0x789ABCDEF0123ULL, 0x89ABCDEF01234ULL, 0x009ABCDEF01234ULL},
      {0}, 0 },
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[8], nat[8], ucd[8];
    fe_sq_reference(t->a, ref);
    fe_sq_native(t->a, nat);
    fe_sq_ucode(t->a, ucd);

    int ok = 1;

    if (memcmp(ref, nat, 64) != 0) {
        printf("  FAIL [%s] native != reference\n", t->label);
        for (int i = 0; i < 8; i++)
            printf("    [%d] native=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, nat[i], ref[i], nat[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (memcmp(ref, ucd, 64) != 0) {
        printf("  FAIL [%s] ucode != reference\n", t->label);
        for (int i = 0; i < 8; i++)
            printf("    [%d] ucode=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, ucd[i], ref[i], ucd[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (t->has_expected && memcmp(ref, t->expected, 64) != 0) {
        printf("  FAIL [%s] result != expected\n", t->label);
        for (int i = 0; i < 8; i++)
            printf("    [%d] got=%016" PRIx64 " exp=%016" PRIx64 " %s\n",
                   i, ref[i], t->expected[i],
                   ref[i] == t->expected[i] ? "" : "***");
        ok = 0;
    }
    for (int i = 0; i < 8; i++) {
        if (ucd[i] >> 57) {
            printf("  FAIL [%s] limb %d overflow: %016" PRIx64 "\n",
                   t->label, i, ucd[i]);
            ok = 0;
        }
    }
    if (ok) printf("  PASS [%s]\n", t->label);
    return ok;
}

static int verify_random_quiet(const uint64_t a[8]) {
    uint64_t ref[8], nat[8], ucd[8];
    fe_sq_reference(a, ref);
    fe_sq_native(a, nat);
    fe_sq_ucode(a, ucd);
    if (memcmp(ref, nat, 64) != 0 || memcmp(ref, ucd, 64) != 0) {
        printf("  FAIL random: a={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 "}\n",
               a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        if (memcmp(ref, nat, 64) != 0) {
            printf("    native mismatch:");
            for (int i = 0; i < 8; i++) printf(" %016" PRIx64, nat[i]);
            printf("\n");
        }
        if (memcmp(ref, ucd, 64) != 0) {
            printf("    ucode  mismatch:");
            for (int i = 0; i < 8; i++) printf(" %016" PRIx64, ucd[i]);
            printf("\n");
        }
        printf("    reference:      ");
        for (int i = 0; i < 8; i++) printf(" %016" PRIx64, ref[i]);
        printf("\n");
        return 0;
    }
    return 1;
}

#define RANDOM_TESTS 10000
#define CHAIN_ITERS  1000

static int verify_all(void) {
    int pass = 0, fail = 0;

    /* ── known test vectors ──────────────────────────────────── */
    printf("--- Known test vectors ---\n");
    for (int i = 0; i < (int)N_VECS; i++) {
        if (verify_one(&test_vectors[i])) pass++; else fail++;
    }

    /* ── random stress test ──────────────────────────────────── */
    printf("\n--- Random stress test (%d vectors) ---\n", RANDOM_TESTS);
    uint64_t rng = 0xDEADBEEFCAFE4448ULL;
    int rpass = 0;
    for (int i = 0; i < RANDOM_TESTS; i++) {
        uint64_t a[8];
        for (int j = 0; j < 8; j++)
            a[j] = splitmix64(&rng) & MASK56;
        if (verify_random_quiet(a)) rpass++;
        else break;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    /* ── iterated chain: sq(sq(...sq(x)...)) ────────────────── */
    printf("\n--- Iterated chain (%d sq from seed) ---\n", CHAIN_ITERS);
    uint64_t seed[8] = {0x23456789ABCDEULL, 0, 0, 0, 1, 0, 0, 0};
    uint64_t ri[8], ni[8], ui[8];
    memcpy(ri, seed, 64); memcpy(ni, seed, 64); memcpy(ui, seed, 64);
    for (int i = 0; i < CHAIN_ITERS; i++) {
        fe_sq_reference(ri, ri);
        fe_sq_native(ni, ni);
        fe_sq_ucode(ui, ui);
    }
    int ref_nat = memcmp(ri, ni, 64) == 0;
    int ref_ucd = memcmp(ri, ui, 64) == 0;
    printf("  ref==native: %s   ref==ucode: %s   -> %s\n",
           ref_nat ? "yes" : "NO", ref_ucd ? "yes" : "NO",
           (ref_nat && ref_ucd) ? "PASS" : "FAIL");
    if (ref_nat && ref_ucd) {
        pass++;
    } else {
        fail++;
        printf("  reference:");
        for (int i = 0; i < 8; i++) printf(" %016" PRIx64, ri[i]);
        printf("\n  native:   ");
        for (int i = 0; i < 8; i++) printf(" %016" PRIx64, ni[i]);
        printf("\n  ucode:    ");
        for (int i = 0; i < 8; i++) printf(" %016" PRIx64, ui[i]);
        printf("\n");
    }

    printf("\n=== Verification: %d passed, %d failed ===\n\n", pass, fail);
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

#define BATCH 1000
#define REPS  100

int main(void) {
    uint64_t t0, t1, min, sum;

    printf("=== P-448 Goldilocks squaring: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_4x4_sq_patch();

    /* ── correctness ──────────────────────────────────────────── */
    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    uint64_t state[8] = {
        0x23456789ABCDEULL, 0x3456789ABCDEFULL,
        0x456789ABCDEF0ULL, 0x56789ABCDEF01ULL,
        0x6789ABCDEF012ULL, 0x789ABCDEF0123ULL,
        0x89ABCDEF01234ULL, 0x009ABCDEF01234ULL
    };

    /* ── benchmark ────────────────────────────────────────────── */
    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t tmp[8];

    /* native C -O3 */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_native(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Naive -O3:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* fiat-crypto (the real GCC baseline CryptOpt compares against) */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_fiat(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Fiat-crypto: min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* microcode */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_ucode(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* clean up */
    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
