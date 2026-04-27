/*
 * full_curve25519.c — Complete X25519 Diffie-Hellman key exchange
 *
 * Implements the full Montgomery ladder (RFC 7748) using:
 *   - Native C field arithmetic (-O3, __uint128_t)
 *   - Microcode-accelerated field arithmetic (fe_mul via vmwrite hook)
 *
 * Field: GF(2^255 - 19), unsaturated radix-2^51, 5 limbs.
 *
 * Strategy: fe_mul microcode patch handles both multiplication and squaring.
 * For squaring, we call fe_mul(a, a, out) — mathematically correct, avoids
 * needing two patches in limited patch RAM.
 *
 * Build:  make PROG=full_curve25519
 * Run:    sudo taskset -c 0 ./full_curve25519_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* ── field element type ──────────────────────────────────────────── */

typedef uint64_t fe[5];   /* 5 limbs x 51 bits */
#define MASK51 0x7FFFFFFFFFFFFULL

/* ════════════════════════════════════════════════════════════════════
 * MICROCODE PATCH INSTALLATION
 * ════════════════════════════════════════════════════════════════════ */

static void install_fe_mul_patch(void) {
    ucode_t patch[] = {

    /*
     * Register state at entry:
     *   RDI=a0  RSI=a1  R12=a2  R11=a3  R14=a4
     *   R15=b0  R13=b1  R9=b2   R10=b3  RBX=b4
     *   RAX=0   R8=0    RCX=free  RDX=free
     *
     * After PREP:
     *   TMP10=b0  TMP11=b1  TMP12=b2  TMP13=b3  TMP14=b4
     *   R13=19*b1  R9=19*b2  R10=19*b3  RBX=19*b4
     *   R15=b0 (unchanged)  RAX=0  R8=0
     */

    /* PREP: save b values, compute 19*bi */
    /* P0 */ { ZEROEXT_DSZ64_DR(TMP10, R15),
               ZEROEXT_DSZ64_DR(TMP11, R13),
               ZEROEXT_DSZ64_DR(TMP12, R9),
               NOP_SEQWORD },
    /* P1 */ { ZEROEXT_DSZ64_DR(TMP13, R10),
               ZEROEXT_DSZ64_DR(TMP14, RBX),
               NOP, NOP_SEQWORD },
    /* P2 */ { MUL_DSZ64_DIR(RCX, 19, R13),
               NOP, NOP, NOP_SEQWORD },
    /* P3 */ { MUL_DSZ64_DIR(RCX, 19, R9),
               NOP, NOP, NOP_SEQWORD },
    /* P4 */ { MUL_DSZ64_DIR(RCX, 19, R10),
               NOP, NOP, NOP_SEQWORD },
    /* P5 */ { MUL_DSZ64_DIR(RCX, 19, RBX),
               NOP, NOP, NOP_SEQWORD },

    /* LIMB c0 = a0*b0 + a1*19b4 + a2*19b3 + a3*19b2 + a4*19b1 */
    /* C0-0 */ { ZEROEXT_DSZ64_DR(TMP0, RAX),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),
                 NOP, NOP_SEQWORD },
    /* C0-1 */ { MUL_DSZ64_DRR(RCX, RDI, RDX),
                 NOP, NOP, NOP_SEQWORD },
    /* C0-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, RBX),
                 NOP_SEQWORD },
    /* C0-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, RDX),
                 NOP, NOP_SEQWORD },
    /* C0-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 ZEROEXT_DSZ64_DR(RDX, R10),
                 NOP_SEQWORD },
    /* C0-5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, RDX),
                 NOP, NOP_SEQWORD },
    /* C0-6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, R9),
                 NOP_SEQWORD },
    /* C0-7 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, RDX),
                 NOP, NOP_SEQWORD },
    /* C0-8 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* C0-9 */ { ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R14, R13),
                 NOP, NOP_SEQWORD },
    /* C0-10 */ { ADD_DSZ64_DRR(TMP2, TMP0, R13),
                  SETCC_CONDB_DR(TMP3, TMP2),
                  NOP, NOP_SEQWORD },
    /* C0-11 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                  ADD_DSZ64_DRR(TMP0, RCX, TMP3),
                  NOP, NOP_SEQWORD },
    /* C0-12 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                  ADD_DSZ64_DRR(TMP1, TMP4, TMP5),
                  ADD_DSZ64_DRR(TMP0, TMP6, TMP0),
                  NOP_SEQWORD },
    /* C0-13 */ { ADD_DSZ64_DRR(R8, R8, TMP7),
                  ADD_DSZ64_DRR(TMP0, TMP1, TMP0),
                  NOP, NOP_SEQWORD },
    /* C0-14 */ { SHR_DSZ64_DRI(R15, TMP9, 13),
                  ADD_DSZ64_DRR(R8, R8, TMP0),
                  NOP, NOP_SEQWORD },

    /* LIMB c1 = a0*b1 + a1*b0 + a2*19b4 + a3*19b3 + a4*19b2 */
    /* C1-0 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 ZEROEXT_DSZ64_DR(RDX, TMP11),
                 NOP_SEQWORD },
    /* C1-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, RDI, RDX),
                 NOP, NOP_SEQWORD },
    /* C1-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),
                 NOP_SEQWORD },
    /* C1-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, RDX),
                 NOP, NOP_SEQWORD },
    /* C1-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 ZEROEXT_DSZ64_DR(RDX, RBX),
                 NOP_SEQWORD },
    /* C1-5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, RDX),
                 NOP, NOP_SEQWORD },
    /* C1-6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, R10),
                 NOP_SEQWORD },
    /* C1-7 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, RDX),
                 NOP, NOP_SEQWORD },
    /* C1-8 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* C1-9 */ { ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R14, R9),
                 NOP, NOP_SEQWORD },
    /* C1-10 */ { ADD_DSZ64_DRR(TMP2, TMP0, R9),
                  SETCC_CONDB_DR(TMP3, TMP2),
                  NOP, NOP_SEQWORD },
    /* C1-11 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                  ADD_DSZ64_DRR(TMP0, RCX, TMP3),
                  NOP, NOP_SEQWORD },
    /* C1-12 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                  ADD_DSZ64_DRR(TMP1, TMP4, TMP5),
                  ADD_DSZ64_DRR(TMP0, TMP6, TMP0),
                  NOP_SEQWORD },
    /* C1-13 */ { ADD_DSZ64_DRR(R8, R8, TMP7),
                  ADD_DSZ64_DRR(TMP0, TMP1, TMP0),
                  NOP, NOP_SEQWORD },
    /* C1-14 */ { SHR_DSZ64_DRI(R13, TMP9, 13),
                  ADD_DSZ64_DRR(R8, R8, TMP0),
                  NOP, NOP_SEQWORD },

    /* LIMB c2 = a0*b2 + a1*b1 + a2*b0 + a3*19b4 + a4*19b3 */
    /* C2-0 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 ZEROEXT_DSZ64_DR(RDX, TMP12),
                 NOP_SEQWORD },
    /* C2-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, RDI, RDX),
                 NOP, NOP_SEQWORD },
    /* C2-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP11),
                 NOP_SEQWORD },
    /* C2-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, RDX),
                 NOP, NOP_SEQWORD },
    /* C2-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),
                 NOP_SEQWORD },
    /* C2-5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, RDX),
                 NOP, NOP_SEQWORD },
    /* C2-6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, RBX),
                 NOP_SEQWORD },
    /* C2-7 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, RDX),
                 NOP, NOP_SEQWORD },
    /* C2-8 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* C2-9 */ { ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R14, R10),
                 NOP, NOP_SEQWORD },
    /* C2-10 */ { ADD_DSZ64_DRR(TMP2, TMP0, R10),
                  SETCC_CONDB_DR(TMP3, TMP2),
                  NOP, NOP_SEQWORD },
    /* C2-11 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                  ADD_DSZ64_DRR(TMP0, RCX, TMP3),
                  NOP, NOP_SEQWORD },
    /* C2-12 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                  ADD_DSZ64_DRR(TMP1, TMP4, TMP5),
                  ADD_DSZ64_DRR(TMP0, TMP6, TMP0),
                  NOP_SEQWORD },
    /* C2-13 */ { ADD_DSZ64_DRR(R8, R8, TMP7),
                  ADD_DSZ64_DRR(TMP0, TMP1, TMP0),
                  NOP, NOP_SEQWORD },
    /* C2-14 */ { SHR_DSZ64_DRI(R9, TMP9, 13),
                  ADD_DSZ64_DRR(R8, R8, TMP0),
                  NOP, NOP_SEQWORD },

    /* LIMB c3 = a0*b3 + a1*b2 + a2*b1 + a3*b0 + a4*19b4 */
    /* C3-0 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 ZEROEXT_DSZ64_DR(RDX, TMP13),
                 NOP_SEQWORD },
    /* C3-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, RDI, RDX),
                 NOP, NOP_SEQWORD },
    /* C3-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP12),
                 NOP_SEQWORD },
    /* C3-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, RDX),
                 NOP, NOP_SEQWORD },
    /* C3-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 ZEROEXT_DSZ64_DR(RDX, TMP11),
                 NOP_SEQWORD },
    /* C3-5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, RDX),
                 NOP, NOP_SEQWORD },
    /* C3-6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),
                 NOP_SEQWORD },
    /* C3-7 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, RDX),
                 NOP, NOP_SEQWORD },
    /* C3-8 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* C3-9 */ { ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R14, RBX),
                 NOP, NOP_SEQWORD },
    /* C3-10 */ { ADD_DSZ64_DRR(TMP2, TMP0, RBX),
                  SETCC_CONDB_DR(TMP3, TMP2),
                  NOP, NOP_SEQWORD },
    /* C3-11 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                  ADD_DSZ64_DRR(TMP0, RCX, TMP3),
                  NOP, NOP_SEQWORD },
    /* C3-12 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                  ADD_DSZ64_DRR(TMP1, TMP4, TMP5),
                  ADD_DSZ64_DRR(TMP0, TMP6, TMP0),
                  NOP_SEQWORD },
    /* C3-13 */ { ADD_DSZ64_DRR(R8, R8, TMP7),
                  ADD_DSZ64_DRR(TMP0, TMP1, TMP0),
                  NOP, NOP_SEQWORD },
    /* C3-14 */ { SHR_DSZ64_DRI(R10, TMP9, 13),
                  ADD_DSZ64_DRR(R8, R8, TMP0),
                  NOP, NOP_SEQWORD },

    /* LIMB c4 = a0*b4 + a1*b3 + a2*b2 + a3*b1 + a4*b0 */
    /* C4-0 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 NOP, NOP_SEQWORD },
    /* C4-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, RDI, TMP14),
                 NOP, NOP_SEQWORD },
    /* C4-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, TMP14),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 NOP, NOP_SEQWORD },
    /* C4-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, TMP13),
                 NOP, NOP_SEQWORD },
    /* C4-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, TMP13),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* C4-5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, TMP12),
                 NOP, NOP_SEQWORD },
    /* C4-6 */ { ADD_DSZ64_DRR(TMP2, TMP0, TMP12),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 NOP, NOP_SEQWORD },
    /* C4-7 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, TMP11),
                 NOP, NOP_SEQWORD },
    /* C4-8 */ { ADD_DSZ64_DRR(TMP0, TMP2, TMP11),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* C4-9 */ { ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R14, TMP10),
                 NOP, NOP_SEQWORD },
    /* C4-10 */ { ADD_DSZ64_DRR(TMP2, TMP0, TMP10),
                  SETCC_CONDB_DR(TMP3, TMP2),
                  NOP, NOP_SEQWORD },
    /* C4-11 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                  ADD_DSZ64_DRR(TMP0, RCX, TMP3),
                  NOP, NOP_SEQWORD },
    /* C4-12 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                  ADD_DSZ64_DRR(TMP1, TMP4, TMP5),
                  ADD_DSZ64_DRR(TMP0, TMP6, TMP0),
                  NOP_SEQWORD },
    /* C4-13 */ { ADD_DSZ64_DRR(R8, R8, TMP7),
                  ADD_DSZ64_DRR(TMP0, TMP1, TMP0),
                  NOP, NOP_SEQWORD },
    /* C4-14 */ { SHR_DSZ64_DRI(RAX, TMP9, 13),
                  ADD_DSZ64_DRR(R8, R8, TMP0),
                  NOP, NOP_SEQWORD },

    /* FINAL REDUCTION */
    /* R0 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
               NOP, NOP, NOP_SEQWORD },
    /* R1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
               NOP, NOP, NOP_SEQWORD },
    /* R2 */ { MUL_DSZ64_DIR(TMP1, 19, TMP0),
               NOP, NOP, NOP_SEQWORD },
    /* R3 */ { ADD_DSZ64_DRR(R15, R15, TMP0),
               NOP, NOP, NOP_SEQWORD },
    /* R4 */ { SHR_DSZ64_DRI(TMP0, R15, 51),
               NOP, NOP, NOP_SEQWORD },
    /* R5 */ { SHL_DSZ64_DRI(TMP1, R15, 13),
               ADD_DSZ64_DRR(R13, R13, TMP0),
               NOP, NOP_SEQWORD },
    /* R6 */ { SHR_DSZ64_DRI(R15, TMP1, 13),
               NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("fe_mul patch installed: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ════════════════════════════════════════════════════════════════════
 * MICROCODE FIELD OPERATIONS
 * ════════════════════════════════════════════════════════════════════ */

static void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_b   asm("rbx") = (uint64_t *)b;
    register uint64_t *_out asm("r15") = out;

    asm volatile(
        "push r15\n\t"

        /* load a[0..4] from rcx */
        "mov rdi, [rcx]\n\t"
        "mov rsi, [rcx + 8]\n\t"
        "mov r12, [rcx + 16]\n\t"
        "mov r11, [rcx + 24]\n\t"
        "mov r14, [rcx + 32]\n\t"

        /* load b[0..4] from rbx */
        "mov r15, [rbx]\n\t"
        "mov r13, [rbx + 8]\n\t"
        "mov r9,  [rbx + 16]\n\t"
        "mov r10, [rbx + 24]\n\t"
        "mov rbx, [rbx + 32]\n\t"   /* last -- clobbers pointer */

        /* clear accumulators */
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* fire microcode */
        "vmwrite rcx, rdx\n\t"

        /* recover output pointer, store 5 result limbs */
        "pop rcx\n\t"
        "mov [rcx],      r15\n\t"
        "mov [rcx + 8],  r13\n\t"
        "mov [rcx + 16], r9\n\t"
        "mov [rcx + 24], r10\n\t"
        "mov [rcx + 32], rax\n\t"

        : "+r"(_a), "+r"(_b), "+r"(_out)
        :
        : "rax", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14",
          "memory", "cc"
    );
}

/* fe_sq via microcode: just call fe_mul(a, a, out) */
static inline void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    fe_mul_ucode(a, a, out);
}

