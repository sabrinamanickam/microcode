/*
 * full_curve25519_inline2.c — register-chained 5×51 ladder experiment.
 *
 * Forked from full_curve25519_inline.c to test whether chaining consecutive
 * ladder ops through registers (skipping memory reloads at the start of
 * each op) reduces cycles. Prior memory `inline-asm-no-help-5x51` recorded
 * 0 cyc savings from a similar restructure; goal here is to verify or
 * disprove that with a more targeted chain pattern.
 *
 * Dataflow chains used in ladder_step:
 *   1→2:   A   = ADD(x2, z2)   →  AA  = SQ(A)     (5 reloads avoided)
 *   3→4:   B   = SUB(x2, z2)   →  BB  = SQ(B)     (5 reloads avoided)
 *   7→8:   D   = SUB(x3, z3)   →  DA  = MUL(D,A)  (5 reloads avoided)
 *  10→11:  t0  = ADD(DA, CB)   →  x3' = SQ(t0)    (5 reloads avoided)
 *  12→13:  t0  = SUB(DA, CB)   →  z3' = SQ(t0)    (5 reloads avoided)
 *
 * Upper bound per X25519: 255 × 25 ≈ 6.4k cyc (at 1 cyc/load) vs ~312k
 * baseline. STLF typically hides this; if it does, no gain.
 *
 * Build: make PROG=full_curve25519_inline2
 * Run:   sudo taskset -c 0 ./full_curve25519_inline2_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "../../../include/patch.h"
#include "../../../include/ucode_macro.h"
#include "../../../include/misc.h"

typedef uint64_t fe[5];
#define MASK51 0x7FFFFFFFFFFFFULL

/*
 * Ladder state: all field elements live in this struct on the stack,
 * accessed by the inline asm via [rbp + offset]. Offsets are fixed
 * constants the asm uses directly.
 */
typedef struct {
    uint64_t x1[5];     /* offset 0   */
    uint64_t x2[5];     /* offset 40  */
    uint64_t z2[5];     /* offset 80  */
    uint64_t x3[5];     /* offset 120 */
    uint64_t z3[5];     /* offset 160 */
    uint64_t A[5];      /* offset 200 */
    uint64_t AA[5];     /* offset 240 */
    uint64_t B[5];      /* offset 280 */
    uint64_t BB[5];     /* offset 320 */
    uint64_t E[5];      /* offset 360 */
    uint64_t C[5];      /* offset 400 */
    uint64_t D[5];      /* offset 440 */
    uint64_t DA[5];     /* offset 480 */
    uint64_t CB[5];     /* offset 520 */
    uint64_t t0[5];     /* offset 560 */
} ladder_state_t;

#define X1_OFF  0
#define X2_OFF  40
#define Z2_OFF  80
#define X3_OFF  120
#define Z3_OFF  160
#define A_OFF   200
#define AA_OFF  240
#define B_OFF   280
#define BB_OFF  320
#define E_OFF   360
#define C_OFF   400
#define D_OFF   440
#define DA_OFF  480
#define CB_OFF  520
#define T0_OFF  560

/* Invert state — local to fe_invert, kept on stack with rbp pointing at it
 * during the inverse exponentiation. Held in its own struct so the offsets
 * are independent of ladder_state_t. */
typedef struct {
    uint64_t z[5];     /* offset 0   — input copy */
    uint64_t z2[5];    /* offset 40  */
    uint64_t z9[5];    /* offset 80  */
    uint64_t z11[5];   /* offset 120 */
    uint64_t t[5];     /* offset 160 */
    uint64_t t0[5];    /* offset 200 */
    uint64_t t1[5];    /* offset 240 */
    uint64_t t2[5];    /* offset 280 */
    uint64_t t3[5];    /* offset 320 */
} invert_state_t;

#define IZ_OFF    0
#define IZ2_OFF   40
#define IZ9_OFF   80
#define IZ11_OFF  120
#define IT_OFF    160
#define IT0_OFF   200
#define IT1_OFF   240
#define IT2_OFF   280
#define IT3_OFF   320

/* Two-step stringify so macro args like X2_OFF expand to their integer
 * literal before being placed into the asm string. */
#define _S(x) #x
#define S(x) _S(x)

/* ════════════════════════════════════════════════════════════════════
 * MICROCODE PATCH INSTALLATION (verbatim from full_curve25519.c)
 * ════════════════════════════════════════════════════════════════════ */

