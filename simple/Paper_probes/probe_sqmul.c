/* probe_sqmul.c — author + measure a 2-output sqmul(X,Y) -> (X^2, X*Y) patch.
 *
 * Goal: get the REAL firing latency of a combined op, to decide the fewer-
 * firings ladder redesign (memory project_microcode_firing_latency).
 *
 * Construction: the mul patch computes X*Y with X (the a-operand) PRESERVED
 * (it's always MUL srcA). So after the mul body, X is still in {rdi,rsi,r12,
 * r11,r14} and X*Y is in {r15,r13,r9,r10,rax}. We then:
 *   - save X*Y to TMP10..TMP14 (the mul's now-dead b-copies),
 *   - synthesize the sq precompute from X (2*Xi into r15/r13/r9/r10,
 *     19*X4->rbx, 19*X3->rdx, rax=0, r8=0) — exactly FE_SQ's wrapper state,
 *   - run the sq body -> X^2 in {rdi,r9,r10,rbx,rax},
 *   - restore X*Y from TMP10..14 into {rsi,r12,r11,r14,r15}.
 * One firing, ~118 triads, fits the 128-triad budget alone.
 *
 * Verify: sqmul's X^2 must equal the standalone sq patch's sq(X), and sqmul's
 * X*Y must equal the standalone mul patch's mul(X,Y). Then time it (dependent
 * chain) and compare to 2*123=246 (two separate firings).
 *
 * Build: make PROG=probe_sqmul CFLAGS="-O3 -march=native -masm=intel -fwrapv -fPIE -I include/"
 * Run:   sudo taskset -c 0 ./probe_sqmul_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL
#define _S(x) #x
#define S(x) _S(x)

/* buffer layout (rbp-relative), 5 u64 each */
#define X_OFF   0
#define Y_OFF   40
#define MR_OFF  80    /* mul reference  = mul(X,Y) */
#define SR_OFF  120   /* sq  reference  = sq(X)    */
#define X2_OFF  160   /* sqmul output X^2 */
#define XY_OFF  200   /* sqmul output X*Y */

/* ───────────────────────── mul patch (66 triads) ───────────────────────── */
static ucode_t mul_patch[] = {
    { ZEROEXT_DSZ64_DR(TMP10, R15), ZEROEXT_DSZ64_DR(TMP11, R13),
      ZEROEXT_DSZ64_DR(TMP12, R9), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R10), ZEROEXT_DSZ64_DR(TMP14, RBX),
      MUL_DSZ64_DIR(RCX, 19, R13), NOP_SEQWORD },
    { MUL_DSZ64_DIR(RCX, 19, R9), MUL_DSZ64_DIR(RCX, 19, R10),
      MUL_DSZ64_DIR(RCX, 19, RBX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RDX, TMP10), MUL_DSZ64_DRR(RCX, RDI, RDX),
      NOTAND_DSZ64_DRR(TMP0, TMP0, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R15, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RDX, TMP11), MUL_DSZ64_DRR(RCX, RDI, RDX),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R13, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RDX, TMP12), MUL_DSZ64_DRR(RCX, RDI, RDX),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R9, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RDX, TMP13), MUL_DSZ64_DRR(RCX, RDI, RDX),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP12), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R10, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RDX, TMP14), MUL_DSZ64_DRR(RCX, RDI, RDX),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP13), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP12), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(RAX, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DIR(TMP2, 19, TMP0),
      ADD_DSZ64_DRR(R15, R15, TMP0), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, R15, 51), ADD_DSZ64_DRR(R13, R13, TMP0),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP2, R15, 13), SHR_DSZ64_DRI(R15, TMP2, 13),
      NOP, END_SEQWORD }
};