/* ════════════════════════════════════════════════════════════════════
 * NATIVE C FIELD OPERATIONS (compiled with -O3)
 * ════════════════════════════════════════════════════════════════════ */

static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t a0=a[0], a1=a[1], a2=a[2], a3=a[3], a4=a[4];
    uint64_t b0=b[0], b1=b[1], b2=b[2], b3=b[3], b4=b[4];
    uint64_t r1=19*b1, r2=19*b2, r3=19*b3, r4=19*b4;

    __uint128_t c0 = (__uint128_t)a0*b0 + (__uint128_t)a1*r4
                   + (__uint128_t)a2*r3 + (__uint128_t)a3*r2 + (__uint128_t)a4*r1;
    __uint128_t c1 = (__uint128_t)a0*b1 + (__uint128_t)a1*b0
                   + (__uint128_t)a2*r4 + (__uint128_t)a3*r3 + (__uint128_t)a4*r2;
    __uint128_t c2 = (__uint128_t)a0*b2 + (__uint128_t)a1*b1
                   + (__uint128_t)a2*b0 + (__uint128_t)a3*r4 + (__uint128_t)a4*r3;
    __uint128_t c3 = (__uint128_t)a0*b3 + (__uint128_t)a1*b2
                   + (__uint128_t)a2*b1 + (__uint128_t)a3*b0 + (__uint128_t)a4*r4;
    __uint128_t c4 = (__uint128_t)a0*b4 + (__uint128_t)a1*b3
                   + (__uint128_t)a2*b2 + (__uint128_t)a3*b1 + (__uint128_t)a4*b0;

    uint64_t carry;
    carry = (uint64_t)(c0>>51); out[0] = (uint64_t)c0 & MASK51;
    c1 += carry;
    carry = (uint64_t)(c1>>51); out[1] = (uint64_t)c1 & MASK51;
    c2 += carry;
    carry = (uint64_t)(c2>>51); out[2] = (uint64_t)c2 & MASK51;
    c3 += carry;
    carry = (uint64_t)(c3>>51); out[3] = (uint64_t)c3 & MASK51;
    c4 += carry;
    carry = (uint64_t)(c4>>51); out[4] = (uint64_t)c4 & MASK51;
    out[0] += carry * 19;
    carry = out[0] >> 51; out[0] &= MASK51;
    out[1] += carry;
}

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    uint64_t a0=a[0], a1=a[1], a2=a[2], a3=a[3], a4=a[4];
    uint64_t d0=2*a0, d1=2*a1, d2=2*a2, d3=2*a3;
    uint64_t r3=19*a3, r4=19*a4;

    __uint128_t c0 = (__uint128_t)a0*a0 + (__uint128_t)d1*r4 + (__uint128_t)d2*r3;
    __uint128_t c1 = (__uint128_t)d0*a1 + (__uint128_t)r3*a3 + (__uint128_t)d2*r4;
    __uint128_t c2 = (__uint128_t)d0*a2 + (__uint128_t)a1*a1 + (__uint128_t)d3*r4;
    __uint128_t c3 = (__uint128_t)d0*a3 + (__uint128_t)d1*a2 + (__uint128_t)r4*a4;
    __uint128_t c4 = (__uint128_t)d0*a4 + (__uint128_t)d1*a3 + (__uint128_t)a2*a2;

    uint64_t carry;
    carry = (uint64_t)(c0>>51); out[0] = (uint64_t)c0 & MASK51;
    c1 += carry;
    carry = (uint64_t)(c1>>51); out[1] = (uint64_t)c1 & MASK51;
    c2 += carry;
    carry = (uint64_t)(c2>>51); out[2] = (uint64_t)c2 & MASK51;
    c3 += carry;
    carry = (uint64_t)(c3>>51); out[3] = (uint64_t)c3 & MASK51;
    c4 += carry;
    carry = (uint64_t)(c4>>51); out[4] = (uint64_t)c4 & MASK51;
    out[0] += carry * 19;
    carry = out[0] >> 51; out[0] &= MASK51;
    out[1] += carry;
}

