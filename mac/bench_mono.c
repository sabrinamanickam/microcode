/*
 * bench_mono.c — Monolithic Curve25519 fe_sq: all 15 MACs in ONE vmwrite
 *
 * Strategy: entire field squaring in microcode — 5 limbs, carry propagation,
 * and final reduction all execute as one uninterrupted microcode sequence.
 * Only 1 vmwrite redirect overhead (~5 cy) instead of 5 (~25 cy).
 *
 * Register convention (caller → microcode):
 *   RDI = a0          RSI = a1          R12 = a2
 *   R11 = a3          R14 = a4          R15 = d0 = 2*a0
 *   R13 = d1 = 2*a1   R9  = d2 = 2*a2   R10 = d3 = 2*a3
 *   RBX = r4 = 19*a4  RDX = r3 = 19*a3
 *   RAX = 0 (acc_lo)  R8  = 0 (acc_hi)  RCX = trigger
 *
 * Output (microcode → caller):
 *   RDI = out[0]  R9  = out[1]  R10 = out[2]
 *   RBX = out[3]  RAX = out[4]
 *
 * MUL convention: MUL_DSZ64_DRR(dst, src0, src1) → hi→dst, lo→src1
 * SETCC constraint: slot 2 must be NOP when SETCC in slot 1
 * Shift constraint: shifts placed in slot 0 (safe for single-shifter ports)
 *
 * Build: make PROG=bench_mono
 * Run:   sudo taskset -c 0 ./bench_mono_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL

/* ══════════════════════════════════════════════════════════════════
 *  Monolithic fe_sq patch — ~59 triads
 *
 *  Per-limb structure (9 triads MAC + 2 triads carry transition):
 *    T0: ZEROEXT(TMP0, acc) + MUL pair1
 *    T1: ADD(TMP2, TMP0, p1_lo) + SETCC_CONDB(TMP3, TMP2) + NOP
 *    T2: ADD(TMP4, RCX, TMP3) + MUL pair2
 *    T3: ADD(TMP0, TMP2, p2_lo) + SETCC(TMP3, TMP0) + NOP
 *    T4: ADD(TMP5, RCX, TMP3) + MUL pair3
 *    T5: ADD(TMP2, TMP0, p3_lo) + SETCC(TMP3, TMP2) + NOP
 *    T6: SHR(TMP8, TMP2, 51) + ADD(TMP6, RCX, TMP3)     [carry overlap]
 *    T7: SHL(TMP9, TMP2, 13) + ADD(R8, R8, TMP4) + ADD(TMP0, TMP5, TMP6)
 *    T8: SHR(out, TMP9, 13)  + ADD(R8, R8, TMP0)         [limb masked]
 *   Transition:
 *    T9:  SHL(TMP1, R8, 13) + NOTAND(R8, R8, R8) + [optional copy]
 *    T10: OR(TMP0, TMP8, TMP1) + MUL(next pair1)  + [optional recompute]
 * ══════════════════════════════════════════════════════════════════ */
