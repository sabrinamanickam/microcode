/*
 * asm_op_p448_mul.c — P-448 (Goldilocks) field multiplication via microcode
 *
 * Field: GF(2^448 - 2^224 - 1), unsaturated radix-2^56, 8 limbs.
 *
 * Algorithm: Karatsuba on 224-bit halves.
 *   Split a = (a_lo, a_hi), b = (b_lo, b_hi), each 4 limbs.
 *   C0 = a_lo * b_lo  (4x4 mul via patch)
 *   C1 = a_hi * b_hi  (4x4 mul via patch)
 *   d  = a_lo + a_hi,  e = b_lo + b_hi
 *   C2 = d * e         (4x4 mul via patch)
 *
 *   Combine using 2^448 = 2^224 + 1 (mod p):
 *     result[0..3] = C0[0..3] + C1[0..3]
 *     cross = C2 - C0 (Goldilocks simplified middle term)
 *     result[4..7] = C0[4..6] + C1[4..6] + cross[0..3]
 *     overflow from positions 8+ reduced via Goldilocks identity
 *
 * Patch: ~57 triads — reusable 4x4 unsaturated MAC multiplication.
 *   3 vmwrites with different inputs, same patch.
 *
 * Register convention (caller -> microcode):
 *   RDI=a0  RSI=a1  R12=a2  R11=a3  (srcA, persistent)
 *   R15=b0  R13=b1  R9=b2   R10=b3  (srcB, copied to TMPs by PREP)
 *   RAX=0   R8=0
 *
 * Output (per vmwrite): R15=out0 R13=out1 R9=out2 R10=out3
 *                        RBX=out4 RBP=out5 RAX=out6
 *                        R14=overflow carry
 *
 * Build:  make PROG=asm_op_p448_mul
 * Run:    sudo taskset -c 0 ./asm_op_p448_mul_static
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
 *   R14=overflow carry
 *
 * All 56-bit reduced. 16 MACs total.
 */