/* ════════════════════════════════════════════════════════════════════
 * COMMON FIELD OPERATIONS (pure C, used by both native and ucode)
 * ════════════════════════════════════════════════════════════════════ */

static inline void fe_add(fe out, const fe a, const fe b) {
    out[0] = a[0] + b[0];
    out[1] = a[1] + b[1];
    out[2] = a[2] + b[2];
    out[3] = a[3] + b[3];
    out[4] = a[4] + b[4];
}

/*
 * fe_sub: out = a - b, with bias to keep limbs positive.
 * We add 2*p to a before subtracting b.
 * 2*p = 2*(2^255 - 19) in 5-limb representation:
 *   limb 0: 2*(2^51 - 19) = 0xFFFFFFFFFFFFDA
 *   limbs 1-4: 2*(2^51 - 1) = 0xFFFFFFFFFFFFFE = 2*MASK51
 */
static inline void fe_sub(fe out, const fe a, const fe b) {
    out[0] = (a[0] + 0xFFFFFFFFFFFDAULL) - b[0];
    out[1] = (a[1] + 0xFFFFFFFFFFFFEULL) - b[1];
    out[2] = (a[2] + 0xFFFFFFFFFFFFEULL) - b[2];
    out[3] = (a[3] + 0xFFFFFFFFFFFFEULL) - b[3];
    out[4] = (a[4] + 0xFFFFFFFFFFFFEULL) - b[4];
}