/* ───────────────────────── sq patch (42 triads) ────────────────────────── */
static ucode_t sq_patch[] = {
    { ZEROEXT_DSZ64_DR(TMP0, RAX), MUL_DSZ64_DRR(RCX, RDI, RDI),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDI), SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP9, TMP15), MUL_DSZ64_DRR(RCX, RBX, R13),
      ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RDX, R9), ADD_DSZ64_DRR(TMP0, TMP0, R9),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(RDI, TMP6, 13), ZEROEXT_DSZ64_DR(R13, RSI),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, R13),
      ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(R9, R12, R12), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP9, TMP15), MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RBX, R9), ADD_DSZ64_DRR(TMP0, TMP0, R9),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(R9, TMP6, 13), ZEROEXT_DSZ64_DR(RDX, R12),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ZEROEXT_DSZ64_DR(R13, RSI), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP9, TMP15), MUL_DSZ64_DRR(RCX, RSI, R13),
      ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RBX, R10), ADD_DSZ64_DRR(TMP0, TMP0, R10),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(R10, TMP6, 13), ZEROEXT_DSZ64_DR(RDX, R11),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, RDX),
      ADD_DSZ64_DRR(R13, RSI, RSI), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RAX, R12), MUL_DSZ64_DRR(RCX, R13, RAX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RAX), SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, R14, RBX), ADD_DSZ64_DRR(TMP0, TMP0, RBX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(TMP8, TMP0, 51),
      SHL_DSZ64_DRI(TMP6, TMP0, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(RBX, TMP6, 13), SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, R14),
      ADD_DSZ64_DRR(TMP0, TMP0, R14), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, R13, R11), ADD_DSZ64_DRR(TMP0, TMP0, R11),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      MUL_DSZ64_DRR(RCX, R12, R12), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, R12), SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHR_DSZ64_DRI(TMP8, TMP0, 51),
      ADD_DSZ64_DRR(R8, R8, TMP9), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP6, TMP0, 13), SHL_DSZ64_DRI(TMP1, R8, 13),
      SHR_DSZ64_DRI(RAX, TMP6, 13), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DIR(TMP6, 19, TMP0),
      ADD_DSZ64_DRR(RDI, RDI, TMP0), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, RDI, 51), ADD_DSZ64_DRR(R9, R9, TMP0),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP6, RDI, 13), SHR_DSZ64_DRI(RDI, TMP6, 13),
      NOP, END_SEQWORD }
};

/* Bridge: save X*Y -> TMP10..14, then synthesize FE_SQ's precompute from X. */
static ucode_t bridge[] = {
    { ZEROEXT_DSZ64_DR(TMP10, R15), ZEROEXT_DSZ64_DR(TMP11, R13),
      ZEROEXT_DSZ64_DR(TMP12, R9), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R10), ZEROEXT_DSZ64_DR(TMP14, RAX),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(R15, RDI, RDI), ADD_DSZ64_DRR(R13, RSI, RSI),
      ADD_DSZ64_DRR(R9, R12, R12), NOP_SEQWORD },          /* 2X0,2X1,2X2 */
    { ADD_DSZ64_DRR(R10, R11, R11), ZEROEXT_DSZ32_DI(TMP0, 19),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },          /* 2X3, TMP0=19, R8=0 */
    { MUL_DSZ64_DRR(TMP1, R14, TMP0), NOP, NOP, NOP_SEQWORD },   /* TMP0 = 19*X4 (R14 kept) */
    { ZEROEXT_DSZ64_DR(RBX, TMP0), ZEROEXT_DSZ32_DI(TMP0, 19),
      NOTAND_DSZ64_DRR(RAX, RAX, RAX), NOP_SEQWORD },        /* RBX=19X4, TMP0=19, RAX=0 */
    { MUL_DSZ64_DRR(TMP1, R11, TMP0), NOP, NOP, NOP_SEQWORD },   /* TMP0 = 19*X3 (R11 kept) */
    { ZEROEXT_DSZ64_DR(RDX, TMP0), NOP, NOP, NOP_SEQWORD }       /* RDX=19X3 */
};

/* Restore X*Y from TMP10..14 into {rsi,r12,r11,r14,r15}. */
static ucode_t restore[] = {
    { ZEROEXT_DSZ64_DR(RSI, TMP10), ZEROEXT_DSZ64_DR(R12, TMP11),
      ZEROEXT_DSZ64_DR(R11, TMP12), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R14, TMP13), ZEROEXT_DSZ64_DR(R15, TMP14),
      NOP, END_SEQWORD }
};

static ucode_t sqmul_patch[128];

static int build_sqmul(void) {
    int n = 0, i;
    int mn = (int)ARRAY_SZ(mul_patch), sn = (int)ARRAY_SZ(sq_patch);
    for (i = 0; i < mn; i++) { sqmul_patch[n] = mul_patch[i];
        if (i == mn - 1) sqmul_patch[n].seqw = NOP_SEQWORD; n++; }   /* mul: drop END */
    for (i = 0; i < (int)ARRAY_SZ(bridge); i++) sqmul_patch[n++] = bridge[i];
    for (i = 0; i < sn; i++) { sqmul_patch[n] = sq_patch[i];
        if (i == sn - 1) sqmul_patch[n].seqw = NOP_SEQWORD; n++; }    /* sq: drop END */
    for (i = 0; i < (int)ARRAY_SZ(restore); i++) sqmul_patch[n++] = restore[i];
    return n;
}