static void install_field_patches(void) {
    ucode_t mul_patch[] = {
    { ZEROEXT_DSZ64_DR(TMP10, R15), ZEROEXT_DSZ64_DR(TMP11, R13),
      ZEROEXT_DSZ64_DR(TMP12, R9), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R10), ZEROEXT_DSZ64_DR(TMP14, RBX),
      MUL_DSZ64_DIR(RCX, 19, R13), NOP_SEQWORD },
    { MUL_DSZ64_DIR(RCX, 19, R9), MUL_DSZ64_DIR(RCX, 19, R10),
      MUL_DSZ64_DIR(RCX, 19, RBX), NOP_SEQWORD },
    /* c0 */
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
    /* c1 */
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
    /* c2 */
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
    /* c3 */
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
    /* c4 */
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
    /* final reduction */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DIR(TMP2, 19, TMP0),
      ADD_DSZ64_DRR(R15, R15, TMP0), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, R15, 51), ADD_DSZ64_DRR(R13, R13, TMP0),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP2, R15, 13), SHR_DSZ64_DRI(R15, TMP2, 13),
      NOP, END_SEQWORD }
    };

    ucode_t sq_patch[] = {
    /* c0 */
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
    /* c1 */
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
    /* c2 */
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
    /* c3 */
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
    /* c4 */
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
    /* final reduction */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DIR(TMP6, 19, TMP0),
      ADD_DSZ64_DRR(RDI, RDI, TMP0), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, RDI, 51), ADD_DSZ64_DRR(R9, R9, TMP0),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP6, RDI, 13), SHR_DSZ64_DRI(RDI, TMP6, 13),
      NOP, END_SEQWORD }
    };

    patch_ucode(0x7c00, mul_patch, ARRAY_SZ(mul_patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    uint64_t sq_addr = 0x7c00 + ARRAY_SZ(mul_patch) * 4;
    patch_ucode(sq_addr, sq_patch, ARRAY_SZ(sq_patch));
    hook_match_and_patch(1, 0x0618, sq_addr);
    printf("fe_mul: %d triads at U%04lx (vmwrite hook)\n",
           (int)ARRAY_SZ(mul_patch), (unsigned long)0x7c00);
    printf("fe_sq : %d triads at U%04lx (vmread  hook)\n",
           (int)ARRAY_SZ(sq_patch),  (unsigned long)sq_addr);
}

/* ════════════════════════════════════════════════════════════════════
 * INLINE-ASM FIELD-OP MACROS
 *
 * Each macro emits an asm string fragment. The fragments share the state
 * pointer in RBP and clobber the same set of GP regs across the whole
 * asm block — GCC only spills around the outer asm block, not between
 * sub-ops. The patches' caller conventions are matched per op.
 *
 * Operand convention: offsets are integer literals relative to [rbp].
 *
 * NOTE: rbp is *not* a callee-saved reg requirement here — we save/restore
 * it manually at the asm block boundary so we can use it as the state
 * pointer throughout.
 * ════════════════════════════════════════════════════════════════════ */

/* FE_MUL(out, a, b) — fires the mul patch with inputs from [rbp+a/b],
 * stores h[0..4] to [rbp+out]. */
#define FE_MUL(out, a, b) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  \
    "mov rsi, [rbp + " S(a) " + 8]\n\t"  \
    "mov r12, [rbp + " S(a) " + 16]\n\t" \
    "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t" \
    "mov r15, [rbp + " S(b) " + 0]\n\t"  \
    "mov r13, [rbp + " S(b) " + 8]\n\t"  \
    "mov r9,  [rbp + " S(b) " + 16]\n\t" \
    "mov r10, [rbp + " S(b) " + 24]\n\t" \
    "mov rbx, [rbp + " S(b) " + 32]\n\t" \
    "xor eax, eax\n\t"                   \
    "xor r8d, r8d\n\t"                   \
    "vmwrite rcx, rdx\n\t"               \
    "mov [rbp + " S(out) " + 0],  r15\n\t" \
    "mov [rbp + " S(out) " + 8],  r13\n\t" \
    "mov [rbp + " S(out) " + 16], r9\n\t"  \
    "mov [rbp + " S(out) " + 24], r10\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

/* FE_SQ(out, a) — fires the sq patch (vmread). Precompute 2*a and 19*a
 * happen inline. Output: rdi=h0, r9=h1, r10=h2, rbx=h3, rax=h4. */
#define FE_SQ(out, a) \
    "mov r14, [rbp + " S(a) " + 32]\n\t" \
    "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t" \
    "mov rsi, [rbp + " S(a) " + 8]\n\t"  \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  \
    "lea r15, [rdi + rdi]\n\t"           \
    "lea r13, [rsi + rsi]\n\t"           \
    "lea r9,  [r12 + r12]\n\t"           \
    "lea r10, [r11 + r11]\n\t"           \
    "imul rbx, r14, 19\n\t"              \
    "imul rdx, r11, 19\n\t"              \
    "xor eax, eax\n\t"                   \
    "xor r8d, r8d\n\t"                   \
    ".byte 0x0f, 0x78, 0xca\n\t"         \
    "mov [rbp + " S(out) " + 0],  rdi\n\t" \
    "mov [rbp + " S(out) " + 8],  r9\n\t"  \
    "mov [rbp + " S(out) " + 16], r10\n\t" \
    "mov [rbp + " S(out) " + 24], rbx\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

/* FE_ADD(out, a, b) — 5-limb add. Restructured to leave the 5 result limbs
 * in {rdi, rsi, r12, r11, r14} so a downstream FE_SQ_FROM_REGS / FE_MUL_FROM_REGS_A
 * can pick them up without reloading from memory. We still store to [out] because
 * non-chained consumers (downstream MUL/SUB in other slots) need the memory copy.
 *
 * Cost vs original (rax-through-serialization): identical instruction count
 * (5 loads + 5 adds + 5 stores), no extra movs. */
#define FE_ADD(out, a, b) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"   "add rdi, [rbp + " S(b) " + 0]\n\t"   "mov [rbp + " S(out) " + 0],  rdi\n\t" \
    "mov rsi, [rbp + " S(a) " + 8]\n\t"   "add rsi, [rbp + " S(b) " + 8]\n\t"   "mov [rbp + " S(out) " + 8],  rsi\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t"  "add r12, [rbp + " S(b) " + 16]\n\t"  "mov [rbp + " S(out) " + 16], r12\n\t" \
    "mov r11, [rbp + " S(a) " + 24]\n\t"  "add r11, [rbp + " S(b) " + 24]\n\t"  "mov [rbp + " S(out) " + 24], r11\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t"  "add r14, [rbp + " S(b) " + 32]\n\t"  "mov [rbp + " S(out) " + 32], r14\n\t"

/* FE_SUB(out, a, b) — adds 2*p as bias to keep limbs positive.
 * limb 0 bias: 2*(2^51 - 19) = 0xFFFFFFFFFFFDA
 * limbs 1..4 bias: 2*(2^51 - 1) = 0xFFFFFFFFFFFFE
 *
 * Restructured to leave 5 result limbs in {rdi, rsi, r12, r11, r14} (chain-ready).
 * Tightened: the limb1..4 bias is loaded into rcx once and reused, saving 3 movs
 * relative to the original macro. */
#define FE_SUB(out, a, b) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  "mov rcx, 0xFFFFFFFFFFFDA\n\t" "add rdi, rcx\n\t" "sub rdi, [rbp + " S(b) " + 0]\n\t"  "mov [rbp + " S(out) " + 0],  rdi\n\t" \
    "mov rcx, 0xFFFFFFFFFFFFE\n\t" \
    "mov rsi, [rbp + " S(a) " + 8]\n\t"  "add rsi, rcx\n\t" "sub rsi, [rbp + " S(b) " + 8]\n\t"  "mov [rbp + " S(out) " + 8],  rsi\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t" "add r12, rcx\n\t" "sub r12, [rbp + " S(b) " + 16]\n\t" "mov [rbp + " S(out) " + 16], r12\n\t" \
    "mov r11, [rbp + " S(a) " + 24]\n\t" "add r11, rcx\n\t" "sub r11, [rbp + " S(b) " + 24]\n\t" "mov [rbp + " S(out) " + 24], r11\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t" "add r14, rcx\n\t" "sub r14, [rbp + " S(b) " + 32]\n\t" "mov [rbp + " S(out) " + 32], r14\n\t"

/* FE_SQ_FROM_REGS(out) — assumes a[0..4] already in {rdi, rsi, r12, r11, r14}.
 * Skips the 5 input loads vs FE_SQ; everything else identical. */
#define FE_SQ_FROM_REGS(out) \
    "lea r15, [rdi + rdi]\n\t"           \
    "lea r13, [rsi + rsi]\n\t"           \
    "lea r9,  [r12 + r12]\n\t"           \
    "lea r10, [r11 + r11]\n\t"           \
    "imul rbx, r14, 19\n\t"              \
    "imul rdx, r11, 19\n\t"              \
    "xor eax, eax\n\t"                   \
    "xor r8d, r8d\n\t"                   \
    ".byte 0x0f, 0x78, 0xca\n\t"         \
    "mov [rbp + " S(out) " + 0],  rdi\n\t" \
    "mov [rbp + " S(out) " + 8],  r9\n\t"  \
    "mov [rbp + " S(out) " + 16], r10\n\t" \
    "mov [rbp + " S(out) " + 24], rbx\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

/* FE_MUL_FROM_REGS_A(out, b) — assumes a[0..4] already in {rdi, rsi, r12, r11, r14}.
 * Only loads b into {r15, r13, r9, r10, rbx}; skips the 5 input loads for a. */
#define FE_MUL_FROM_REGS_A(out, b) \
    "mov r15, [rbp + " S(b) " + 0]\n\t"  \
    "mov r13, [rbp + " S(b) " + 8]\n\t"  \
    "mov r9,  [rbp + " S(b) " + 16]\n\t" \
    "mov r10, [rbp + " S(b) " + 24]\n\t" \
    "mov rbx, [rbp + " S(b) " + 32]\n\t" \
    "xor eax, eax\n\t"                   \
    "xor r8d, r8d\n\t"                   \
    "vmwrite rcx, rdx\n\t"               \
    "mov [rbp + " S(out) " + 0],  r15\n\t" \
    "mov [rbp + " S(out) " + 8],  r13\n\t" \
    "mov [rbp + " S(out) " + 16], r9\n\t"  \
    "mov [rbp + " S(out) " + 24], r10\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

/* Use a C helper for fe_mul121665 — too complex to write cleanly inline. */

/* ════════════════════════════════════════════════════════════════════
 * INVERT-CHAIN MACROS
 *
 * For fe_invert we want to chain consecutive squarings inside one asm
 * block without round-tripping through memory between squarings. The
 * sq patch's caller convention is:
 *   inputs : a in {rdi, rsi, r12, r11, r14}
 *   outputs: h in {rdi, r9, r10, rbx, rax}
 *
 * Only rdi (a[0] = h[0]) overlaps. To chain, we rename the other 4
 * outputs back to the input slots (4 movs) between consecutive sqs.
 * No memory traffic for the intermediate.
 * ════════════════════════════════════════════════════════════════════ */

/* Load `a` from [rbp+a_off] into the sq input registers. */
#define INV_SQ_LOAD(a) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  \
    "mov rsi, [rbp + " S(a) " + 8]\n\t"  \
    "mov r12, [rbp + " S(a) " + 16]\n\t" \
    "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t"

/* Rename previous sq's output {rdi, r9, r10, rbx, rax} → input {rdi, rsi, r12, r11, r14}.
 * rdi is already correct (h[0] = a[0]); 4 movs cover the other 4 limbs. */
#define INV_SQ_RENAME \
    "mov rsi, r9\n\t"  \
    "mov r12, r10\n\t" \
    "mov r11, rbx\n\t" \
    "mov r14, rax\n\t"

/* Execute one squaring: inputs assumed already in sq input regs; outputs to sq output regs.
 * Identical to FE_SQ's body without the load/store wrappers. */
#define INV_SQ_OP \
    "lea r15, [rdi + rdi]\n\t"           \
    "lea r13, [rsi + rsi]\n\t"           \
    "lea r9,  [r12 + r12]\n\t"           \
    "lea r10, [r11 + r11]\n\t"           \
    "imul rbx, r14, 19\n\t"              \
    "imul rdx, r11, 19\n\t"              \
    "xor eax, eax\n\t"                   \
    "xor r8d, r8d\n\t"                   \
    ".byte 0x0f, 0x78, 0xca\n\t"

/* Store sq output to [rbp+out_off]. */
#define INV_SQ_STORE(out) \
    "mov [rbp + " S(out) " + 0],  rdi\n\t" \
    "mov [rbp + " S(out) " + 8],  r9\n\t"  \
    "mov [rbp + " S(out) " + 16], r10\n\t" \
    "mov [rbp + " S(out) " + 24], rbx\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

/* Single sq (memory → memory). No chain. */
#define INV_SQ(out, a) INV_SQ_LOAD(a) INV_SQ_OP INV_SQ_STORE(out)

/* MUL (memory → memory). Standalone; standard fe_mul caller convention. */
#define INV_MUL(out, a, b) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  \
    "mov rsi, [rbp + " S(a) " + 8]\n\t"  \
    "mov r12, [rbp + " S(a) " + 16]\n\t" \
    "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t" \
    "mov r15, [rbp + " S(b) " + 0]\n\t"  \
    "mov r13, [rbp + " S(b) " + 8]\n\t"  \
    "mov r9,  [rbp + " S(b) " + 16]\n\t" \
    "mov r10, [rbp + " S(b) " + 24]\n\t" \
    "mov rbx, [rbp + " S(b) " + 32]\n\t" \
    "xor eax, eax\n\t"                   \
    "xor r8d, r8d\n\t"                   \
    "vmwrite rcx, rdx\n\t"               \
    "mov [rbp + " S(out) " + 0],  r15\n\t" \
    "mov [rbp + " S(out) " + 8],  r13\n\t" \
    "mov [rbp + " S(out) " + 16], r9\n\t"  \
    "mov [rbp + " S(out) " + 24], r10\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

/* ════════════════════════════════════════════════════════════════════
 * LADDER STEP — one big asm block per iteration
 * ════════════════════════════════════════════════════════════════════ */

static void fe_mul121665_native(uint64_t *out, const uint64_t *a) {
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

static void ladder_step(ladder_state_t *st) {
    /* The asm block computes:
     *   A   = x2 + z2
     *   AA  = sq(A)
     *   B   = x2 - z2
     *   BB  = sq(B)
     *   E   = AA - BB
     *   C   = x3 + z3
     *   D   = x3 - z3
     *   DA  = mul(D, A)
     *   CB  = mul(C, B)
     *   t0  = DA + CB
     *   x3' = sq(t0)        (stored to st->x3)
     *   t0  = DA - CB
     *   z3' = sq(t0)        (temp; next mul finishes z3)
     *   z3  = mul(x1, z3')  (stored to st->z3 via t0 slot then mul'd)
     *   x2' = mul(AA, BB)   (stored to st->x2)
     * And then in C (outside asm):
     *   t0  = mul121665(E)
     *   t0  = AA + t0
     *   z2  = mul(E, t0)
     *
     * We split: do the bulk in asm; do mul121665 + the final two in C with
     * the regular asm wrappers (they pay the wrapper cost, but it's only
     * 2 ops out of 9 per iter). */

    register ladder_state_t *_st asm("rbp") = st;
    asm volatile(
        /* 1→2 chain: FE_ADD leaves A in {rdi,rsi,r12,r11,r14}; SQ_FROM_REGS picks up. */
        FE_ADD(A_OFF, X2_OFF, Z2_OFF)
        FE_SQ_FROM_REGS(AA_OFF)
        /* 3→4 chain: FE_SUB leaves B in same regs. */
        FE_SUB(B_OFF, X2_OFF, Z2_OFF)
        FE_SQ_FROM_REGS(BB_OFF)
        /* Step 5: full FE_SUB (AA, BB both reloaded from memory). */
        FE_SUB(E_OFF, AA_OFF, BB_OFF)
        /* Reordered 7-8-6-9 to enable two chains in this section.
         * Original: 6 (C=ADD), 7 (D=SUB), 8 (DA=MUL(D,A)), 9 (CB=MUL(C,B)) — one chain (7→8).
         * Reordered: 7 (D=SUB) → 8 (DA chain), 6 (C=ADD) → 9 (CB chain) — two chains.
         * Dataflow is preserved: C and D both depend only on x3, z3, and feed different muls. */
        FE_SUB(D_OFF, X3_OFF, Z3_OFF)
        FE_MUL_FROM_REGS_A(DA_OFF, A_OFF)
        FE_ADD(C_OFF, X3_OFF, Z3_OFF)
        FE_MUL_FROM_REGS_A(CB_OFF, B_OFF)
        /* 10→11 chain: FE_ADD leaves t0 in regs; SQ_FROM_REGS picks up. */
        FE_ADD(T0_OFF, DA_OFF, CB_OFF)
        FE_SQ_FROM_REGS(X3_OFF)
        /* 12→13 chain: FE_SUB leaves t0 in regs; SQ_FROM_REGS picks up. */
        FE_SUB(T0_OFF, DA_OFF, CB_OFF)
        FE_SQ_FROM_REGS(Z3_OFF)
        /* Step 14: full FE_MUL (sq output regs don't match mul input regs; reload from mem). */
        FE_MUL(Z3_OFF, X1_OFF, Z3_OFF)
        /* Step 15: full FE_MUL. */
        FE_MUL(X2_OFF, AA_OFF, BB_OFF)
        :
        : "r"(_st)
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );

    /* Tail: mul121665 (C) + add + mul (inline asm).
     * Chain: FE_ADD leaves t0 in {rdi,rsi,r12,r11,r14}; FE_MUL_FROM_REGS_A
     * consumes t0 as its `a` operand. Since mul commutes, t0*E = E*t0 = z2. */
    fe_mul121665_native(st->t0, st->E);
    register ladder_state_t *_st2 asm("rbp") = st;
    asm volatile(
        FE_ADD(T0_OFF, AA_OFF, T0_OFF)
        FE_MUL_FROM_REGS_A(Z2_OFF, E_OFF)
        :
        : "r"(_st2)
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );
}

/* ════════════════════════════════════════════════════════════════════
 * cswap, frombytes, tobytes, invert, ladder driver
 * (these match full_curve25519.c's 5×51 implementations)
 * ════════════════════════════════════════════════════════════════════ */

static inline void fe_cswap(uint64_t a[5], uint64_t b[5], uint64_t swap) {
    swap = (uint64_t)(-(int64_t)swap);
    for (int i = 0; i < 5; i++) {
        uint64_t x = (a[i] ^ b[i]) & swap;
        a[i] ^= x;
        b[i] ^= x;
    }
}

static void fe_frombytes(uint64_t out[5], const uint8_t in[32]) {
    uint64_t t[5];
    t[0]  = ((uint64_t)in[0])  | ((uint64_t)in[1] << 8) | ((uint64_t)in[2] << 16)
          | ((uint64_t)in[3] << 24) | ((uint64_t)in[4] << 32) | ((uint64_t)in[5] << 40)
          | ((uint64_t)(in[6] & 0x07) << 48);
    t[1]  = ((uint64_t)in[6] >> 3) | ((uint64_t)in[7] << 5) | ((uint64_t)in[8] << 13)
          | ((uint64_t)in[9] << 21) | ((uint64_t)in[10] << 29) | ((uint64_t)in[11] << 37)
          | ((uint64_t)(in[12] & 0x3f) << 45);
    t[2]  = ((uint64_t)in[12] >> 6) | ((uint64_t)in[13] << 2) | ((uint64_t)in[14] << 10)
          | ((uint64_t)in[15] << 18) | ((uint64_t)in[16] << 26) | ((uint64_t)in[17] << 34)
          | ((uint64_t)in[18] << 42) | ((uint64_t)(in[19] & 0x01) << 50);
    t[3]  = ((uint64_t)in[19] >> 1) | ((uint64_t)in[20] << 7) | ((uint64_t)in[21] << 15)
          | ((uint64_t)in[22] << 23) | ((uint64_t)in[23] << 31) | ((uint64_t)in[24] << 39)
          | ((uint64_t)(in[25] & 0x0f) << 47);
    t[4]  = ((uint64_t)in[25] >> 4) | ((uint64_t)in[26] << 4) | ((uint64_t)in[27] << 12)
          | ((uint64_t)in[28] << 20) | ((uint64_t)in[29] << 28) | ((uint64_t)in[30] << 36)
          | ((uint64_t)(in[31] & 0x7f) << 44);
    memcpy(out, t, 40);
}

static void fe_reduce(uint64_t out[5], const uint64_t in[5]) {
    uint64_t h[5];
    memcpy(h, in, 40);
    uint64_t c = h[0] >> 51; h[0] &= MASK51; h[1] += c;
    c = h[1] >> 51; h[1] &= MASK51; h[2] += c;
    c = h[2] >> 51; h[2] &= MASK51; h[3] += c;
    c = h[3] >> 51; h[3] &= MASK51; h[4] += c;
    c = h[4] >> 51; h[4] &= MASK51; h[0] += 19 * c;
    c = h[0] >> 51; h[0] &= MASK51; h[1] += c;
    /* Now h < 2^255 + 18; conditionally subtract p. */
    uint64_t q = (h[0] + 19) >> 51;
    q = (h[1] + q) >> 51;
    q = (h[2] + q) >> 51;
    q = (h[3] + q) >> 51;
    q = (h[4] + q) >> 51;
    h[0] += 19 * q;
    h[1] += h[0] >> 51; h[0] &= MASK51;
    h[2] += h[1] >> 51; h[1] &= MASK51;
    h[3] += h[2] >> 51; h[2] &= MASK51;
    h[4] += h[3] >> 51; h[3] &= MASK51;
    h[4] &= MASK51;
    memcpy(out, h, 40);
}

static void fe_tobytes(uint8_t out[32], const uint64_t in[5]) {
    uint64_t h[5];
    fe_reduce(h, in);
    out[0]  = (uint8_t)h[0];
    out[1]  = (uint8_t)(h[0] >> 8);
    out[2]  = (uint8_t)(h[0] >> 16);
    out[3]  = (uint8_t)(h[0] >> 24);
    out[4]  = (uint8_t)(h[0] >> 32);
    out[5]  = (uint8_t)(h[0] >> 40);
    out[6]  = (uint8_t)((h[0] >> 48) | (h[1] << 3));
    out[7]  = (uint8_t)(h[1] >> 5);
    out[8]  = (uint8_t)(h[1] >> 13);
    out[9]  = (uint8_t)(h[1] >> 21);
    out[10] = (uint8_t)(h[1] >> 29);
    out[11] = (uint8_t)(h[1] >> 37);
    out[12] = (uint8_t)((h[1] >> 45) | (h[2] << 6));
    out[13] = (uint8_t)(h[2] >> 2);
    out[14] = (uint8_t)(h[2] >> 10);
    out[15] = (uint8_t)(h[2] >> 18);
    out[16] = (uint8_t)(h[2] >> 26);
    out[17] = (uint8_t)(h[2] >> 34);
    out[18] = (uint8_t)(h[2] >> 42);
    out[19] = (uint8_t)((h[2] >> 50) | (h[3] << 1));
    out[20] = (uint8_t)(h[3] >> 7);
    out[21] = (uint8_t)(h[3] >> 15);
    out[22] = (uint8_t)(h[3] >> 23);
    out[23] = (uint8_t)(h[3] >> 31);
    out[24] = (uint8_t)(h[3] >> 39);
    out[25] = (uint8_t)((h[3] >> 47) | (h[4] << 4));
    out[26] = (uint8_t)(h[4] >> 4);
    out[27] = (uint8_t)(h[4] >> 12);
    out[28] = (uint8_t)(h[4] >> 20);
    out[29] = (uint8_t)(h[4] >> 28);
    out[30] = (uint8_t)(h[4] >> 36);
    out[31] = (uint8_t)(h[4] >> 44);
}

/* fe_invert — inverse via Fermat's little theorem, z^(p-2).
 *
 * Inlined into one large asm block with chained squarings. The Fermat
 * addition chain has 254 sqs split into runs of 1, 2, 1, 5, 10, 20, 10,
 * 50, 100, 50, 5 — the runs of length ≥ 2 are SQ-chained (intermediates
 * stay in registers; only the start loads from memory and only the end
 * stores back). 11 muls separate the runs and are standalone (output of
 * sq doesn't match mul input register set).
 *
 * Final result is computed into the t1 slot and copied to *out. */
static void fe_invert(uint64_t out[5], const uint64_t z[5]) {
    invert_state_t st;
    /* Copy z into st.z so the asm block can address it via [rbp+IZ_OFF]. */
    st.z[0] = z[0]; st.z[1] = z[1]; st.z[2] = z[2]; st.z[3] = z[3]; st.z[4] = z[4];

    register invert_state_t *_st asm("rbp") = &st;
    asm volatile(
        /* z2 = sq(z) */
        INV_SQ(IZ2_OFF, IZ_OFF)
        /* t  = sq^2(z2)  — chain of 2 */
        INV_SQ_LOAD(IZ2_OFF) INV_SQ_OP
        INV_SQ_RENAME INV_SQ_OP
        INV_SQ_STORE(IT_OFF)
        /* z9  = mul(t, z) */
        INV_MUL(IZ9_OFF, IT_OFF, IZ_OFF)
        /* z11 = mul(z9, z2) */
        INV_MUL(IZ11_OFF, IZ9_OFF, IZ2_OFF)
        /* t   = sq(z11) */
        INV_SQ(IT_OFF, IZ11_OFF)
        /* t0  = mul(t, z9) */
        INV_MUL(IT0_OFF, IT_OFF, IZ9_OFF)
        /* t1  = sq^5(t0) */
        INV_SQ_LOAD(IT0_OFF) INV_SQ_OP
        ".rept 4\n\t" INV_SQ_RENAME INV_SQ_OP ".endr\n\t"
        INV_SQ_STORE(IT1_OFF)
        /* t1  = mul(t1, t0) */
        INV_MUL(IT1_OFF, IT1_OFF, IT0_OFF)
        /* t2  = sq^10(t1) */
        INV_SQ_LOAD(IT1_OFF) INV_SQ_OP
        ".rept 9\n\t" INV_SQ_RENAME INV_SQ_OP ".endr\n\t"
        INV_SQ_STORE(IT2_OFF)
        /* t2  = mul(t2, t1) */
        INV_MUL(IT2_OFF, IT2_OFF, IT1_OFF)
        /* t3  = sq^20(t2) */
        INV_SQ_LOAD(IT2_OFF) INV_SQ_OP
        ".rept 19\n\t" INV_SQ_RENAME INV_SQ_OP ".endr\n\t"
        INV_SQ_STORE(IT3_OFF)
        /* t3  = mul(t3, t2) */
        INV_MUL(IT3_OFF, IT3_OFF, IT2_OFF)
        /* t3  = sq^10(t3) */
        INV_SQ_LOAD(IT3_OFF) INV_SQ_OP
        ".rept 9\n\t" INV_SQ_RENAME INV_SQ_OP ".endr\n\t"
        INV_SQ_STORE(IT3_OFF)
        /* t1  = mul(t3, t1) */
        INV_MUL(IT1_OFF, IT3_OFF, IT1_OFF)
        /* t2  = sq^50(t1) */
        INV_SQ_LOAD(IT1_OFF) INV_SQ_OP
        ".rept 49\n\t" INV_SQ_RENAME INV_SQ_OP ".endr\n\t"
        INV_SQ_STORE(IT2_OFF)
        /* t2  = mul(t2, t1) */
        INV_MUL(IT2_OFF, IT2_OFF, IT1_OFF)
        /* t3  = sq^100(t2) */
        INV_SQ_LOAD(IT2_OFF) INV_SQ_OP
        ".rept 99\n\t" INV_SQ_RENAME INV_SQ_OP ".endr\n\t"
        INV_SQ_STORE(IT3_OFF)
        /* t3  = mul(t3, t2) */
        INV_MUL(IT3_OFF, IT3_OFF, IT2_OFF)
        /* t3  = sq^50(t3) */
        INV_SQ_LOAD(IT3_OFF) INV_SQ_OP
        ".rept 49\n\t" INV_SQ_RENAME INV_SQ_OP ".endr\n\t"
        INV_SQ_STORE(IT3_OFF)
        /* t1  = mul(t3, t1) */
        INV_MUL(IT1_OFF, IT3_OFF, IT1_OFF)
        /* t1  = sq^5(t1) */
        INV_SQ_LOAD(IT1_OFF) INV_SQ_OP
        ".rept 4\n\t" INV_SQ_RENAME INV_SQ_OP ".endr\n\t"
        INV_SQ_STORE(IT1_OFF)
        /* out = mul(t1, z11)  — final result stored to IT1_OFF slot */
        INV_MUL(IT1_OFF, IT1_OFF, IZ11_OFF)
        :
        : "r"(_st)
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );

    out[0] = st.t1[0]; out[1] = st.t1[1]; out[2] = st.t1[2];
    out[3] = st.t1[3]; out[4] = st.t1[4];
}

static void scalar_clamp(uint8_t s[32]) {
    s[0] &= 248; s[31] &= 127; s[31] |= 64;
}

static void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t e[32];
    memcpy(e, scalar, 32);
    scalar_clamp(e);

    ladder_state_t st;
    fe_frombytes(st.x1, point);
    memcpy(st.x2, (const uint64_t[]){1, 0, 0, 0, 0}, 40);
    memset(st.z2, 0, 40);
    memcpy(st.x3, st.x1, 40);
    memcpy(st.z3, (const uint64_t[]){1, 0, 0, 0, 0}, 40);

    uint64_t swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (e[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(st.x2, st.x3, swap);
        fe_cswap(st.z2, st.z3, swap);
        swap = bit;
        ladder_step(&st);
    }
    fe_cswap(st.x2, st.x3, swap);
    fe_cswap(st.z2, st.z3, swap);

    fe_invert(st.z2, st.z2);
    /* Final x2 = x2 * z2 — inlined as one FE_MUL on the ladder_state. */
    {
        register ladder_state_t *_st asm("rbp") = &st;
        asm volatile(
            FE_MUL(X2_OFF, X2_OFF, Z2_OFF)
            :
            : "r"(_st)
            : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
              "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
              "memory", "cc"
        );
    }
    fe_tobytes(out, st.x2);
}

/* ════════════════════════════════════════════════════════════════════
 * Verification + bench
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
    printf("=== RFC 7748 (inline-asm 5×51 X25519, register-chained) ===\n\n");

    printf("--- Vector 1 ---\n");
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);
    x25519(r, scalar, point);
    print_hex("got     ", r, 32);
    printf("  expect: c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552\n");
    if (memcmp_hex(r, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    printf("\n--- Vector 2 ---\n");
    hex_to_bytes("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", scalar, 32);
    hex_to_bytes("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", point, 32);
    x25519(r, scalar, point);
    print_hex("got     ", r, 32);
    printf("  expect: 95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957\n");
    if (memcmp_hex(r, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    printf("\n--- Iterated test (1 round) ---\n");
    uint8_t k[32] = {0}, u[32] = {0};
    k[0] = 9; u[0] = 9;
    x25519(r, k, u);
    print_hex("got     ", r, 32);
    printf("  expect: 422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079\n");
    if (memcmp_hex(r, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    printf("\n--- Iterated test (1000 rounds) ---\n");
    uint8_t kn[32], un[32];
    memcpy(kn, k, 32); memcpy(un, u, 32);
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

    printf("\n=== %d / %d passed ===\n\n", pass, pass + fail);
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

#ifndef INLINE2_PROFILE
int main(void) {
    printf("=== X25519 via inline-asm 5×51 ladder (register-chained) ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();
    printf("\n");

    if (test_rfc7748()) {
        printf("Verification FAILED, aborting bench.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    uint8_t scalar[32], point[32], out[32];
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);

    uint64_t samples[BENCH_REPS];
    for (int i = 0; i < 5; i++) x25519(out, scalar, point);  /* warmup */

    for (int r = 0; r < BENCH_REPS; r++) {
        uint64_t t0 = rdtsc_start();
        x25519(out, scalar, point);
        uint64_t t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    qsort(samples, BENCH_REPS, sizeof(samples[0]), cmp_u64);

    printf("--- Bench (%d reps) ---\n", BENCH_REPS);
    printf("min:    %" PRIu64 " cyc\n", samples[0]);
    printf("median: %" PRIu64 " cyc\n", samples[BENCH_REPS/2]);
    printf("p90:    %" PRIu64 " cyc\n", samples[BENCH_REPS*9/10]);
    printf("\nFor reference:\n");
    printf("  5×51 microcode (production C-wrapper): ~312000 cyc\n");
    printf("  amd64-64 (SUPERCOP, asm):              ~272000 cyc\n");

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
#endif /* !INLINE2_PROFILE */

#ifdef INLINE2_PROFILE
/* ════════════════════════════════════════════════════════════════════
 * PER-OP COST PROFILER  (compiled via inline2_profile.c, which #includes
 * this file with -DINLINE2_PROFILE). Reuses the SAME patches + FE_* macros
 * as the benchmark, so the numbers are the real ladder ops.
 *
 * Method: time each field op in a DEPENDENT chain — in the ladder the field
 * ops are issued serially (each microcode firing serializes), so latency, not
 * throughput, is what matters. Then reconstruct the X25519 budget top-down:
 *     x25519 ≈ 255·ladder_step + 510·cswap + fe_invert + (un)pack
 * ════════════════════════════════════════════════════════════════════ */
#define P_UNROLL 16
#define P_REPS   1000
#define P_TRIALS 50
/* REP* (not R*) to avoid colliding with opcode.h's R0..R15 register names. */
#define REP2(x)  x x
#define REP4(x)  REP2(x) REP2(x)
#define REP8(x)  REP4(x) REP4(x)
#define REP16(x) REP8(x) REP8(x)      /* expands an op P_UNROLL=16× in one asm block */

static ladder_state_t g_st;

static void profile_init_state(void) {
    uint64_t *p = (uint64_t *)&g_st;
    for (size_t i = 0; i < sizeof(g_st) / 8; i++)
        p[i] = (0x123456789ABCDULL * (i + 1)) & MASK51;   /* valid nonzero 51-bit limbs */
}

/* Time a microcode op chained 16× per asm block, REPS×16 ops per trial. */
#define TIME_UCODE(LABEL, BODY) do {                                        \
    uint64_t _best = ~0ULL;                                                 \
    for (int _t = 0; _t < P_TRIALS; _t++) {                                 \
        register ladder_state_t *_st asm("rbp") = &g_st;                    \
        uint64_t _a = rdtsc_start();                                        \
        for (int _r = 0; _r < P_REPS; _r++) {                               \
            asm volatile( BODY : : "r"(_st)                                 \
                : "rax","rbx","rcx","rdx","rsi","rdi",                      \
                  "r8","r9","r10","r11","r12","r13","r14","r15",            \
                  "memory","cc");                                           \
        }                                                                   \
        uint64_t _c = rdtsc_end() - _a;                                     \
        if (_c < _best) _best = _c;                                         \
    }                                                                       \
    printf("  %-30s %7.2f cyc/op\n", LABEL,                                 \
           (double)_best / ((double)P_REPS * P_UNROLL));                    \
} while (0)

/* Time a native C call ITERS times. */
#define TIME_NATIVE(LABEL, CALL) do {                                       \
    uint64_t _best = ~0ULL;                                                 \
    for (int _t = 0; _t < P_TRIALS; _t++) {                                 \
        uint64_t _a = rdtsc_start();                                        \
        for (int _r = 0; _r < P_REPS * P_UNROLL; _r++) { CALL; }            \
        uint64_t _c = rdtsc_end() - _a;                                     \
        if (_c < _best) _best = _c;                                         \
    }                                                                       \
    printf("  %-30s %7.2f cyc/op\n", LABEL,                                 \
           (double)_best / ((double)P_REPS * P_UNROLL));                    \
} while (0)

/* Time a whole function call ITERS×, min of FTRIALS. */
#define TIME_FN(LABEL, CALL, ITERS, FTRIALS) do {                           \
    uint64_t _best = ~0ULL;                                                 \
    for (int _t = 0; _t < (FTRIALS); _t++) {                                \
        uint64_t _a = rdtsc_start();                                        \
        for (int _r = 0; _r < (ITERS); _r++) { CALL; }                      \
        uint64_t _c = rdtsc_end() - _a;                                     \
        if (_c < _best) _best = _c;                                         \
    }                                                                       \
    printf("  %-30s %9.1f cyc\n", LABEL, (double)_best / (ITERS));          \
} while (0)

