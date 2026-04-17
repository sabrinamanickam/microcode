/*
 * asm_op_p256_sq.c — Montgomery squaring for P-256 via microcode
 *
 * Single-iteration patch (56 triads), called 4× via vmwrite.
 * NO TMP persistence between calls — b values in arch regs, acc[4] in RAX.
 * Conditional subtract in native C.
 *
 * Build:  make PROG=asm_op_p256_sq
 * Run:    sudo taskset -c 0 ./asm_op_p256_sq_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

static const uint64_t P256_P[4] = {
    UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x00000000FFFFFFFF),
    UINT64_C(0x0000000000000000), UINT64_C(0xFFFFFFFF00000001)
};

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

    uint64_t m = acc[0];
    t = (__uint128_t)m * UINT64_C(0xFFFFFFFFFFFFFFFF);
    uint64_t mp0_lo = (uint64_t)t, mp0_hi = (uint64_t)(t >> 64);
    t = (__uint128_t)m * UINT32_C(0xFFFFFFFF);
    uint64_t mp1_lo = (uint64_t)t, mp1_hi = (uint64_t)(t >> 64);
    t = (__uint128_t)m * UINT64_C(0xFFFFFFFF00000001);
    uint64_t mp3_lo = (uint64_t)t, mp3_hi = (uint64_t)(t >> 64);

    t = (__uint128_t)mp0_hi + mp1_lo;
    uint64_t red1 = (uint64_t)t;
    uint64_t red2 = mp1_hi + (uint64_t)(t >> 64);

    t = (__uint128_t)acc[0] + mp0_lo;  c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[1] + red1 + c; acc[0] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[2] + red2 + c; acc[1] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc[3] + mp3_lo + c; acc[2] = (uint64_t)t; c = (uint64_t)(t >> 64);
    t = (__uint128_t)acc4 + mp3_hi + c; acc[3] = (uint64_t)t;
    acc[4] = (uint64_t)(t >> 64) + acc4_hi;
}

static inline void cond_subtract(const uint64_t acc[5], uint64_t *out) {
    uint64_t diff[4];
    __uint128_t b128;
    b128 = (__uint128_t)acc[0] - UINT64_C(0xFFFFFFFFFFFFFFFF);
    diff[0] = (uint64_t)b128;
    b128 = (__uint128_t)acc[1] - UINT32_C(0xFFFFFFFF) - ((uint64_t)(b128 >> 64) & 1);
    diff[1] = (uint64_t)b128;
    b128 = (__uint128_t)acc[2] - 0 - ((uint64_t)(b128 >> 64) & 1);
    diff[2] = (uint64_t)b128;
    b128 = (__uint128_t)acc[3] - UINT64_C(0xFFFFFFFF00000001) - ((uint64_t)(b128 >> 64) & 1);
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
        uint64_t m = acc[0];
        c = 0;
        for (int j = 0; j < 4; j++) {
            c += (__uint128_t)m * P256_P[j] + acc[j];
            acc[j] = (uint64_t)c; c >>= 64;
        }
        word4 += (uint64_t)c;
        acc[0]=acc[1]; acc[1]=acc[2]; acc[2]=acc[3];
        acc[3]=(uint64_t)word4; acc[4]=(uint64_t)(word4>>64);
    }
    cond_subtract(acc, out);
}

/* OLD SECTION REMOVED */
#if 0
__placeholder__
 * 2-iterations-per-vmwrite.  2 vmwrites = 4 Montgomery iterations.
 * Patch: PREP(2) + ITER(53) + switch(1) + ITER(53) + END(1) = 110 triads.
 */

/* One Montgomery iteration: 53 triads.
 * Reads a_i from RDI, b from TMP10-13, acc from R15/R9/R10/R13/RAX.
 * Does NOT touch: RSI,R11,R12,R14 (b arch), R8,RBX (p), TMP15. */
#undef MONT_ITER
#define MONT_ITER \
 *
 * Arch regs (persist across calls):
 *   R15=acc[0]  R9=acc[1]  R10=acc[2]  R13=acc[3]  RAX=acc[4]
 *   RSI=b[0]    R12=b[1]   R11=b[2]    R14=b[3]
 *   R8=p[0] (all 1s)       RBX=p[3]
 *   RDI=a_i (set by caller before each vmwrite)
 *
 * TMP regs (set at start of each iteration, NOT assumed to persist):
 *   TMP10-13 = b[0..3] (reloaded from RSI,R12,R11,R14)
 *
 * p[1]=0xFFFFFFFF computed on-the-fly as SHR(R8, 32) inside Phase B.
 *
 * SETCC rule: only after ADD to TMP registers, never arch registers.
 */