/* fe_mul121665: out = a * 121665 */
static void fe_mul121665(fe out, const fe a) {
    __uint128_t c;
    c = (__uint128_t)a[0] * 121665;
    out[0] = (uint64_t)c & MASK51; c >>= 51;
    c += (__uint128_t)a[1] * 121665;
    out[1] = (uint64_t)c & MASK51; c >>= 51;
    c += (__uint128_t)a[2] * 121665;
    out[2] = (uint64_t)c & MASK51; c >>= 51;
    c += (__uint128_t)a[3] * 121665;
    out[3] = (uint64_t)c & MASK51; c >>= 51;
    c += (__uint128_t)a[4] * 121665;
    out[4] = (uint64_t)c & MASK51; c >>= 51;
    out[0] += (uint64_t)c * 19;
    uint64_t carry = out[0] >> 51;
    out[0] &= MASK51;
    out[1] += carry;
}

/* constant-time conditional swap */
static void fe_cswap(fe a, fe b, uint64_t swap) {
    swap = (uint64_t)(-(int64_t)swap);  /* 0 or 0xFFFF...FFFF */
    for (int i = 0; i < 5; i++) {
        uint64_t x = (a[i] ^ b[i]) & swap;
        a[i] ^= x;
        b[i] ^= x;
    }
}

static inline void fe_copy(fe out, const fe a) {
    memcpy(out, a, 5 * sizeof(uint64_t));
}