static void install_mono_fesq(void) {
    ucode_t mono_patch[] = {

    /* ════════════════════════════════════════════════════════════
     *  LIMB c0 = a0*a0 + d1*r4 + d2*r3
     *  MUL1: (RDI, RDI)  a0*a0     lo→RDI   [a0 clobbered, only used here]
     *  MUL2: (RBX, R13)  r4*d1     lo→R13   [d1 clobbered, recompute later]
     *  MUL3: (RDX, R9)   r3*d2     lo→R9    [d2 clobbered, recompute later]
     * ════════════════════════════════════════════════════════════ */

    /* c0 T0: save acc(=0), MUL a0*a0 */
    { ZEROEXT_DSZ64_DR(TMP0, RAX),
      MUL_DSZ64_DRR(RCX, RDI, RDI),
      NOP, NOP_SEQWORD },

    /* c0 T1: acc += lo(a0*a0), carry1 */
    { ADD_DSZ64_DRR(TMP2, TMP0, RDI),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },

    /* c0 T2: hi1+c1, MUL r4*d1 */
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R13),
      NOP, NOP_SEQWORD },

    /* c0 T3: acc += lo(r4*d1), carry2 */
    { ADD_DSZ64_DRR(TMP0, TMP2, R13),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },

    /* c0 T4: hi2+c2, MUL r3*d2 */
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RDX, R9),
      NOP, NOP_SEQWORD },

    /* c0 T5: acc += lo(r3*d2), carry3 */
    { ADD_DSZ64_DRR(TMP2, TMP0, R9),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },

    /* c0 T6: hi3+c3 + start carry extraction (lo>>51) */
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },

    /* c0 T7: lo<<13 + hi accumulation */
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },

    /* c0 T8: out[0]→RDI (masked) + finalize R8 */
    { SHR_DSZ64_DRI(RDI, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ── c0→c1 transition ────────────────────────────────────
     *  Copy a1(RSI)→R13 for MUL, recompute d2=2*a2 in R9 */

    /* T9: carry_hi + reset R8 + copy a1 for c1 MUL1 */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(R13, RSI),
      NOP_SEQWORD },

    /* T10: merge carry + MUL d0*a1_copy + recompute d2 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, R13),
      ADD_DSZ64_DRR(R9, R12, R12),
      NOP_SEQWORD },

    /* ════════════════════════════════════════════════════════════
     *  LIMB c1 = d0*a1 + r3*a3 + d2*r4
     *  MUL1: (R15, R13)  d0*a1_copy lo→R13  [d0 survives, a1(RSI) safe]
     *  MUL2: (R11, RDX)  a3*r3      lo→RDX  [r3 clobbered, last use in c1]
     *  MUL3: (RBX, R9)   r4*d2      lo→R9   [d2 clobbered, done after c1]
     * ════════════════════════════════════════════════════════════ */

    /* c1 T1: acc += lo(d0*a1), carry1 */
    { ADD_DSZ64_DRR(TMP2, TMP0, R13),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },

    /* c1 T2: hi1+c1, MUL a3*r3 */
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      NOP, NOP_SEQWORD },

    /* c1 T3: acc += lo(a3*r3), carry2 */
    { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },

    /* c1 T4: hi2+c2, MUL r4*d2 */
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R9),
      NOP, NOP_SEQWORD },

    /* c1 T5: acc += lo(r4*d2), carry3 */
    { ADD_DSZ64_DRR(TMP2, TMP0, R9),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },

    /* c1 T6: carry overlap */
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },

    /* c1 T7 */
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },

    /* c1 T8: out[1]→R9 */
    { SHR_DSZ64_DRI(R9, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ── c1→c2 transition ────────────────────────────────────
     *  Copy a2(R12)→RDX for c2 MUL1, copy a1(RSI)→R13 for c2 MUL2 */

    /* T9: carry_hi + reset R8 + copy a2 */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(RDX, R12),
      NOP_SEQWORD },

    /* T10: merge carry + MUL d0*a2_copy + copy a1 for a1*a1 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, RDX),
      ZEROEXT_DSZ64_DR(R13, RSI),
      NOP_SEQWORD },

    /* ════════════════════════════════════════════════════════════
     *  LIMB c2 = d0*a2 + a1*a1 + d3*r4
     *  MUL1: (R15, RDX)  d0*a2_copy lo→RDX [d0 survives, a2(R12) safe]
     *  MUL2: (RSI, R13)  a1*a1_copy lo→R13 [a1(RSI) survives for d1 recompute]
     *  MUL3: (RBX, R10)  r4*d3      lo→R10 [d3 clobbered, only in c2]
     * ════════════════════════════════════════════════════════════ */

    /* c2 T1 */
    { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },

    /* c2 T2: hi1+c1, MUL a1*a1_copy */
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RSI, R13),
      NOP, NOP_SEQWORD },

    /* c2 T3 */
    { ADD_DSZ64_DRR(TMP0, TMP2, R13),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },

    /* c2 T4: hi2+c2, MUL r4*d3 */
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R10),
      NOP, NOP_SEQWORD },

    /* c2 T5 */
    { ADD_DSZ64_DRR(TMP2, TMP0, R10),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },

    /* c2 T6 */
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },

    /* c2 T7 */
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },

    /* c2 T8: out[2]→R10 */
    { SHR_DSZ64_DRI(R10, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ── c2→c3 transition ────────────────────────────────────
     *  Recompute d1=2*a1 into R13 (from RSI, still live).
     *  Copy a3(R11)→RDX for c3 MUL1. */

    /* T9: carry_hi + reset R8 + copy a3 */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(RDX, R11),
      NOP_SEQWORD },

    /* T10: merge carry + MUL d0*a3_copy + recompute d1 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, RDX),
      ADD_DSZ64_DRR(R13, RSI, RSI),
      NOP_SEQWORD },

    /* ════════════════════════════════════════════════════════════
     *  LIMB c3 = d0*a3 + d1*a2 + r4*a4
     *  MUL1: (R15, RDX)  d0*a3_copy  lo→RDX [d0 survives, a3(R11) safe]
     *  MUL2: (R13, RSI)  d1*a2_copy  lo→RSI [d1(R13) survives for c4]
     *     NB: a2(R12) survives, but we use RSI (freed) as a2 copy
     *  MUL3: (R14, RBX)  a4*r4       lo→RBX [r4 last use, a4 survives]
     *
     *  Extra triad needed: copy a2(R12)→RSI before MUL2
     * ════════════════════════════════════════════════════════════ */

    /* c3 T1 */
    { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },

    /* c3 T1.5: copy a2→RSI for MUL2 (a1 no longer needed, RSI freed) */
    { ZEROEXT_DSZ64_DR(RSI, R12),
      ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      NOP, NOP_SEQWORD },

    /* c3 T2: MUL d1*a2_copy → lo→RSI */
    { NOP,
      MUL_DSZ64_DRR(RCX, R13, RSI),
      NOP, NOP_SEQWORD },

    /* c3 T3 */
    { ADD_DSZ64_DRR(TMP0, TMP2, RSI),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },

    /* c3 T4: hi2+c2, MUL a4*r4 (r4 last use) */
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R14, RBX),
      NOP, NOP_SEQWORD },

    /* c3 T5 */
    { ADD_DSZ64_DRR(TMP2, TMP0, RBX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },

    /* c3 T6 */
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },

    /* c3 T7 */
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },

    /* c3 T8: out[3]→RBX */
    { SHR_DSZ64_DRI(RBX, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ── c3→c4 transition ────────────────────────────────────
     *  c4 MUL1: d0(R15)*a4(R14) — both last use, no copies needed
     *  c4 MUL2: d1(R13)*a3(R11) — both last use
     *  c4 MUL3: a2(R12)*a2(R12) — last use */

    /* T9: carry_hi + reset R8 */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      NOP, NOP_SEQWORD },

    /* T10: merge carry + MUL d0*a4 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, R14),
      NOP, NOP_SEQWORD },

    /* ════════════════════════════════════════════════════════════
     *  LIMB c4 = d0*a4 + d1*a3 + a2*a2
     *  MUL1: (R15, R14)  d0*a4  lo→R14 [both last use]
     *  MUL2: (R13, R11)  d1*a3  lo→R11 [both last use]
     *  MUL3: (R12, R12)  a2*a2  lo→R12 [last use]
     * ════════════════════════════════════════════════════════════ */

    /* c4 T1 */
    { ADD_DSZ64_DRR(TMP2, TMP0, R14),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },

    /* c4 T2: hi1+c1, MUL d1*a3 */
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R13, R11),
      NOP, NOP_SEQWORD },

    /* c4 T3 */
    { ADD_DSZ64_DRR(TMP0, TMP2, R11),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },

    /* c4 T4: hi2+c2, MUL a2*a2 */
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R12, R12),
      NOP, NOP_SEQWORD },

    /* c4 T5 */
    { ADD_DSZ64_DRR(TMP2, TMP0, R12),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },

    /* c4 T6 */
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },

    /* c4 T7 */
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },

    /* c4 T8: out[4]→RAX (masked) + finalize R8 */
    { SHR_DSZ64_DRI(RAX, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ════════════════════════════════════════════════════════════
     *  FINAL REDUCTION: carry4 * 19 → out[0], then carry → out[1]
     *
     *  carry4 = (R8 << 13) | (TMP2 >> 51) = (R8<<13) | TMP8
     *  19 = 16 + 2 + 1
     * ════════════════════════════════════════════════════════════ */

    /* carry4 = (R8<<13) | TMP8 */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOP, NOP, NOP_SEQWORD },

    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      NOP, NOP, NOP_SEQWORD },

    /* TMP0 = carry4.  Compute carry4 * 19 = carry4*16 + carry4*2 + carry4 */
    { SHL_DSZ64_DRI(TMP1, TMP0, 4),
      NOP, NOP, NOP_SEQWORD },

    /* TMP2 = 2*carry, TMP1 += carry → 17*carry */
    { SHL_DSZ64_DRI(TMP2, TMP0, 1),
      ADD_DSZ64_DRR(TMP1, TMP1, TMP0),
      NOP, NOP_SEQWORD },

    /* TMP1 = 17*carry + 2*carry = 19*carry */
    { ADD_DSZ64_DRR(TMP1, TMP1, TMP2),
      NOP, NOP, NOP_SEQWORD },

    /* out[0] += 19*carry */
    { ADD_DSZ64_DRR(RDI, RDI, TMP1),
      NOP, NOP, NOP_SEQWORD },

    /* Final carry from out[0] → out[1] */
    { SHR_DSZ64_DRI(TMP0, RDI, 51),
      NOP, NOP, NOP_SEQWORD },

    /* out[0] &= MASK51 (SHL 13, SHR 13), out[1] += final carry */
    { SHL_DSZ64_DRI(TMP1, RDI, 13),
      ADD_DSZ64_DRR(R9, R9, TMP0),
      NOP, NOP_SEQWORD },

    { SHR_DSZ64_DRI(RDI, TMP1, 13),
      NOP, NOP, END_SEQWORD }

    }; /* end mono_patch */

    printf("Monolithic fe_sq patch: %zu triads\n", ARRAY_SZ(mono_patch));

    assign_to_core(0);
    do_fix_IN_patch();
    patch_ucode(0x7c00, mono_patch, ARRAY_SZ(mono_patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("Hook installed.\n");
}


/* ══════════════════════════════════════════════════════════════════
 *  Caller: x86 asm that sets up registers and invokes vmwrite
 * ══════════════════════════════════════════════════════════════════ */
__attribute__((noinline))
static void fe_sq_mono(const uint64_t *a, uint64_t *out) {
    /* Save pointers where asm can reach them — we clobber ALL regs */
    uint64_t a_ptr = (uint64_t)a, out_ptr = (uint64_t)out;
    asm volatile(
        /* ── Precompute and load all registers ────────────── */
        /* Load input pointer, save a[] to stack (handles a==out) */
        "mov rax, %[ap]\n\t"
        "sub rsp, 56\n\t"
        "mov rcx, %[op]\n\t"
        "mov [rsp+40], rcx\n\t"          /* save out_ptr */

        "mov rcx, [rax]\n\t"
        "mov [rsp],    rcx\n\t"          /* a0 */
        "mov rcx, [rax+8]\n\t"
        "mov [rsp+8],  rcx\n\t"          /* a1 */
        "mov rcx, [rax+16]\n\t"
        "mov [rsp+16], rcx\n\t"          /* a2 */
        "mov rcx, [rax+24]\n\t"
        "mov [rsp+24], rcx\n\t"          /* a3 */
        "mov rcx, [rax+32]\n\t"
        "mov [rsp+32], rcx\n\t"          /* a4 */

        /* Load base values */
        "mov rdi, [rsp]\n\t"             /* RDI = a0 */
        "mov rsi, [rsp+8]\n\t"           /* RSI = a1 */
        "mov r12, [rsp+16]\n\t"          /* R12 = a2 */
        "mov r11, [rsp+24]\n\t"          /* R11 = a3 */
        "mov r14, [rsp+32]\n\t"          /* R14 = a4 */

        /* Precompute doubled values */
        "lea r15, [rdi+rdi]\n\t"          /* R15 = d0 = 2*a0 */
        "lea r13, [rsi+rsi]\n\t"          /* R13 = d1 = 2*a1 */
        "lea r9,  [r12+r12]\n\t"          /* R9  = d2 = 2*a2 */
        "lea r10, [r11+r11]\n\t"          /* R10 = d3 = 2*a3 */

        /* Precompute reduced values */
        "imul rbx, r14, 19\n\t"           /* RBX = r4 = 19*a4 */
        "imul rdx, r11, 19\n\t"           /* RDX = r3 = 19*a3 */

        /* Accumulator starts at 0 */
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* ── Single vmwrite: entire fe_sq in microcode ──── */
        "vmwrite rcx, rdx\n\t"

        /* ── Read results from registers ─────────────────── */
        /* RDI=out[0], R9=out[1], R10=out[2], RBX=out[3], RAX=out[4] */
        "mov rcx, [rsp+40]\n\t"          /* restore out_ptr */
        "mov [rcx],    rdi\n\t"
        "mov [rcx+8],  r9\n\t"
        "mov [rcx+16], r10\n\t"
        "mov [rcx+24], rbx\n\t"
        "mov [rcx+32], rax\n\t"

        "add rsp, 56\n\t"
        :
        : [ap] "m"(a_ptr), [op] "m"(out_ptr)
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory"
    );
}


/* ══════════════════════════════════════════════════════════════════
 *  Reference C — native field square (same as bench_3mac.c)
 * ══════════════════════════════════════════════════════════════════ */
__attribute__((noinline))
static void fe_sq_ref(const uint64_t *a, uint64_t *out) {
    uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
    uint64_t d0 = 2*a0, d1 = 2*a1, d2 = 2*a2, d3 = 2*a3;
    uint64_t r3 = 19*a3, r4 = 19*a4;

    __uint128_t c0 = (__uint128_t)a0*a0 + (__uint128_t)d1*r4 + (__uint128_t)d2*r3;
    __uint128_t c1 = (__uint128_t)d0*a1 + (__uint128_t)r3*a3 + (__uint128_t)d2*r4;
    __uint128_t c2 = (__uint128_t)d0*a2 + (__uint128_t)a1*a1 + (__uint128_t)d3*r4;
    __uint128_t c3 = (__uint128_t)d0*a3 + (__uint128_t)d1*a2 + (__uint128_t)r4*a4;
    __uint128_t c4 = (__uint128_t)d0*a4 + (__uint128_t)d1*a3 + (__uint128_t)a2*a2;

    uint64_t carry;
    carry = (uint64_t)(c0 >> 51); out[0] = (uint64_t)c0 & MASK51;
    c1 += carry;
    carry = (uint64_t)(c1 >> 51); out[1] = (uint64_t)c1 & MASK51;
    c2 += carry;
    carry = (uint64_t)(c2 >> 51); out[2] = (uint64_t)c2 & MASK51;
    c3 += carry;
    carry = (uint64_t)(c3 >> 51); out[3] = (uint64_t)c3 & MASK51;
    c4 += carry;
    carry = (uint64_t)(c4 >> 51); out[4] = (uint64_t)c4 & MASK51;

    out[0] += carry * 19;
    carry = out[0] >> 51; out[0] &= MASK51;
    out[1] += carry;
}


/* ══════════════════════════════════════════════════════════════════
 *  Benchmark
 * ══════════════════════════════════════════════════════════════════ */
static inline uint64_t rdtsc_start(void) {
    uint32_t lo, hi;
    asm volatile("cpuid\n\trdtsc" : "=a"(lo), "=d"(hi) :: "rbx", "rcx");
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtsc_end(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
    asm volatile("cpuid" ::: "rax", "rbx", "rcx", "rdx");
    return ((uint64_t)hi << 32) | lo;
}

#define BATCH 1000
#define REPS  100

int main(void) {
    uint64_t t0, t1, min, sum;

    uint64_t state[5] = { 0x00062D608F25D51AULL,
                          0x000412A4B4F6592AULL,
                          0x00075B7171A4B31DULL,
                          0x0001FF60527118FEULL,
                          0x000216936D3CD6E5ULL };
    uint64_t ref[5], mono[5], tmp[5];

    printf("=== Monolithic Curve25519 fe_sq ===\n\n");
    install_mono_fesq();

    /* ── Correctness ──────────────────────────────────────────── */
    printf("\n--- Correctness ---\n");

    /* Single fe_sq */
    fe_sq_ref(state, ref);
    memcpy(tmp, state, sizeof(tmp));
    fe_sq_mono(tmp, mono);
    int ok = memcmp(ref, mono, sizeof(ref)) == 0;
    printf("  Single fe_sq: %s\n", ok ? "PASS" : "FAIL");
    if (!ok) {
        for (int i = 0; i < 5; i++)
            printf("    [%d] ref=%016" PRIx64 " mono=%016" PRIx64 " %s\n",
                   i, ref[i], mono[i], ref[i] == mono[i] ? "" : "***");
    }

    /* In-place (a == out) */
    memcpy(tmp, state, sizeof(tmp));
    fe_sq_mono(tmp, tmp);
    ok = memcmp(ref, tmp, sizeof(ref)) == 0;
    printf("  In-place:     %s\n", ok ? "PASS" : "FAIL");

    /* Iterated: 1000 squarings */
    uint64_t ref_iter[5], mono_iter[5];
    memcpy(ref_iter, state, sizeof(state));
    memcpy(mono_iter, state, sizeof(state));
    for (int i = 0; i < 1000; i++) {
        fe_sq_ref(ref_iter, ref_iter);
        fe_sq_mono(mono_iter, mono_iter);
    }
    ok = memcmp(ref_iter, mono_iter, sizeof(ref_iter)) == 0;
    printf("  1000 iters:   %s\n", ok ? "PASS" : "FAIL");
    if (!ok) {
        for (int i = 0; i < 5; i++)
            printf("    [%d] ref=%016" PRIx64 " mono=%016" PRIx64 " %s\n",
                   i, ref_iter[i], mono_iter[i],
                   ref_iter[i] == mono_iter[i] ? "" : "***");
        fprintf(stderr, "Correctness FAILED — skipping benchmark.\n");
        return 1;
    }

    /* ── Benchmark ────────────────────────────────────────────── */
    printf("\n--- Benchmark: %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    /* Native C (GCC __uint128_t) */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++)
            fe_sq_ref(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("  Native C:      min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min / BATCH, sum / REPS / BATCH);

    /* Monolithic (1 vmwrite) */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++)
            fe_sq_mono(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("  Monolithic:    min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min / BATCH, sum / REPS / BATCH);

    printf("\n--- Analysis ---\n");
    printf("  Native C:    GCC __uint128_t, 15 MUL + carry chain\n");
    printf("  Monolithic:  1 vmwrite, ~60 triads, all 15 MACs + carry + reduction\n");
    printf("  Savings:     4 fewer vmwrite redirects = ~20 fewer redirect cycles\n");

    return 0;
}