/*
 * Macro for one iteration body (53 triads).
 * Expects: RDI=a_i, TMP10-13=b[0..3], acc in R15/R9/R10/R13/RAX.
 */
#define MONT_ITER \
    /* Phase A: schoolbook a_i(RDI) × b(TMP10-13) → product (15 triads) */
    { ZEROEXT_DSZ64_DR(RDX, RDI), NOP, NOP, NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, TMP10, RDX), NOP, NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP0, RDX), ZEROEXT_DSZ64_DR(TMP1, RCX),
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, TMP11, RDX), NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP1, RDX), SETCC_CONDB_DR(TMP3, TMP2),
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RDX, RDI), NOP, NOP, NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, TMP12, RDX), NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, TMP1, RDX), SETCC_CONDB_DR(TMP5, TMP4),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP3), SETCC_CONDB_DR(TMP6, TMP4),
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP5, TMP6), ZEROEXT_DSZ64_DR(RDX, RDI),
      NOP, NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, TMP13, RDX), NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, TMP1, RDX), SETCC_CONDB_DR(TMP6, TMP5),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, TMP5, TMP3), SETCC_CONDB_DR(TMP7, TMP5),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP6, TMP7), NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP6, RCX, TMP3), NOP, NOP, NOP_SEQWORD },
    /* Product: TMP0=w0, TMP2=w1, TMP4=w2, TMP5=w3, TMP6=w4 */

    /* ── PHASE A': add product to accumulator (14 triads) ── */
    /* acc[4] is in RAX, not TMP15 */
    { ADD_DSZ64_DRR(TMP0, R15, TMP0), SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R15, TMP0), NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, R9, TMP2), SETCC_CONDB_DR(TMP1, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R9, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, R10, TMP4), SETCC_CONDB_DR(TMP1, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R10, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, R13, TMP5), SETCC_CONDB_DR(TMP1, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R13, TMP0),
      NOP, NOP_SEQWORD },
    /* acc[4] in RAX */
    { ADD_DSZ64_DRR(TMP0, RAX, TMP6), SETCC_CONDB_DR(TMP1, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP14, TMP1, TMP8), ZEROEXT_DSZ64_DR(RAX, TMP0),
      NOP, NOP_SEQWORD },
    /* TMP14 = extra carry from Phase A' */

    /* ── PHASE B: m=R15(acc[0]), m×p[0,1,3], chain (9 triads) ── */
    { ZEROEXT_DSZ64_DR(RDX, R15), NOP, NOP, NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, R8, RDX), NOP, NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP7, RCX), ZEROEXT_DSZ64_DR(TMP8, RDX),
      ZEROEXT_DSZ64_DR(RDX, R15), NOP_SEQWORD },
    /* Compute p[1] = R8 >> 32 = 0xFFFFFFFF, then MUL */
    { SHR_DSZ64_DRI(TMP9, R8, 32), NOP, NOP, NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, TMP9, RDX), NOP, NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP9, RCX), ZEROEXT_DSZ64_DR(TMP3, RDX),
      ZEROEXT_DSZ64_DR(RDX, R15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RBX, RDX), NOP, NOP, NOP_SEQWORD },
    /* Chain: red[1] = hi(m*p0)+lo(m*p1), red[2] = hi(m*p1)+carry */
    { ADD_DSZ64_DRR(TMP7, TMP7, TMP3), SETCC_CONDB_DR(TMP3, TMP7),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP9, TMP9, TMP3), NOP, NOP, NOP_SEQWORD },
    /* After: TMP8=red[0], TMP7=red[1], TMP9=red[2], RDX=red[3], RCX=red[4] */

    /* ── PHASE C: add m×p to acc, shift (16 triads) ── */
    /* word 0: discard */
    { ADD_DSZ64_DRR(TMP0, R15, TMP8), SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    /* word 1 → new acc[0] → R15 */
    { ADD_DSZ64_DRR(TMP0, R9, TMP7), SETCC_CONDB_DR(TMP1, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R15, TMP0),
      NOP, NOP_SEQWORD },
    /* word 2 → new acc[1] → R9 */
    { ADD_DSZ64_DRR(TMP0, R10, TMP9), SETCC_CONDB_DR(TMP1, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R9, TMP0),
      NOP, NOP_SEQWORD },
    /* word 3 → new acc[2] → R10 */
    { ADD_DSZ64_DRR(TMP0, R13, RDX), SETCC_CONDB_DR(TMP1, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R10, TMP0),
      NOP, NOP_SEQWORD },
    /* word 4 → new acc[3] → R13 */
    { ADD_DSZ64_DRR(TMP0, RAX, RCX), SETCC_CONDB_DR(TMP1, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R13, TMP0), NOP, NOP, NOP_SEQWORD },
    /* new acc[4] → RAX */
    { ADD_DSZ64_DRR(TMP0, TMP1, TMP8), NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(RAX, TMP0, TMP14), NOP, NOP, NOP_SEQWORD },

    { NOP, NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("p256_sq: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ── fe_sq via microcode ─────────────────────────────────────── */

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    uint64_t acc[5];
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_acc asm("r15") = acc;

    asm volatile(
        "push r15\n\t"
        "push rcx\n\t"

        /* b = a for squaring → arch regs that persist across vmwrites */
        "mov rsi, [rcx]\n\t"       /* b[0] = a[0] */
        "mov r12, [rcx + 8]\n\t"   /* b[1] = a[1] */
        "mov r11, [rcx + 16]\n\t"  /* b[2] = a[2] */
        "mov r14, [rcx + 24]\n\t"  /* b[3] = a[3] */

        /* p constants */
        "mov r8, -1\n\t"           /* p[0] = all ones */
        "mov rbx, 0xffffffff00000001\n\t"  /* p[3] */
        /* p[1] computed on-the-fly in the patch as SHR(R8,32) */

        /* Zero accumulator */
        "xor r15d, r15d\n\t"       /* acc[0] */
        "xor r9d, r9d\n\t"         /* acc[1] */
        "xor r10d, r10d\n\t"       /* acc[2] */
        "xor r13d, r13d\n\t"       /* acc[3] */
        "xor eax, eax\n\t"         /* acc[4] */

        /* Iteration 0 */
        "mov rcx, [rsp]\n\t"
        "mov rdi, [rcx]\n\t"
        "vmwrite rcx, rdx\n\t"

        /* Iteration 1 */
        "mov rcx, [rsp]\n\t"
        "mov rdi, [rcx + 8]\n\t"
        "vmwrite rcx, rdx\n\t"

        /* Iteration 2 */
        "mov rcx, [rsp]\n\t"
        "mov rdi, [rcx + 16]\n\t"
        "vmwrite rcx, rdx\n\t"

        /* Iteration 3 */
        "mov rcx, [rsp]\n\t"
        "mov rdi, [rcx + 24]\n\t"
        "vmwrite rcx, rdx\n\t"

        /* Store: R15=acc[0], R9=acc[1], R10=acc[2], R13=acc[3], RAX=acc[4] */
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
#endif /* OLD SECTION */

/* ADC/SBB 64-bit + GENARITHFLAGS */
#define _ADC_DSZ64 (0x37eUL << 32)
#define _SBB_DSZ64 (0x37fUL << 32)
#define ADC_DSZ64_DRR(dst, src0, src1) ( _ADC_DSZ64 | INSTR_DRR(dst, src0, src1) )
#define CLC_OP  ( _CLC | INSTR_DRR(0, 0, 0) )

/* ── microcode (2-iter-per-vmwrite, ADC-optimized) ──────────── */

#define MONT_ITER \
    { ZEROEXT_DSZ64_DR(RDX, RDI), NOP, NOP, NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP10, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP0, RDX), ZEROEXT_DSZ64_DR(TMP1, RCX), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP11, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP2, TMP1, RDX), SETCC_CONDB_DR(TMP3, TMP2), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(RDX, RDI), NOP, NOP, NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP12, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP4, TMP1, RDX), SETCC_CONDB_DR(TMP5, TMP4), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP3), SETCC_CONDB_DR(TMP6, TMP4), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP5, TMP6), ZEROEXT_DSZ64_DR(RDX, RDI), \
      NOP, NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP13, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP5, TMP1, RDX), SETCC_CONDB_DR(TMP6, TMP5), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP5, TMP5, TMP3), SETCC_CONDB_DR(TMP7, TMP5), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP6, TMP7), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP6, RCX, TMP3), NOP, NOP, NOP_SEQWORD }, \
    /* Phase A': add product to acc (14 triads, proven working pattern) */ \
    { ADD_DSZ64_DRR(TMP0, R15, TMP0), SETCC_CONDB_DR(TMP3, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(R15, TMP0), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, R9, TMP2), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R9, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, R10, TMP4), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R10, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, R13, TMP5), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R13, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, RAX, TMP6), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP14, TMP1, TMP8), ZEROEXT_DSZ64_DR(RAX, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(RDX, R15), NOP, NOP, NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, R8, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP7, RCX), ZEROEXT_DSZ64_DR(TMP8, RDX), \
      ZEROEXT_DSZ64_DR(RDX, R15), NOP_SEQWORD }, \
    { SHR_DSZ64_DRI(TMP9, R8, 32), NOP, NOP, NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP9, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP9, RCX), ZEROEXT_DSZ64_DR(TMP3, RDX), \
      ZEROEXT_DSZ64_DR(RDX, R15), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, RBX, RDX), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP7, TMP7, TMP3), SETCC_CONDB_DR(TMP3, TMP7), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP9, TMP9, TMP3), NOP, NOP, NOP_SEQWORD }, \
    /* Phase C: add m×p to acc + shift (15 triads, proven ADD+SETCC pattern) */ \
    /* red: TMP8=red[0], TMP7=red[1], TMP9=red[2], RDX=red[3], RCX=red[4] */ \
    { ADD_DSZ64_DRR(TMP0, R15, TMP8), SETCC_CONDB_DR(TMP3, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, R9, TMP7), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R15, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, R10, TMP9), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R9, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, R13, RDX), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP3, TMP1, TMP8), ZEROEXT_DSZ64_DR(R10, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, RAX, RCX), SETCC_CONDB_DR(TMP1, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP8, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(R13, TMP0), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(TMP0, TMP1, TMP8), NOP, NOP, NOP_SEQWORD }, \
    { ADD_DSZ64_DRR(RAX, TMP0, TMP14), NOP, NOP, NOP_SEQWORD }