/* fe_reduce: full reduction mod p = 2^255 - 19 */
static void fe_reduce(fe out, const fe a) {
    uint64_t t[5];
    memcpy(t, a, 40);

    /* first, propagate carries */
    uint64_t carry;
    carry = t[0] >> 51; t[0] &= MASK51; t[1] += carry;
    carry = t[1] >> 51; t[1] &= MASK51; t[2] += carry;
    carry = t[2] >> 51; t[2] &= MASK51; t[3] += carry;
    carry = t[3] >> 51; t[3] &= MASK51; t[4] += carry;
    carry = t[4] >> 51; t[4] &= MASK51; t[0] += carry * 19;
    carry = t[0] >> 51; t[0] &= MASK51; t[1] += carry;

    /* now t is in [0, 2^255-1]. subtract p if t >= p */
    /* compute t - p; if no borrow, use the result */
    uint64_t s[5];
    int64_t borrow;
    s[0] = t[0] - (MASK51 - 18);  /* p limb 0 = 2^51 - 19 */
    borrow = (int64_t)s[0] >> 63;
    s[1] = t[1] - MASK51 + borrow;
    borrow = (int64_t)s[1] >> 63;
    s[2] = t[2] - MASK51 + borrow;
    borrow = (int64_t)s[2] >> 63;
    s[3] = t[3] - MASK51 + borrow;
    borrow = (int64_t)s[3] >> 63;
    s[4] = t[4] - MASK51 + borrow;
    borrow = (int64_t)s[4] >> 63;

    /* if borrow == 0, s >= 0, so t >= p: use s. else use t. */
    uint64_t mask = (uint64_t)borrow;  /* 0 if t>=p, 0xFFFF... if t<p */
    for (int i = 0; i < 5; i++) {
        out[i] = (t[i] & mask) | (s[i] & ~mask);
        out[i] &= MASK51;
    }
}

/* ════════════════════════════════════════════════════════════════════
 * BYTE ENCODING / DECODING (little-endian, 256-bit)
 * ════════════════════════════════════════════════════════════════════ */

static void fe_frombytes(fe out, const uint8_t in[32]) {
    uint64_t t[5];
    /* read 256 bits as a little-endian integer, split into 51-bit limbs */
    t[0]  = ((uint64_t)in[0])
           | ((uint64_t)in[1] << 8)
           | ((uint64_t)in[2] << 16)
           | ((uint64_t)in[3] << 24)
           | ((uint64_t)in[4] << 32)
           | ((uint64_t)in[5] << 40)
           | ((uint64_t)(in[6] & 0x07) << 48);
    t[1]  = ((uint64_t)in[6] >> 3)
           | ((uint64_t)in[7] << 5)
           | ((uint64_t)in[8] << 13)
           | ((uint64_t)in[9] << 21)
           | ((uint64_t)in[10] << 29)
           | ((uint64_t)in[11] << 37)
           | ((uint64_t)(in[12] & 0x3f) << 45);
    t[2]  = ((uint64_t)in[12] >> 6)
           | ((uint64_t)in[13] << 2)
           | ((uint64_t)in[14] << 10)
           | ((uint64_t)in[15] << 18)
           | ((uint64_t)in[16] << 26)
           | ((uint64_t)in[17] << 34)
           | ((uint64_t)in[18] << 42)
           | ((uint64_t)(in[19] & 0x01) << 50);
    t[3]  = ((uint64_t)in[19] >> 1)
           | ((uint64_t)in[20] << 7)
           | ((uint64_t)in[21] << 15)
           | ((uint64_t)in[22] << 23)
           | ((uint64_t)in[23] << 31)
           | ((uint64_t)in[24] << 39)
           | ((uint64_t)(in[25] & 0x0f) << 47);
    t[4]  = ((uint64_t)in[25] >> 4)
           | ((uint64_t)in[26] << 4)
           | ((uint64_t)in[27] << 12)
           | ((uint64_t)in[28] << 20)
           | ((uint64_t)in[29] << 28)
           | ((uint64_t)in[30] << 36)
           | ((uint64_t)(in[31] & 0x7f) << 44);  /* clear bit 255 */

    memcpy(out, t, 40);
}

static void fe_tobytes(uint8_t out[32], const fe in) {
    fe t;
    fe_reduce(t, in);

    /* pack 5 x 51-bit limbs into 32 bytes, little-endian */
    /* We have 255 bits: limbs 0..4, each 51 bits */
    /* Total bits: 5 * 51 = 255 */
    uint64_t h0 = t[0], h1 = t[1], h2 = t[2], h3 = t[3], h4 = t[4];

    /* Combine into a 256-bit number (bit 255 = 0) */
    /* limb 0: bits 0..50 */
    /* limb 1: bits 51..101 */
    /* limb 2: bits 102..152 */
    /* limb 3: bits 153..203 */
    /* limb 4: bits 204..254 */

    out[0]  = (uint8_t)(h0);
    out[1]  = (uint8_t)(h0 >> 8);
    out[2]  = (uint8_t)(h0 >> 16);
    out[3]  = (uint8_t)(h0 >> 24);
    out[4]  = (uint8_t)(h0 >> 32);
    out[5]  = (uint8_t)(h0 >> 40);
    out[6]  = (uint8_t)((h0 >> 48) | (h1 << 3));
    out[7]  = (uint8_t)(h1 >> 5);
    out[8]  = (uint8_t)(h1 >> 13);
    out[9]  = (uint8_t)(h1 >> 21);
    out[10] = (uint8_t)(h1 >> 29);
    out[11] = (uint8_t)(h1 >> 37);
    out[12] = (uint8_t)((h1 >> 45) | (h2 << 6));
    out[13] = (uint8_t)(h2 >> 2);
    out[14] = (uint8_t)(h2 >> 10);
    out[15] = (uint8_t)(h2 >> 18);
    out[16] = (uint8_t)(h2 >> 26);
    out[17] = (uint8_t)(h2 >> 34);
    out[18] = (uint8_t)(h2 >> 42);
    out[19] = (uint8_t)((h2 >> 50) | (h3 << 1));
    out[20] = (uint8_t)(h3 >> 7);
    out[21] = (uint8_t)(h3 >> 15);
    out[22] = (uint8_t)(h3 >> 23);
    out[23] = (uint8_t)(h3 >> 31);
    out[24] = (uint8_t)(h3 >> 39);
    out[25] = (uint8_t)((h3 >> 47) | (h4 << 4));
    out[26] = (uint8_t)(h4 >> 4);
    out[27] = (uint8_t)(h4 >> 12);
    out[28] = (uint8_t)(h4 >> 20);
    out[29] = (uint8_t)(h4 >> 28);
    out[30] = (uint8_t)(h4 >> 36);
    out[31] = (uint8_t)(h4 >> 44);
}