/* ─────────────────────────── wrappers ─────────────────────────── */
#define LOAD_A(a) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  "mov rsi, [rbp + " S(a) " + 8]\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t" "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t"

#define FE_MUL(out, a, b) \
    LOAD_A(a) \
    "mov r15, [rbp + " S(b) " + 0]\n\t"  "mov r13, [rbp + " S(b) " + 8]\n\t" \
    "mov r9,  [rbp + " S(b) " + 16]\n\t" "mov r10, [rbp + " S(b) " + 24]\n\t" \
    "mov rbx, [rbp + " S(b) " + 32]\n\t" \
    "xor eax, eax\n\t" "xor r8d, r8d\n\t" "vmwrite rcx, rdx\n\t" \
    "mov [rbp + " S(out) " + 0],  r15\n\t" "mov [rbp + " S(out) " + 8],  r13\n\t" \
    "mov [rbp + " S(out) " + 16], r9\n\t"  "mov [rbp + " S(out) " + 24], r10\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

#define FE_SQ(out, a) \
    "mov r14, [rbp + " S(a) " + 32]\n\t" "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t" "mov rsi, [rbp + " S(a) " + 8]\n\t" \
    "mov rdi, [rbp + " S(a) " + 0]\n\t" \
    "lea r15, [rdi + rdi]\n\t" "lea r13, [rsi + rsi]\n\t" \
    "lea r9,  [r12 + r12]\n\t" "lea r10, [r11 + r11]\n\t" \
    "imul rbx, r14, 19\n\t" "imul rdx, r11, 19\n\t" \
    "xor eax, eax\n\t" "xor r8d, r8d\n\t" \
    ".byte 0x0f, 0x78, 0xca\n\t" \
    "mov [rbp + " S(out) " + 0],  rdi\n\t" "mov [rbp + " S(out) " + 8],  r9\n\t" \
    "mov [rbp + " S(out) " + 16], r10\n\t" "mov [rbp + " S(out) " + 24], rbx\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

/* sqmul: vmwrite trigger; X^2 in {rdi,r9,r10,rbx,rax}, X*Y in {rsi,r12,r11,r14,r15}. */
#define SQMUL(x2, xy, X, Y) \
    LOAD_A(X) \
    "mov r15, [rbp + " S(Y) " + 0]\n\t"  "mov r13, [rbp + " S(Y) " + 8]\n\t" \
    "mov r9,  [rbp + " S(Y) " + 16]\n\t" "mov r10, [rbp + " S(Y) " + 24]\n\t" \
    "mov rbx, [rbp + " S(Y) " + 32]\n\t" \
    "xor eax, eax\n\t" "xor r8d, r8d\n\t" "vmwrite rcx, rdx\n\t" \
    "mov [rbp + " S(x2) " + 0],  rdi\n\t" "mov [rbp + " S(x2) " + 8],  r9\n\t" \
    "mov [rbp + " S(x2) " + 16], r10\n\t" "mov [rbp + " S(x2) " + 24], rbx\n\t" \
    "mov [rbp + " S(x2) " + 32], rax\n\t" \
    "mov [rbp + " S(xy) " + 0],  rsi\n\t" "mov [rbp + " S(xy) " + 8],  r12\n\t" \
    "mov [rbp + " S(xy) " + 16], r11\n\t" "mov [rbp + " S(xy) " + 24], r14\n\t" \
    "mov [rbp + " S(xy) " + 32], r15\n\t"

#define CLOB "rax","rbx","rcx","rdx","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"

#define REP2(x)  x x
#define REP4(x)  REP2(x) REP2(x)
#define REP8(x)  REP4(x) REP4(x)
#define REP16(x) REP8(x) REP8(x)
#define UNROLL 16
#define REPS   1000
#define TRIALS 50

static inline uint64_t rdtsc_start(void){ uint32_t lo,hi;
    asm volatile("cpuid\n\trdtsc":"=a"(lo),"=d"(hi)::"rbx","rcx","memory"); return ((uint64_t)hi<<32)|lo; }
static inline uint64_t rdtsc_end(void){ uint32_t lo,hi;
    asm volatile("rdtscp":"=a"(lo),"=d"(hi)::"rcx","memory");
    asm volatile("cpuid":::"rax","rbx","rcx","rdx","memory"); return ((uint64_t)hi<<32)|lo; }