int main(void) {
    printf("=== inline2 PER-OP PROFILER ===\n\n");
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();
    printf("\n");

    if (test_rfc7748()) {
        printf("Verification FAILED — patches not installed correctly; abort.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }
    printf("RFC 7748 OK — patches valid.\n\n");

    profile_init_state();
    printf("-- Per-op cost: dependent chain, min of %d trials (%d ops each) --\n",
           P_TRIALS, P_REPS * P_UNROLL);
    TIME_UCODE("fe_mul  (mem->mem, dep)",  REP16(FE_MUL(DA_OFF, DA_OFF, A_OFF)));
    TIME_UCODE("fe_sq   (mem->mem, dep)",  REP16(FE_SQ(AA_OFF, AA_OFF)));
    TIME_UCODE("fe_add",                   REP16(FE_ADD(A_OFF, A_OFF, Z2_OFF)));
    TIME_UCODE("fe_sub",                   REP16(FE_SUB(B_OFF, B_OFF, Z2_OFF)));
    TIME_UCODE("add->sq chain (FROM_REGS)",
               REP16(FE_ADD(A_OFF, X2_OFF, Z2_OFF) FE_SQ_FROM_REGS(AA_OFF)));
    TIME_NATIVE("mul121665 (native)", fe_mul121665_native(g_st.E, g_st.E));
    TIME_NATIVE("cswap (native)",     fe_cswap(g_st.x2, g_st.x3, (uint64_t)(_r & 1)));

    printf("\n-- Top-down: whole-op cost (dependent, min) --\n");
    profile_init_state();
    TIME_FN("ladder_step (1 full step)", ladder_step(&g_st), 2000, 50);
    profile_init_state();
    TIME_FN("fe_invert",                 fe_invert(g_st.z2, g_st.z2), 200, 50);
    {
        uint8_t scalar[32], point[32], out[32];
        hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
        hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);
        for (int i = 0; i < 5; i++) x25519(out, scalar, point);   /* warmup */
        uint64_t best = ~0ULL;
        for (int t = 0; t < 200; t++) {
            uint64_t a = rdtsc_start(); x25519(out, scalar, point);
            uint64_t c = rdtsc_end() - a; if (c < best) best = c;
        }
        printf("  %-30s %9llu cyc\n", "x25519 (full)", (unsigned long long)best);
    }

    /* Touch the state so the dependent chains above can't be optimized away. */
    volatile uint64_t sink = 0;
    for (size_t i = 0; i < sizeof(g_st) / 8; i++) sink += ((uint64_t *)&g_st)[i];
    printf("\n(state checksum: %llu)\n", (unsigned long long)sink);

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
#endif /* INLINE2_PROFILE */