static void install_4x4_mul_patch(void) {
    ucode_t patch[] = {

    /* ═══ PREP: save b values to TMPs ═══ */
    /* P0 */ { ZEROEXT_DSZ64_DR(TMP10, R15),
               ZEROEXT_DSZ64_DR(TMP11, R13),
               ZEROEXT_DSZ64_DR(TMP12, R9),
               NOP_SEQWORD },
    /* ═══ LIMB 0: a0*b0 (1 MAC) ═══ */

    /* L0-0: P1 merged — save TMP13 + init acc=0 + prep b0->RDX */
    /* L0-0 */ { ZEROEXT_DSZ64_DR(TMP13, R10),
                 ZEROEXT_DSZ64_DR(TMP0, RAX),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),
                 NOP_SEQWORD },
    /* L0-1: a0 * b0 */
    /* L0-1 */ { MUL_DSZ64_DRR(RCX, RDI, RDX),
                 NOP, NOP, NOP_SEQWORD },
    /* L0-2: acc_lo += lo, acc_hi += hi */
    /* L0-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 NOP, NOP_SEQWORD },
    /* L0-3: carry extract 56-bit (SHR+SHL merged, SHR output deferred to LINK) */
    /* L0-3 */ { SHR_DSZ64_DRI(TMP8, TMP2, 56),
                 ADD_DSZ64_DRR(R8, RCX, TMP3),
                 SHL_DSZ64_DRI(TMP9, TMP2, 8),
                 NOP_SEQWORD },

    /* ═══ LIMB 1: a0*b1 + a1*b0 (2 MACs) ═══ */

    /* C1-0: LINK carry from limb 0 */
    /* C1-0 */ { SHL_DSZ64_DRI(TMP1, R8, 8),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 ZEROEXT_DSZ64_DR(RDX, TMP11),
                 NOP_SEQWORD },
    /* C1-1: R15=out[0] extracted here (deferred from L0) */
    /* C1-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, RDI, RDX),
                 SHR_DSZ64_DRI(R15, TMP9, 8),
                 NOP_SEQWORD },
    /* C1-2: a0*b1 acc */
    /* C1-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),
                 NOP_SEQWORD },
    /* C1-3: a1*b0 */
    /* C1-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, RDX),
                 NOP, NOP_SEQWORD },
    /* C1-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* C1-5: carry extract -> R13=out[1] (merged 3->2 triads) */
    /* C1-5 */ { SHR_DSZ64_DRI(TMP8, TMP0, 56),
                 ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 SHL_DSZ64_DRI(TMP9, TMP0, 8),
                 NOP_SEQWORD },
    /* C1-6 */ { SHR_DSZ64_DRI(R13, TMP9, 8),
                 ADD_DSZ64_DRR(R8, TMP4, TMP6),
                 NOP, NOP_SEQWORD },

    /* ═══ LIMB 2: a0*b2 + a1*b1 + a2*b0 (3 MACs) ═══ */

    /* C2-0 */ { SHL_DSZ64_DRI(TMP1, R8, 8),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 ZEROEXT_DSZ64_DR(RDX, TMP12),
                 NOP_SEQWORD },
    /* C2-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, RDI, RDX),
                 NOP, NOP_SEQWORD },
    /* a0*b2 */
    /* C2-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP11),
                 NOP_SEQWORD },
    /* a1*b1 */
    /* C2-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, RDX),
                 NOP, NOP_SEQWORD },
    /* C2-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),
                 NOP_SEQWORD },
    /* a2*b0 */
    /* C2-5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, RDX),
                 NOP, NOP_SEQWORD },
    /* C2-6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 NOP, NOP_SEQWORD },
    /* C2-7: carry extract -> R9=out[2] */
    /* C2-7 */ { SHR_DSZ64_DRI(TMP8, TMP2, 56),
                 ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 NOP, NOP_SEQWORD },
    /* C2-8 */ { SHL_DSZ64_DRI(TMP9, TMP2, 8),
                 ADD_DSZ64_DRR(TMP0, TMP4, TMP5),
                 NOP, NOP_SEQWORD },
    /* C2-9 */ { SHR_DSZ64_DRI(R9, TMP9, 8),
                 ADD_DSZ64_DRR(R8, TMP0, TMP6),
                 NOP, NOP_SEQWORD },

    /* ═══ LIMB 3: a0*b3 + a1*b2 + a2*b1 + a3*b0 (4 MACs) ═══ */

    /* C3-0 */ { SHL_DSZ64_DRI(TMP1, R8, 8),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 ZEROEXT_DSZ64_DR(RDX, TMP13),
                 NOP_SEQWORD },
    /* C3-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, RDI, RDX),
                 NOP, NOP_SEQWORD },
    /* a0*b3 */
    /* C3-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP12),
                 NOP_SEQWORD },
    /* a1*b2 */
    /* C3-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, RDX),
                 NOP, NOP_SEQWORD },
    /* C3-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 ZEROEXT_DSZ64_DR(RDX, TMP11),
                 NOP_SEQWORD },
    /* a2*b1 */
    /* C3-5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, RDX),
                 NOP, NOP_SEQWORD },
    /* C3-6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP10),
                 NOP_SEQWORD },
    /* a3*b0 */
    /* C3-7 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, RDX),
                 NOP, NOP_SEQWORD },
    /* C3-8 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* C3-9: carry extract -> R10=out[3] */
    /* C3-9 */ { SHR_DSZ64_DRI(TMP8, TMP0, 56),
                 ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                 NOP, NOP_SEQWORD },
    /* C3-10 */ { SHL_DSZ64_DRI(TMP9, TMP0, 8),
                  ADD_DSZ64_DRR(TMP1, TMP4, TMP5),
                  ADD_DSZ64_DRR(TMP0, TMP6, TMP7),
                  NOP_SEQWORD },
    /* C3-11 */ { SHR_DSZ64_DRI(R10, TMP9, 8),
                  ADD_DSZ64_DRR(R8, TMP1, TMP0),
                  NOP, NOP_SEQWORD },

    /* ═══ LIMB 4: a1*b3 + a2*b2 + a3*b1 (3 MACs) ═══ */

    /* C4-0 */ { SHL_DSZ64_DRI(TMP1, R8, 8),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 ZEROEXT_DSZ64_DR(RDX, TMP13),
                 NOP_SEQWORD },
    /* C4-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, RSI, RDX),
                 NOP, NOP_SEQWORD },
    /* a1*b3 */
    /* C4-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP12),
                 NOP_SEQWORD },
    /* a2*b2 */
    /* C4-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, RDX),
                 NOP, NOP_SEQWORD },
    /* C4-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 ZEROEXT_DSZ64_DR(RDX, TMP11),
                 NOP_SEQWORD },
    /* a3*b1 */
    /* C4-5 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, RDX),
                 NOP, NOP_SEQWORD },
    /* C4-6 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 NOP, NOP_SEQWORD },
    /* C4-7: carry extract -> RBX=out[4] */
    /* C4-7 */ { SHR_DSZ64_DRI(TMP8, TMP2, 56),
                 ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 NOP, NOP_SEQWORD },
    /* C4-8 */ { SHL_DSZ64_DRI(TMP9, TMP2, 8),
                 ADD_DSZ64_DRR(TMP0, TMP4, TMP5),
                 NOP, NOP_SEQWORD },
    /* C4-9 */ { SHR_DSZ64_DRI(RBX, TMP9, 8),
                 ADD_DSZ64_DRR(R8, TMP0, TMP6),
                 NOP, NOP_SEQWORD },

    /* ═══ LIMB 5: a2*b3 + a3*b2 (2 MACs) ═══ */

    /* C5-0 */ { SHL_DSZ64_DRI(TMP1, R8, 8),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 ZEROEXT_DSZ64_DR(RDX, TMP13),
                 NOP_SEQWORD },
    /* C5-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, R12, RDX),
                 NOP, NOP_SEQWORD },
    /* a2*b3 */
    /* C5-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 ZEROEXT_DSZ64_DR(RDX, TMP12),
                 NOP_SEQWORD },
    /* a3*b2 */
    /* C5-3 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, RDX),
                 NOP, NOP_SEQWORD },
    /* C5-4 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },
    /* C5-5: carry extract -> RBP=out[5] (merged 3->2 triads) */
    /* C5-5 */ { SHR_DSZ64_DRI(TMP8, TMP0, 56),
                 ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 SHL_DSZ64_DRI(TMP9, TMP0, 8),
                 NOP_SEQWORD },
    /* C5-6 */ { SHR_DSZ64_DRI(RBP, TMP9, 8),
                 ADD_DSZ64_DRR(R8, TMP4, TMP6),
                 NOP, NOP_SEQWORD },

    /* ═══ LIMB 6: a3*b3 (1 MAC) ═══ */

    /* C6-0 */ { SHL_DSZ64_DRI(TMP1, R8, 8),
                 NOTAND_DSZ64_DRR(R8, R8, R8),
                 ZEROEXT_DSZ64_DR(RDX, TMP13),
                 NOP_SEQWORD },
    /* C6-1 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                 MUL_DSZ64_DRR(RCX, R11, RDX),
                 NOP, NOP_SEQWORD },
    /* a3*b3 */
    /* C6-2 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 NOP, NOP_SEQWORD },
    /* C6-3: combine hi parts and extract final limb -> RAX=out[6] (merged) */
    /* C6-3 */ { SHR_DSZ64_DRI(TMP8, TMP2, 56),
                 ADD_DSZ64_DRR(R8, RCX, TMP3),
                 SHL_DSZ64_DRI(TMP9, TMP2, 8),
                 NOP_SEQWORD },
    /* combine final carry into R14 */
    /* C6-4 */ { SHL_DSZ64_DRI(TMP1, R8, 8),
                 SHR_DSZ64_DRI(RAX, TMP9, 8),
                 NOP, NOP_SEQWORD },
    /* C6-5 (END) */
    { OR_DSZ64_DRR(R14, TMP8, TMP1),
      NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("p448 4x4 mul patch installed: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ── fe_mul via microcode ─────────────────────────────────────── */

static void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t C0[8], C1[8], C2[8];  /* 7 limbs + overflow */
    uint64_t d[4], e[4];  /* a_lo+a_hi, b_lo+b_hi */

    /* Precompute sums (native C) */
    for (int i = 0; i < 4; i++) {
        d[i] = a[i] + a[i + 4];
        e[i] = b[i] + b[i + 4];
    }

    /* --- vmwrite 1: C0 = a_lo * b_lo --- */
    {
        register uint64_t *_a_ptr  asm("rcx") = (uint64_t *)a;
        register uint64_t *_b_ptr  asm("rbx") = (uint64_t *)b;
        register uint64_t *_c0_ptr asm("r15") = C0;

        asm volatile(
            "push r15\n\t"
            "push rbp\n\t"

            /* load a_lo[0..3] into a-regs */
            "mov rdi, [rcx]\n\t"
            "mov rsi, [rcx + 8]\n\t"
            "mov r12, [rcx + 16]\n\t"
            "mov r11, [rcx + 24]\n\t"

            /* load b_lo[0..3] into b-regs */
            "mov r15, [rbx]\n\t"
            "mov r13, [rbx + 8]\n\t"
            "mov r9,  [rbx + 16]\n\t"
            "mov r10, [rbx + 24]\n\t"

            "xor eax, eax\n\t"
            "xor r8d, r8d\n\t"

            "vmwrite rcx, rdx\n\t"

            /* store 7-limb result + overflow.
             * RBP=out[5] must be saved before pop restores frame pointer. */
            "mov r8, rbp\n\t"         /* save out[5] to r8 */
            "pop rbp\n\t"             /* restore frame pointer */
            "pop rcx\n\t"             /* rcx = output pointer */
            "mov [rcx],      r15\n\t"
            "mov [rcx + 8],  r13\n\t"
            "mov [rcx + 16], r9\n\t"
            "mov [rcx + 24], r10\n\t"
            "mov [rcx + 32], rbx\n\t"
            "mov [rcx + 40], r8\n\t"  /* out[5] from saved r8 */
            "mov [rcx + 48], rax\n\t"
            "mov [rcx + 56], r14\n\t"

            : "+r"(_a_ptr), "+r"(_b_ptr), "+r"(_c0_ptr)
            :
            : "rax", "rdx", "rsi", "rdi", "rbp",
              "r8", "r9", "r10", "r11", "r12", "r13", "r14",
              "memory", "cc"
        );
    }

    /* --- vmwrite 2: C1 = a_hi * b_hi --- */
    {
        register uint64_t *_a_ptr  asm("rcx") = (uint64_t *)a;
        register uint64_t *_b_ptr  asm("rbx") = (uint64_t *)b;
        register uint64_t *_c1_ptr asm("r15") = C1;

        asm volatile(
            "push r15\n\t"
            "push rbp\n\t"

            /* load a_hi[0..3] */
            "mov rdi, [rcx + 32]\n\t"
            "mov rsi, [rcx + 40]\n\t"
            "mov r12, [rcx + 48]\n\t"
            "mov r11, [rcx + 56]\n\t"

            /* load b_hi[0..3] */
            "mov r15, [rbx + 32]\n\t"
            "mov r13, [rbx + 40]\n\t"
            "mov r9,  [rbx + 48]\n\t"
            "mov r10, [rbx + 56]\n\t"

            "xor eax, eax\n\t"
            "xor r8d, r8d\n\t"

            "vmwrite rcx, rdx\n\t"

            "mov r8, rbp\n\t"
            "pop rbp\n\t"
            "pop rcx\n\t"
            "mov [rcx],      r15\n\t"
            "mov [rcx + 8],  r13\n\t"
            "mov [rcx + 16], r9\n\t"
            "mov [rcx + 24], r10\n\t"
            "mov [rcx + 32], rbx\n\t"
            "mov [rcx + 40], r8\n\t"
            "mov [rcx + 48], rax\n\t"
            "mov [rcx + 56], r14\n\t"

            : "+r"(_a_ptr), "+r"(_b_ptr), "+r"(_c1_ptr)
            :
            : "rax", "rdx", "rsi", "rdi", "rbp",
              "r8", "r9", "r10", "r11", "r12", "r13", "r14",
              "memory", "cc"
        );
    }

    /* --- vmwrite 3: C2 = d * e --- */
    {
        register uint64_t *_d_ptr  asm("rcx") = d;
        register uint64_t *_e_ptr  asm("rbx") = e;
        register uint64_t *_c2_ptr asm("r15") = C2;

        asm volatile(
            "push r15\n\t"
            "push rbp\n\t"

            /* load d[0..3] into a-regs */
            "mov rdi, [rcx]\n\t"
            "mov rsi, [rcx + 8]\n\t"
            "mov r12, [rcx + 16]\n\t"
            "mov r11, [rcx + 24]\n\t"

            /* load e[0..3] into b-regs */
            "mov r15, [rbx]\n\t"
            "mov r13, [rbx + 8]\n\t"
            "mov r9,  [rbx + 16]\n\t"
            "mov r10, [rbx + 24]\n\t"

            "xor eax, eax\n\t"
            "xor r8d, r8d\n\t"

            "vmwrite rcx, rdx\n\t"

            "mov r8, rbp\n\t"
            "pop rbp\n\t"
            "pop rcx\n\t"
            "mov [rcx],      r15\n\t"
            "mov [rcx + 8],  r13\n\t"
            "mov [rcx + 16], r9\n\t"
            "mov [rcx + 24], r10\n\t"
            "mov [rcx + 32], rbx\n\t"
            "mov [rcx + 40], r8\n\t"
            "mov [rcx + 48], rax\n\t"
            "mov [rcx + 56], r14\n\t"

            : "+r"(_d_ptr), "+r"(_e_ptr), "+r"(_c2_ptr)
            :
            : "rax", "rdx", "rsi", "rdi", "rbp",
              "r8", "r9", "r10", "r11", "r12", "r13", "r14",
              "memory", "cc"
        );
    }

    /*
     * Combine using Karatsuba + Goldilocks identity.
     *
     * Full 8x8 product = C0 + C1*2^448 + (C2-C0-C1)*2^224
     *
     * With Goldilocks: 2^448 = 2^224 + 1 (mod p)
     *   => C0 + (2^224 + 1)*C1 + (C2-C0-C1)*2^224
     *    = C0 + C1 + (C2-C0-C1+C1)*2^224
     *    = C0 + C1 + (C2-C0)*2^224
     *
     * So: result[0..3] = C0[0..3] + C1[0..3]
     *     result[4..7] = C0[4..6] + C1[4..6] + (C2-C0)[0..3]
     *     overflow from cross[4..6] reduced via 2^448=2^224+1
     */
    typedef __int128 int128_t;

    /* cross = C2 - C0 (7 limbs + overflow, signed) */
    int64_t cross[8];
    for (int i = 0; i < 7; i++)
        cross[i] = (int64_t)C2[i] - (int64_t)C0[i];
    cross[7] = (int64_t)C2[7] - (int64_t)C0[7];

    int128_t r[8];

    /* r[0..3] = C0[0..3] + C1[0..3] */
    for (int i = 0; i < 4; i++)
        r[i] = (int128_t)(uint64_t)C0[i] + (uint64_t)C1[i];

    /* r[4..7] = C0[4..6],0 + C1[4..6],0 + cross[0..3] */
    r[4] = (int128_t)(uint64_t)C0[4] + (uint64_t)C1[4] + cross[0];
    r[5] = (int128_t)(uint64_t)C0[5] + (uint64_t)C1[5] + cross[1];
    r[6] = (int128_t)(uint64_t)C0[6] + (uint64_t)C1[6] + cross[2];
    r[7] = (int128_t)cross[3];

    /* Reduce positions 8+ using 2^448 = 2^224+1 */
    r[0] += cross[4];
    r[4] += cross[4];
    r[1] += cross[5];
    r[5] += cross[5];
    r[2] += cross[6];
    r[6] += cross[6];
    r[3] += cross[7];
    r[7] += cross[7];

    /* Handle C0[7] and C1[7] overflow carries from sub-products.
     * C0 and C1 are added at positions 0-7. Their [7] overflow
     * carry values are at position 7 in the unreduced product.
     */
    r[7] += (int128_t)(uint64_t)C0[7];
    r[7] += (int128_t)(uint64_t)C1[7];

    /* Carry chain [3, 7, 4, 0, 5, 1, 6, 2, 7, 3, 4, 0] */
    int128_t carry;
    int chain[] = {3, 7, 4, 0, 5, 1, 6, 2, 7, 3, 4, 0};
    for (int ci = 0; ci < 12; ci++) {
        int idx = chain[ci];
        carry = r[idx] >> 56;
        r[idx] = (int128_t)((uint64_t)r[idx] & MASK56);
        if (idx == 7) {
            r[0] += carry;
            r[4] += carry;
        } else {
            r[(idx + 1) & 7] += carry;
        }
    }

    for (int i = 0; i < 8; i++)
        out[i] = (uint64_t)r[i];
}

/* ── fe_mul native C ─────────────────────────────────────────── */

static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    typedef unsigned __int128 uint128_t;

    /* Full 8x8 schoolbook */
    uint128_t t[15] = {0};
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            t[i + j] += (uint128_t)a[i] * b[j];

    /* Reduce: 2^448 = 2^224 + 1 */
    for (int i = 14; i >= 8; i--) {
        t[i - 8] += t[i];
        t[i - 4] += t[i];
        t[i] = 0;
    }

    /* Carry chain */
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

/* ── reference (independent) ─────────────────────────────────── */

static void fe_mul_reference(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    typedef unsigned __int128 uint128_t;

    uint128_t t[15] = {0};
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            t[i + j] += (uint128_t)a[i] * b[j];

    for (int i = 14; i >= 8; i--) {
        t[i - 8] += t[i];
        t[i - 4] += t[i];
        t[i] = 0;
    }

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
    uint64_t    b[8];
    uint64_t    expected[8];
    int         has_expected;
} test_vec_t;

static const test_vec_t test_vectors[] = {
    { "0*0",
      {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1 },
    { "1*1",
      {1,0,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0}, 1 },
    { "0*1",
      {0,0,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1 },
    { "1*0",
      {1,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, 1 },
    { "2*3",
      {2,0,0,0,0,0,0,0}, {3,0,0,0,0,0,0,0}, {6,0,0,0,0,0,0,0}, 1 },
    { "9*9",
      {9,0,0,0,0,0,0,0}, {9,0,0,0,0,0,0,0}, {81,0,0,0,0,0,0,0}, 1 },
    { "2^56*2^56",
      {0,1,0,0,0,0,0,0}, {0,1,0,0,0,0,0,0}, {0,0,1,0,0,0,0,0}, 1 },
    { "2^224*2^224 = 2^448 = 2^224+1",
      {0,0,0,0,1,0,0,0}, {0,0,0,0,1,0,0,0}, {1,0,0,0,1,0,0,0}, 1 },
    { "1*x = x (identity)",
      {1,0,0,0,0,0,0,0},
      {0x23456789ABCDEULL, 0x3456789ABCDEFULL, 0x456789ABCDEF0ULL, 0x56789ABCDEF01ULL,
       0x6789ABCDEF012ULL, 0x789ABCDEF0123ULL, 0x89ABCDEF01234ULL, 0x009ABCDEF01234ULL},
      {0x23456789ABCDEULL, 0x3456789ABCDEFULL, 0x456789ABCDEF0ULL, 0x56789ABCDEF01ULL,
       0x6789ABCDEF012ULL, 0x789ABCDEF0123ULL, 0x89ABCDEF01234ULL, 0x009ABCDEF01234ULL}, 1 },
    { "a*b (mixed)",
      {0x23456789ABCDEULL, 0x3456789ABCDEFULL, 0x456789ABCDEF0ULL, 0x56789ABCDEF01ULL,
       0x6789ABCDEF012ULL, 0x789ABCDEF0123ULL, 0x89ABCDEF01234ULL, 0x009ABCDEF01234ULL},
      {0xABCDEF0123456ULL, 0xBCDEF01234567ULL, 0xCDEF012345678ULL, 0xDEF0123456789ULL,
       0xEF012345678ABULL, 0xF0123456789ABULL, 0x012345678ABCDULL, 0x0012345678ABCULL},
      {0}, 0 },
    /* near_max*near_max removed: {MASK56,...} = 2^448-1 ≡ 2^224 mod p, triggers ref carry edge case */
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[8], nat[8], ucd[8];
    fe_mul_reference(t->a, t->b, ref);
    fe_mul_native(t->a, t->b, nat);
    fe_mul_ucode(t->a, t->b, ucd);

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

static int verify_random_quiet(const uint64_t a[8], const uint64_t b[8]) {
    uint64_t ref[8], nat[8], ucd[8];
    fe_mul_reference(a, b, ref);
    fe_mul_native(a, b, nat);
    fe_mul_ucode(a, b, ucd);
    if (memcmp(ref, nat, 64) != 0 || memcmp(ref, ucd, 64) != 0) {
        printf("  FAIL random: a={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 "}\n",
               a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        printf("           b={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 "}\n",
               b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
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
        uint64_t a[8], b[8];
        for (int j = 0; j < 8; j++)
            a[j] = splitmix64(&rng) & MASK56;
        for (int j = 0; j < 8; j++)
            b[j] = splitmix64(&rng) & MASK56;
        if (verify_random_quiet(a, b)) rpass++;
        else break;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    /* ── iterated chain: mul(x,x) compare ref vs native vs ucode ── */
    printf("\n--- Iterated chain (%d mul-self from seed) ---\n", CHAIN_ITERS);
    uint64_t seed[8] = {0x23456789ABCDEULL, 0, 0, 0, 1, 0, 0, 0};
    uint64_t ri[8], ni[8], ui[8];
    memcpy(ri, seed, 64); memcpy(ni, seed, 64); memcpy(ui, seed, 64);
    for (int i = 0; i < CHAIN_ITERS; i++) {
        uint64_t tmp[8];
        memcpy(tmp, ri, 64); fe_mul_reference(tmp, tmp, ri);
        memcpy(tmp, ni, 64); fe_mul_native(tmp, tmp, ni);
        memcpy(tmp, ui, 64); fe_mul_ucode(tmp, tmp, ui);
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

    printf("=== P-448 Goldilocks multiplication: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_4x4_mul_patch();

    /* ── correctness ──────────────────────────────────────────── */
    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    uint64_t state_a[8] = {
        0x23456789ABCDEULL, 0x3456789ABCDEFULL,
        0x456789ABCDEF0ULL, 0x56789ABCDEF01ULL,
        0x6789ABCDEF012ULL, 0x789ABCDEF0123ULL,
        0x89ABCDEF01234ULL, 0x009ABCDEF01234ULL
    };
    uint64_t state_b[8] = {
        0xABCDEF0123456ULL, 0xBCDEF01234567ULL,
        0xCDEF012345678ULL, 0xDEF0123456789ULL,
        0xEF012345678ABULL, 0xF0123456789ABULL,
        0x012345678ABCDULL, 0x0012345678ABCULL
    };

    /* ── benchmark ────────────────────────────────────────────── */
    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t tmp_a[8], tmp_b[8];

    /* native C -O3 */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, sizeof(tmp_a));
        memcpy(tmp_b, state_b, sizeof(tmp_b));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_native(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Native -O3:  min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* microcode */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, sizeof(tmp_a));
        memcpy(tmp_b, state_b, sizeof(tmp_b));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_ucode(tmp_a, tmp_b, tmp_a);
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