static void do_mul(uint64_t *b){ register uint64_t *p asm("rbp")=b;
    asm volatile(FE_MUL(MR_OFF, X_OFF, Y_OFF):: "r"(p): CLOB); }
static void do_sq(uint64_t *b){ register uint64_t *p asm("rbp")=b;
    asm volatile(FE_SQ(SR_OFF, X_OFF):: "r"(p): CLOB); }
static void do_sqmul(uint64_t *b){ register uint64_t *p asm("rbp")=b;
    asm volatile(SQMUL(X2_OFF, XY_OFF, X_OFF, Y_OFF):: "r"(p): CLOB); }

static uint64_t time_sqmul(uint64_t *b){
    uint64_t best=~0ULL;
    for(int t=0;t<TRIALS;t++){
        register uint64_t *p asm("rbp")=b;
        uint64_t a=rdtsc_start();
        for(int r=0;r<REPS;r++) asm volatile(REP16(SQMUL(X_OFF, XY_OFF, X_OFF, Y_OFF)):: "r"(p): CLOB);
        uint64_t c=rdtsc_end()-a; if(c<best)best=c;
    }
    return best;
}

typedef unsigned long long ull;
static void pr(const uint64_t*v){ printf("%013llx %013llx %013llx %013llx %013llx\n",
    (ull)v[0],(ull)v[1],(ull)v[2],(ull)v[3],(ull)v[4]); }

int main(void){
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    static uint64_t buf[30];
    uint64_t X[5]={0x123456789ABCD&MASK51,0x2468ACE13579B&MASK51,0x13579BDF02468&MASK51,
                   0x0FEDCBA987654&MASK51,0x1111122222333&MASK51};
    uint64_t Y[5]={0x0AAAA11119999&MASK51,0x07777BBBB5555&MASK51,0x033338888CCCC&MASK51,
                   0x1234599991111&MASK51,0x0ABCDEF012345&MASK51};
    memcpy(&buf[X_OFF/8],X,40); memcpy(&buf[Y_OFF/8],Y,40);

    /* references from the real separate patches (both hooks, like inline2) */
    patch_ucode(0x7c00, mul_patch, ARRAY_SZ(mul_patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    uint64_t sq_addr = 0x7c00 + ARRAY_SZ(mul_patch)*4;
    patch_ucode(sq_addr, sq_patch, ARRAY_SZ(sq_patch));
    hook_match_and_patch(1, 0x0618, sq_addr);
    do_mul(buf);   /* MR = X*Y */
    do_sq(buf);    /* SR = X^2 */

    /* install sqmul alone, hooked on vmwrite */
    int n = build_sqmul();
    printf("sqmul patch: %d triads (mul %d + bridge %d + sq %d + restore %d)\n\n",
           n, (int)ARRAY_SZ(mul_patch), (int)ARRAY_SZ(bridge),
           (int)ARRAY_SZ(sq_patch), (int)ARRAY_SZ(restore));
    patch_ucode(0x7c00, sqmul_patch, n);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    do_sqmul(buf);  /* X2_OFF = X^2 ; XY_OFF = X*Y */

    int x2_ok = memcmp(&buf[X2_OFF/8], &buf[SR_OFF/8], 40) == 0;
    int xy_ok = memcmp(&buf[XY_OFF/8], &buf[MR_OFF/8], 40) == 0;
    printf("verify X^2 (sqmul vs sq):  %s\n", x2_ok ? "OK" : "MISMATCH");
    printf("verify X*Y (sqmul vs mul): %s\n", xy_ok ? "OK" : "MISMATCH");
    if (!x2_ok || !xy_ok) {
        printf("  X^2 sqmul: "); pr(&buf[X2_OFF/8]);
        printf("  X^2 ref  : "); pr(&buf[SR_OFF/8]);
        printf("  X*Y sqmul: "); pr(&buf[XY_OFF/8]);
        printf("  X*Y ref  : "); pr(&buf[MR_OFF/8]);
    }

    /* latency */
    memcpy(&buf[X_OFF/8], X, 40);
    uint64_t best = time_sqmul(buf);
    double per = (double)best / ((double)REPS*UNROLL);
    printf("\nsqmul firing latency (dependent): %.2f cyc\n", per);
    printf("  vs 2 separate firings (~2*123): 246 cyc\n");
    printf("  -> %s per pair\n", per < 246 ? "WIN (combining beats 2 firings)" : "LOSS");

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