static void install_p256_sq_patch(void) {
    ucode_t patch[] = {
    /* PREP: reload b→TMPs, save a[i+1] from RDX→TMP15 */
    { ZEROEXT_DSZ64_DR(TMP10, RSI), ZEROEXT_DSZ64_DR(TMP11, R12),
      ZEROEXT_DSZ64_DR(TMP12, R11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R14), ZEROEXT_DSZ64_DR(TMP15, RDX),
      NOP, NOP_SEQWORD },
    MONT_ITER,       /* iteration i   (a_i in RDI) */
    { ZEROEXT_DSZ64_DR(RDI, TMP15), NOP, NOP, NOP_SEQWORD },  /* switch */
    MONT_ITER,       /* iteration i+1 (a_{i+1} now in RDI) */
    { NOP, NOP, NOP, END_SEQWORD }
    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("p256_sq: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    uint64_t acc[5];
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_acc asm("r15") = acc;

    asm volatile(
        "push r15\n\t"
        "push rcx\n\t"
        "mov rsi, [rcx]\n\t"
        "mov r12, [rcx + 8]\n\t"
        "mov r11, [rcx + 16]\n\t"
        "mov r14, [rcx + 24]\n\t"
        "mov r8, -1\n\t"
        "mov rbx, 0xffffffff00000001\n\t"
        "xor r15d, r15d\n\t"
        "xor r9d, r9d\n\t"
        "xor r10d, r10d\n\t"
        "xor r13d, r13d\n\t"
        "xor eax, eax\n\t"
        /* Iterations 0-1: a[0]→RDI, a[1]→RDX */
        "mov rcx, [rsp]\n\t"
        "mov rdi, [rcx]\n\t"
        "mov rdx, [rcx + 8]\n\t"
        "vmwrite rcx, rdx\n\t"
        /* Iterations 2-3: a[2]→RDI, a[3]→RDX */
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
        int lt = 0;
        for (int j = 3; j >= 0; j--) {
            if (out[j] < P256_P[j]) { lt = 1; break; }
            if (out[j] > P256_P[j]) break;
        }
        if (lt) break;
    }
}

static int verify_all(void) {
    int pass = 0, fail = 0;

    printf("--- Known vectors ---\n");
    struct { const char *name; uint64_t a[4]; } vecs[] = {
        { "0^2", {0,0,0,0} },
        { "1_mont^2", {1, 0xFFFFFFFF00000000ULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFEULL} },
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
    uint64_t rng = 0xA256CAFE12345678ULL;
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
    uint64_t ri[4]={1,0xFFFFFFFF00000000ULL,0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFEULL};
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

#define BATCH 1000
#define REPS  100

int main(void) {
    printf("=== P-256 Montgomery squaring: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_p256_sq_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED, skipping benchmark.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    uint64_t state[4] = {1, 0xFFFFFFFF00000000ULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFEULL};
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
    printf("Native -O3:  min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n", min/BATCH, sum/REPS/BATCH);

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