/* ════════════════════════════════════════════════════════════════════
 * FIELD INVERSION via Fermat's little theorem: a^(p-2) mod p
 * p - 2 = 2^255 - 21
 *
 * Standard addition chain from donna/ref10.
 * ════════════════════════════════════════════════════════════════════ */

/* fe_invert using native C field operations */
static void fe_invert_native(fe out, const fe z) {
    fe z2, z9, z11, t, t0, t1, t2, t3;
    int i;

    /* Standard addition chain for p-2 = 2^255 - 21 */
    fe_sq_native(z, z2);                     /* z^2 */
    fe_sq_native(z2, t);                     /* z^4 */
    fe_sq_native(t, t);                      /* z^8 */
    fe_mul_native(t, z, z9);                 /* z^9 */
    fe_mul_native(z9, z2, z11);              /* z^11 */
    fe_sq_native(z11, t);                    /* z^22 */
    fe_mul_native(t, z9, t0);                /* z^31 = z^(2^5-1) */

    fe_sq_native(t0, t1);
    for (i = 1; i < 5; i++) fe_sq_native(t1, t1);
    fe_mul_native(t1, t0, t1);               /* z^(2^10-1) */

    fe_sq_native(t1, t2);
    for (i = 1; i < 10; i++) fe_sq_native(t2, t2);
    fe_mul_native(t2, t1, t2);               /* z^(2^20-1) */

    fe_sq_native(t2, t3);
    for (i = 1; i < 20; i++) fe_sq_native(t3, t3);
    fe_mul_native(t3, t2, t3);               /* z^(2^40-1) */

    for (i = 0; i < 10; i++) fe_sq_native(t3, t3);
    fe_mul_native(t3, t1, t1);               /* z^(2^50-1) */

    fe_sq_native(t1, t2);
    for (i = 1; i < 50; i++) fe_sq_native(t2, t2);
    fe_mul_native(t2, t1, t2);               /* z^(2^100-1) */

    fe_sq_native(t2, t3);
    for (i = 1; i < 100; i++) fe_sq_native(t3, t3);
    fe_mul_native(t3, t2, t3);               /* z^(2^200-1) */

    for (i = 0; i < 50; i++) fe_sq_native(t3, t3);
    fe_mul_native(t3, t1, t1);               /* z^(2^250-1) */

    fe_sq_native(t1, t1);                    /* z^(2^251-2) */
    fe_sq_native(t1, t1);                    /* z^(2^252-4) */
    fe_sq_native(t1, t1);                    /* z^(2^253-8) */
    fe_sq_native(t1, t1);                    /* z^(2^254-16) */
    fe_sq_native(t1, t1);                    /* z^(2^255-32) */
    fe_mul_native(t1, z11, out);             /* z^(2^255-21) = z^(p-2) */
}

/* fe_invert using microcode field operations */
static void fe_invert_ucode(fe out, const fe z) {
    fe z2, z9, z11, t, t0, t1, t2, t3;
    int i;

    fe_sq_ucode(z, z2);
    fe_sq_ucode(z2, t);
    fe_sq_ucode(t, t);
    fe_mul_ucode(z, t, z9);
    fe_mul_ucode(z9, z2, z11);
    fe_sq_ucode(z11, t);
    fe_mul_ucode(z9, t, t0);

    fe_sq_ucode(t0, t1);
    for (i = 1; i < 5; i++) fe_sq_ucode(t1, t1);
    fe_mul_ucode(t0, t1, t1);

    fe_sq_ucode(t1, t2);
    for (i = 1; i < 10; i++) fe_sq_ucode(t2, t2);
    fe_mul_ucode(t1, t2, t2);

    fe_sq_ucode(t2, t3);
    for (i = 1; i < 20; i++) fe_sq_ucode(t3, t3);
    fe_mul_ucode(t2, t3, t3);

    for (i = 0; i < 10; i++) fe_sq_ucode(t3, t3);
    fe_mul_ucode(t1, t3, t1);

    fe_sq_ucode(t1, t2);
    for (i = 1; i < 50; i++) fe_sq_ucode(t2, t2);
    fe_mul_ucode(t1, t2, t2);

    fe_sq_ucode(t2, t3);
    for (i = 1; i < 100; i++) fe_sq_ucode(t3, t3);
    fe_mul_ucode(t2, t3, t3);

    for (i = 0; i < 50; i++) fe_sq_ucode(t3, t3);
    fe_mul_ucode(t1, t3, t1);

    fe_sq_ucode(t1, t1);
    fe_sq_ucode(t1, t1);
    fe_sq_ucode(t1, t1);
    fe_sq_ucode(t1, t1);
    fe_sq_ucode(t1, t1);
    fe_mul_ucode(z11, t1, out);
}

/* ════════════════════════════════════════════════════════════════════
 * X25519 MONTGOMERY LADDER (RFC 7748)
 * ════════════════════════════════════════════════════════════════════ */

static void scalar_clamp(uint8_t s[32]) {
    s[0]  &= 248;   /* clear bits 0, 1, 2 */
    s[31] &= 127;   /* clear bit 255 */
    s[31] |= 64;    /* set bit 254 */
}

/*
 * x25519_native: native C implementation of the full X25519 function.
 * scalar: 32-byte secret key (will be clamped)
 * point:  32-byte u-coordinate of the base point
 * out:    32-byte result
 */
static void x25519_native(uint8_t out[32], const uint8_t scalar[32],
                           const uint8_t point[32]) {
    uint8_t e[32];
    memcpy(e, scalar, 32);
    scalar_clamp(e);

    fe x1, x2, z2, x3, z3;
    fe A, AA, B, BB, E, C, D, DA, CB, t0;

    fe_frombytes(x1, point);
    fe_copy(x2, (const uint64_t[]){1,0,0,0,0});
    memset(z2, 0, sizeof(fe));
    fe_copy(x3, x1);
    fe_copy(z3, (const uint64_t[]){1,0,0,0,0});

    uint64_t swap = 0;

    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (e[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(A, x2, z2);
        fe_sq_native(A, AA);
        fe_sub(B, x2, z2);
        fe_sq_native(B, BB);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul_native(D, A, DA);
        fe_mul_native(C, B, CB);

        fe_add(t0, DA, CB);
        fe_sq_native(t0, x3);

        fe_sub(t0, DA, CB);
        fe_sq_native(t0, z3);
        fe_mul_native(x1, z3, z3);

        fe_mul_native(AA, BB, x2);

        fe_mul121665(t0, E);
        fe_add(t0, AA, t0);
        fe_mul_native(E, t0, z2);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert_native(z2, z2);
    fe_mul_native(x2, z2, x2);
    fe_tobytes(out, x2);
}

/*
 * x25519_ucode: microcode-accelerated X25519.
 * Uses fe_mul_ucode for all multiplications and squarings.
 */
static void x25519_ucode(uint8_t out[32], const uint8_t scalar[32],
                          const uint8_t point[32]) {
    uint8_t e[32];
    memcpy(e, scalar, 32);
    scalar_clamp(e);

    fe x1, x2, z2, x3, z3;
    fe A, AA, B, BB, E, C, D, DA, CB, t0;

    fe_frombytes(x1, point);
    fe_copy(x2, (const uint64_t[]){1,0,0,0,0});
    memset(z2, 0, sizeof(fe));
    fe_copy(x3, x1);
    fe_copy(z3, (const uint64_t[]){1,0,0,0,0});

    uint64_t swap = 0;

    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (e[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(A, x2, z2);
        fe_sq_ucode(A, AA);
        fe_sub(B, x2, z2);
        fe_sq_ucode(B, BB);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul_ucode(D, A, DA);
        fe_mul_ucode(C, B, CB);

        fe_add(t0, DA, CB);
        fe_sq_ucode(t0, x3);

        fe_sub(t0, DA, CB);
        fe_sq_ucode(t0, z3);
        fe_mul_ucode(x1, z3, z3);

        fe_mul_ucode(AA, BB, x2);

        fe_mul121665(t0, E);
        fe_add(t0, AA, t0);
        fe_mul_ucode(E, t0, z2);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert_ucode(z2, z2);
    fe_mul_ucode(x2, z2, x2);
    fe_tobytes(out, x2);
}

/* ════════════════════════════════════════════════════════════════════
 * UTILITY FUNCTIONS
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

/* ════════════════════════════════════════════════════════════════════
 * RFC 7748 TEST VECTORS
 * ════════════════════════════════════════════════════════════════════ */

static int test_rfc7748(void) {
    int pass = 0, fail = 0;
    uint8_t scalar[32], point[32], result_native[32], result_ucode[32];

    printf("=== RFC 7748 Test Vectors ===\n\n");

    /* --- Test vector 1 --- */
    printf("--- Test vector 1 ---\n");
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
                 scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
                 point, 32);
    x25519_native(result_native, scalar, point);
    x25519_ucode(result_ucode, scalar, point);

    print_hex("native", result_native, 32);
    print_hex("ucode ", result_ucode, 32);

    if (memcmp_hex(result_native,
                   "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552",
                   32) == 0) {
        printf("  native: PASS\n"); pass++;
    } else {
        printf("  native: FAIL\n"); fail++;
    }
    if (memcmp_hex(result_ucode,
                   "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552",
                   32) == 0) {
        printf("  ucode:  PASS\n"); pass++;
    } else {
        printf("  ucode:  FAIL\n"); fail++;
    }

    /* --- Test vector 2 --- */
    printf("\n--- Test vector 2 ---\n");
    hex_to_bytes("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
                 scalar, 32);
    hex_to_bytes("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
                 point, 32);
    x25519_native(result_native, scalar, point);
    x25519_ucode(result_ucode, scalar, point);

    print_hex("native", result_native, 32);
    print_hex("ucode ", result_ucode, 32);

    if (memcmp_hex(result_native,
                   "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957",
                   32) == 0) {
        printf("  native: PASS\n"); pass++;
    } else {
        printf("  native: FAIL\n"); fail++;
    }
    if (memcmp_hex(result_ucode,
                   "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957",
                   32) == 0) {
        printf("  ucode:  PASS\n"); pass++;
    } else {
        printf("  ucode:  FAIL\n"); fail++;
    }

    /* --- Iterated test: 1 iteration --- */
    printf("\n--- Iterated test (1 iteration) ---\n");
    {
        /* Start: k = u = basepoint (9) */
        uint8_t k[32] = {0}, u[32] = {0}, r[32];
        k[0] = 9;
        u[0] = 9;

        x25519_native(r, k, u);

        print_hex("native after 1", r, 32);
        if (memcmp_hex(r,
                       "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079",
                       32) == 0) {
            printf("  native: PASS\n"); pass++;
        } else {
            printf("  native: FAIL\n"); fail++;
        }

        /* Also test ucode */
        x25519_ucode(r, k, u);
        print_hex("ucode  after 1", r, 32);
        if (memcmp_hex(r,
                       "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079",
                       32) == 0) {
            printf("  ucode:  PASS\n"); pass++;
        } else {
            printf("  ucode:  FAIL\n"); fail++;
        }
    }

    /* --- Iterated test: 1000 iterations --- */
    printf("\n--- Iterated test (1000 iterations) ---\n");
    {
        uint8_t k[32] = {0}, u[32] = {0}, r[32];
        k[0] = 9;
        u[0] = 9;

        /* native */
        uint8_t kn[32], un[32];
        memcpy(kn, k, 32);
        memcpy(un, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_native(r, kn, un);
            memcpy(un, kn, 32);
            memcpy(kn, r, 32);
        }
        print_hex("native after 1000", kn, 32);
        if (memcmp_hex(kn,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  native: PASS\n"); pass++;
        } else {
            printf("  native: FAIL\n"); fail++;
        }

        /* ucode */
        uint8_t ku[32], uu[32];
        memcpy(ku, k, 32);
        memcpy(uu, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_ucode(r, ku, uu);
            memcpy(uu, ku, 32);
            memcpy(ku, r, 32);
        }
        print_hex("ucode  after 1000", ku, 32);
        if (memcmp_hex(ku,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  ucode:  PASS\n"); pass++;
        } else {
            printf("  ucode:  FAIL\n"); fail++;
        }

        /* Cross-check: native == ucode */
        if (memcmp(kn, ku, 32) == 0) {
            printf("  native==ucode: PASS\n"); pass++;
        } else {
            printf("  native==ucode: FAIL\n"); fail++;
        }
    }

    printf("\n=== RFC 7748: %d passed, %d failed ===\n\n", pass, fail);
    return fail;
}

/* ════════════════════════════════════════════════════════════════════
 * BENCHMARKING
 * ════════════════════════════════════════════════════════════════════ */

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

#define BENCH_REPS 50

static void benchmark(void) {
    uint8_t scalar[32] = {0}, point[32] = {0}, out[32];
    uint64_t t0, t1, min, sum;

    /* Use the RFC test vector 1 scalar and basepoint for benchmarking */
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
                 scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
                 point, 32);

    printf("=== X25519 Benchmark (%d repetitions) ===\n\n", BENCH_REPS);

    /* Warm up */
    x25519_native(out, scalar, point);
    x25519_ucode(out, scalar, point);

    /* Benchmark native C */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < BENCH_REPS; r++) {
        t0 = rdtsc_start();
        x25519_native(out, scalar, point);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt;
        if (dt < min) min = dt;
    }
    printf("Native C (-O3):     min %8" PRIu64 "  avg %8" PRIu64 " cycles\n",
           min, sum / BENCH_REPS);

    /* Benchmark microcode */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < BENCH_REPS; r++) {
        t0 = rdtsc_start();
        x25519_ucode(out, scalar, point);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt;
        if (dt < min) min = dt;
    }
    printf("Microcode (vmwrite): min %8" PRIu64 "  avg %8" PRIu64 " cycles\n",
           min, sum / BENCH_REPS);

    printf("\n");
}

/* ════════════════════════════════════════════════════════════════════
 * MAIN
 * ════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("=== Full X25519: microcode vs native C ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_fe_mul_patch();

    /* Quick diagnostic: verify fe_invert and encode/decode */
    {
        printf("--- Diagnostics ---\n");
        fe x = {7, 0, 0, 0, 0};
        fe inv_x, product;
        fe_invert_native(inv_x, x);
        fe_mul_native(x, inv_x, product);
        fe_reduce(product, product);
        printf("  inv(7)*7 = {%lu,%lu,%lu,%lu,%lu} (expect {1,0,0,0,0})\n",
               product[0], product[1], product[2], product[3], product[4]);

        /* Verify encode/decode roundtrip */
        uint8_t bytes[32];
        fe decoded;
        fe orig = {0x123456789ABULL, 0x23456789ABCULL, 0x3456789ABCDULL,
                   0x456789ABCDEULL, 0x56789ABCDEFULL};
        fe_tobytes(bytes, orig);
        fe_frombytes(decoded, bytes);
        printf("  encode/decode: {%lx,%lx,%lx,%lx,%lx} → {%lx,%lx,%lx,%lx,%lx} %s\n",
               orig[0], orig[1], orig[2], orig[3], orig[4],
               decoded[0], decoded[1], decoded[2], decoded[3], decoded[4],
               memcmp(orig, decoded, 40) == 0 ? "MATCH" : "MISMATCH");

        /* Verify single ladder step: 9 * 9 (basepoint * basepoint scalar) */
        uint8_t sc9[32] = {0}, bp9[32] = {0}, result9[32];
        sc9[0] = 9; bp9[0] = 9;
        x25519_native(result9, sc9, bp9);
        printf("  x25519(9, 9) = ");
        for (int i = 0; i < 32; i++) printf("%02x", result9[i]);
        printf("\n  (expect 422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079)\n");
        printf("\n");
    }

    /* Run RFC 7748 test vectors */
    int failures = test_rfc7748();

    if (failures) {
        printf("Test vectors FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    /* Benchmark */
    benchmark();

    /* Clean up: remove patches */
    init_match_and_patch();
    do_fix_IN_patch();
    printf("Done.\n");
    return 0;
}
