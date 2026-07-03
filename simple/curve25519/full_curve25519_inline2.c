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
 * Shared utilities (hex, timing) — used by both the benchmark build and
 * the INLINE2_PROFILE build.
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

#define BENCH_REPS 1000   /* enough samples for stable p10/p90 tails */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

#if defined(INLINE2_PROFILE) && !defined(INLINE2_LIB)
/* Minimal RFC test for the per-op profiler build (only x25519 inline). */
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
#endif /* INLINE2_PROFILE */

#if !defined(INLINE2_PROFILE) && !defined(INLINE2_LIB)
/* ════════════════════════════════════════════════════════════════════
 * CONTENDER BACKENDS — copied verbatim from full_curve25519.c so the
 * inline binary now hosts every X25519 contender. full_curve25519.c is
 * kept on disk but no longer benched (its measurement left the table).
 * The shared 5×51 field helpers (cswap/frombytes/tobytes/reduce/clamp)
 * and the microcode patches above are reused as-is.
 * ════════════════════════════════════════════════════════════════════ */

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
 * MICROCODE C-WRAPPER FIELD OPS (the non-inline "ours/ucode" backend)
 * ════════════════════════════════════════════════════════════════════ */

void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    /* Pin args to their natural SysV-ABI registers — GCC won't emit
     * extra reg-reg moves to relocate them. "+r" tells GCC the regs are
     * both input and clobbered (no need to also list in clobber set). */
    register const uint64_t *_a   asm("rdi") = a;
    register const uint64_t *_b   asm("rsi") = b;
    register       uint64_t *_out asm("rdx") = out;

    asm volatile(
        /* Stash out pointer in callee-saved rbp; survives the patch.
         * Avoids the old inner `push r15 / pop rcx` pair. */
        "mov rbp, rdx\n\t"

        /* Load b[0..4] from rsi (rsi unchanged through these). */
        "mov r15, [rsi]\n\t"
        "mov r13, [rsi + 8]\n\t"
        "mov r9,  [rsi + 16]\n\t"
        "mov r10, [rsi + 24]\n\t"
        "mov rbx, [rsi + 32]\n\t"

        /* Load a[1..4] then a[0] last (a[0] destroys rdi's pointer). */
        "mov rsi, [rdi + 8]\n\t"
        "mov r12, [rdi + 16]\n\t"
        "mov r11, [rdi + 24]\n\t"
        "mov r14, [rdi + 32]\n\t"
        "mov rdi, [rdi]\n\t"

        /* Clear accumulators */
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* Fire fe_mul microcode via vmwrite */
        "vmwrite rcx, rdx\n\t"

        /* Store 5 result limbs via rbp */
        "mov [rbp],      r15\n\t"
        "mov [rbp + 8],  r13\n\t"
        "mov [rbp + 16], r9\n\t"
        "mov [rbp + 24], r10\n\t"
        "mov [rbp + 32], rax\n\t"

        : "+r"(_a), "+r"(_b), "+r"(_out)
        :
        : "rax", "rbx", "rcx", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );
}

void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    register const uint64_t *_a   asm("rdi") = a;
    register       uint64_t *_out asm("rsi") = out;

    asm volatile(
        /* Stash out pointer in callee-saved rbp; survives the patch. */
        "mov rbp, rsi\n\t"

        /* Load a[1..4] then a[0] last (a[0] destroys rdi's pointer). */
        "mov r14, [rdi + 32]\n\t"
        "mov r11, [rdi + 24]\n\t"
        "mov r12, [rdi + 16]\n\t"
        "mov rsi, [rdi + 8]\n\t"
        "mov rdi, [rdi]\n\t"

        /* Precompute doubled (2*a_i) and reduced (19*a_i) operands */
        "lea r15, [rdi + rdi]\n\t"
        "lea r13, [rsi + rsi]\n\t"
        "lea r9,  [r12 + r12]\n\t"
        "lea r10, [r11 + r11]\n\t"
        "imul rbx, r14, 19\n\t"
        "imul rdx, r11, 19\n\t"

        /* Clear accumulators */
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* Fire fe_sq microcode via vmread (opcode: 0f 78 ca) */
        ".byte 0x0f, 0x78, 0xca\n\t"

        /* Store 5 result limbs via rbp */
        "mov [rbp],      rdi\n\t"
        "mov [rbp + 8],  r9\n\t"
        "mov [rbp + 16], r10\n\t"
        "mov [rbp + 24], rbx\n\t"
        "mov [rbp + 32], rax\n\t"

        : "+r"(_a), "+r"(_out)
        :
        : "rax", "rbx", "rcx", "rdx", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );
}

/* fe_sq_ucode_n: square `a` `n` times in place (out = a^(2^n)).
 *
 * Keeps the running result in arch registers across iterations to
 * skip the memory store/load and the precompute-from-memory cost each
 * iteration. Saves ~10 cyc/iter vs n separate fe_sq_ucode calls.
 *
 * After each patch fire, the output lives in (rdi, r9, r10, rbx, rax)
 * = (h0..h4). The next iter wants inputs in (rdi, rsi, r12, r11, r14).
 * Only rdi is already in place; 4 reg-moves stage the next iter.
 *
 * n must be >= 1. */
static void fe_sq_ucode_n(uint64_t *out, const uint64_t *a, int n) {
    register const uint64_t *_a   asm("rdi") = a;
    register       uint64_t *_out asm("rsi") = out;
    register int             _n   asm("edx") = n;

    asm volatile(
        /* Stash out ptr and counter on stack so they survive the
         * patch's clobbering of every GP register. */
        "sub rsp, 16\n\t"
        "mov [rsp],   rsi\n\t"     /* save out ptr */
        "mov [rsp+8], rdx\n\t"     /* save loop counter */

        /* Load a[1..4] then a[0] (a[0] destroys rdi). */
        "mov r14, [rdi + 32]\n\t"
        "mov r11, [rdi + 24]\n\t"
        "mov r12, [rdi + 16]\n\t"
        "mov rsi, [rdi + 8]\n\t"
        "mov rdi, [rdi]\n\t"

        "Lsqn%=:\n\t"
        /* Precompute 2*a_i and 19*a_i for the patch */
        "lea r15, [rdi + rdi]\n\t"
        "lea r13, [rsi + rsi]\n\t"
        "lea r9,  [r12 + r12]\n\t"
        "lea r10, [r11 + r11]\n\t"
        "imul rbx, r14, 19\n\t"
        "imul rdx, r11, 19\n\t"
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* Fire fe_sq (vmread, 0f 78 ca) */
        ".byte 0x0f, 0x78, 0xca\n\t"

        /* After patch: rdi=h0, r9=h1, r10=h2, rbx=h3, rax=h4 */
        /* Stage h[1..4] into the input regs for next iter.
         * (rdi already has h0; need rsi=h1, r12=h2, r11=h3, r14=h4) */
        "mov rsi, r9\n\t"
        "mov r12, r10\n\t"
        "mov r11, rbx\n\t"
        "mov r14, rax\n\t"

        "dec qword ptr [rsp+8]\n\t"
        "jnz Lsqn%=\n\t"

        /* Final result is now in rdi/rsi/r12/r11/r14 (h0..h4). */
        "mov rbp, [rsp]\n\t"
        "mov [rbp],      rdi\n\t"
        "mov [rbp + 8],  rsi\n\t"
        "mov [rbp + 16], r12\n\t"
        "mov [rbp + 24], r11\n\t"
        "mov [rbp + 32], r14\n\t"

        "add rsp, 16\n\t"

        : "+r"(_a), "+r"(_out), "+r"(_n)
        :
        : "rax", "rbx", "rcx", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );
}

/* fe_add_sq_ucode: fused (x + y) → A_out, then A_out² → AA_out.
 *
 * EXPERIMENTAL: measured break-even with separate fe_add + fe_sq_ucode on
 * Goldmont (2026-05-19). The memory roundtrip between the add and the
 * patch fire is already hidden by OoO + store-to-load forwarding while
 * the patch's ~37-cyc microcode block executes, so eliminating it via
 * a fused wrapper yields no measurable gain. Kept here as documentation
 * of the failed hypothesis; not used in the ladder.
 *
 * Register staging mirrors fe_sq_ucode after the add:
 *   A[0]=RDI, A[1]=RSI, A[2]=R12, A[3]=R11, A[4]=R14
 * Output of patch (h0..h4) goes through (RDI, R9, R10, RBX, RAX), stored to AA_out. */

/* ════════════════════════════════════════════════════════════════════
 * FIAT-CRYPTO FIELD OPERATIONS (SUPERCOP-style reference baseline)
 *
 * Both files share identical helper typedefs; C11 allows redundant
 * typedef declarations of the same type, so including both compiles
 * cleanly under -std=gnu11+. We only consume the two carry_* functions.
 * ════════════════════════════════════════════════════════════════════ */

#include "../../curvesC/curve25519_mul.c"
#include "../../curvesC/curve25519_square.c"

static inline void fe_mul_fiat(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    fiat_curve25519_carry_mul(out, a, b);
}

static inline void fe_sq_fiat(const uint64_t *a, uint64_t *out) {
    fiat_curve25519_carry_square(out, a);
}

/* CryptOpt-tuned x86-64 asm (assembled from cryptopt_{mul,sq}.asm). Same
 * fiat-style signature as the C version; symbols renamed at assemble
 * time to avoid clashing. */
extern void cryptopt_carry_mul(uint64_t out[5], const uint64_t a[5], const uint64_t b[5]);
extern void cryptopt_carry_square(uint64_t out[5], const uint64_t a[5]);

static inline void fe_mul_cryptopt(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    cryptopt_carry_mul(out, a, b);
}

static inline void fe_sq_cryptopt(const uint64_t *a, uint64_t *out) {
    cryptopt_carry_square(out, a);
}

/* ════════════════════════════════════════════════════════════════════
 * SUPERCOP donna_c64 (vendored from supercop-20260330)
 *
 * The entry point `crypto_scalarmult` in donna_c64/smult.c is renamed to
 * x25519_donna_c64 via macro substitution before #include. The static
 * helper functions (fmul, fexpand, crecip, ...) are self-contained and
 * do not clash with our identifiers. The `crypto_scalarmult.h` stub at
 * simple/include/ satisfies smult.c's only external include.
 * ════════════════════════════════════════════════════════════════════ */

#define crypto_scalarmult x25519_donna_c64
#include "../supercop-20260330/crypto_scalarmult/curve25519/donna_c64/smult.c"
#undef crypto_scalarmult

/* Forward-declare with our public name for use below. */
extern int x25519_donna_c64(unsigned char *mypublic,
                            const unsigned char *secret,
                            const unsigned char *basepoint);

/* ════════════════════════════════════════════════════════════════════
 * SUPERCOP amd64-51 (hand-tuned x86-64 assembly, Bernstein/Schwabe 2011)
 *
 * Compiled as separate .o files by the Makefile from the SUPERCOP tree.
 * The entry point `crypto_scalarmult` in mont25519.c is renamed to
 * x25519_amd64_51 via -include include/amd64_51_namespace.h. The static
 * library libcryptoint.a provides supercop_int64_optblocker.
 * ════════════════════════════════════════════════════════════════════ */

extern int x25519_amd64_51(unsigned char *out,
                           const unsigned char *scalar,
                           const unsigned char *point);

/* ════════════════════════════════════════════════════════════════════
 * SUPERCOP amd64-64 (4x64-bit saturated, Bernstein/Schwabe)
 *
 * Same author family as amd64-51 but uses radix 2^64 (saturated 4-limb
 * representation). lib25519's autotuner selects this on Goldmont — its
 * Goldmont measurement of ~280k cycles maps to this implementation,
 * not amd64-51.
 * ════════════════════════════════════════════════════════════════════ */

extern int x25519_amd64_64(unsigned char *out,
                           const unsigned char *scalar,
                           const unsigned char *point);

/* ════════════════════════════════════════════════════════════════════
 * Hybrid: amd64-51's driver/invert/pack + microcode field ops.
 *
 * Same SUPERCOP framework (mont25519/mladder/invert/pack/unpack/freeze/
 * cswap) but with ladderstep + fe25519_mul + fe25519_square replaced by
 * C versions that call our microcode wrappers. Isolates field-op cost
 * from the surrounding ladder structure — see benchmark_review.md issue
 * #1. Sources in simple/amd64-51-ucode/.
 * ════════════════════════════════════════════════════════════════════ */

extern int x25519_amd64_51_ucode(unsigned char *out,
                                 const unsigned char *scalar,
                                 const unsigned char *point);

/* ════════════════════════════════════════════════════════════════════
 * COMMON FIELD OPERATIONS (pure C, used by all backends)
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

/* constant-time conditional swap — scalar */

static inline void fe_copy(fe out, const fe a) {
    memcpy(out, a, 5 * sizeof(uint64_t));
}

/* fe_reduce: full reduction mod p = 2^255 - 19 */

static void fe_invert_native(fe out, const fe z) {
    fe z2, z9, z11, t, t0, t1, t2, t3;
    int i;

    fe_sq_native(z, z2);
    fe_sq_native(z2, t);
    fe_sq_native(t, t);
    fe_mul_native(t, z, z9);
    fe_mul_native(z9, z2, z11);
    fe_sq_native(z11, t);
    fe_mul_native(t, z9, t0);

    fe_sq_native(t0, t1);
    for (i = 1; i < 5; i++) fe_sq_native(t1, t1);
    fe_mul_native(t1, t0, t1);

    fe_sq_native(t1, t2);
    for (i = 1; i < 10; i++) fe_sq_native(t2, t2);
    fe_mul_native(t2, t1, t2);

    fe_sq_native(t2, t3);
    for (i = 1; i < 20; i++) fe_sq_native(t3, t3);
    fe_mul_native(t3, t2, t3);

    for (i = 0; i < 10; i++) fe_sq_native(t3, t3);
    fe_mul_native(t3, t1, t1);

    fe_sq_native(t1, t2);
    for (i = 1; i < 50; i++) fe_sq_native(t2, t2);
    fe_mul_native(t2, t1, t2);

    fe_sq_native(t2, t3);
    for (i = 1; i < 100; i++) fe_sq_native(t3, t3);
    fe_mul_native(t3, t2, t3);

    for (i = 0; i < 50; i++) fe_sq_native(t3, t3);
    fe_mul_native(t3, t1, t1);

    fe_sq_native(t1, t1);
    fe_sq_native(t1, t1);
    fe_sq_native(t1, t1);
    fe_sq_native(t1, t1);
    fe_sq_native(t1, t1);
    fe_mul_native(t1, z11, out);
}

static void fe_invert_ucode(fe out, const fe z) {
    fe z2, z9, z11, t, t0, t1, t2, t3;

    fe_sq_ucode(z, z2);
    fe_sq_ucode(z2, t);
    fe_sq_ucode(t, t);
    fe_mul_ucode(z, t, z9);
    fe_mul_ucode(z9, z2, z11);
    fe_sq_ucode(z11, t);
    fe_mul_ucode(z9, t, t0);

    fe_sq_ucode_n(t1, t0,   5);    /* 5 squarings */
    fe_mul_ucode(t0, t1, t1);

    fe_sq_ucode_n(t2, t1,  10);    /* 10 */
    fe_mul_ucode(t1, t2, t2);

    fe_sq_ucode_n(t3, t2,  20);    /* 20 */
    fe_mul_ucode(t2, t3, t3);

    fe_sq_ucode_n(t3, t3,  10);    /* 10 */
    fe_mul_ucode(t1, t3, t1);

    fe_sq_ucode_n(t2, t1,  50);    /* 50 */
    fe_mul_ucode(t1, t2, t2);

    fe_sq_ucode_n(t3, t2, 100);    /* 100 */
    fe_mul_ucode(t2, t3, t3);

    fe_sq_ucode_n(t3, t3,  50);    /* 50 */
    fe_mul_ucode(t1, t3, t1);

    fe_sq_ucode_n(t1, t1,   5);    /* final 5 */
    fe_mul_ucode(z11, t1, out);
}

static void fe_invert_fiat(fe out, const fe z) {
    fe z2, z9, z11, t, t0, t1, t2, t3;
    int i;

    fe_sq_fiat(z, z2);
    fe_sq_fiat(z2, t);
    fe_sq_fiat(t, t);
    fe_mul_fiat(t, z, z9);
    fe_mul_fiat(z9, z2, z11);
    fe_sq_fiat(z11, t);
    fe_mul_fiat(t, z9, t0);

    fe_sq_fiat(t0, t1);
    for (i = 1; i < 5; i++) fe_sq_fiat(t1, t1);
    fe_mul_fiat(t1, t0, t1);

    fe_sq_fiat(t1, t2);
    for (i = 1; i < 10; i++) fe_sq_fiat(t2, t2);
    fe_mul_fiat(t2, t1, t2);

    fe_sq_fiat(t2, t3);
    for (i = 1; i < 20; i++) fe_sq_fiat(t3, t3);
    fe_mul_fiat(t3, t2, t3);

    for (i = 0; i < 10; i++) fe_sq_fiat(t3, t3);
    fe_mul_fiat(t3, t1, t1);

    fe_sq_fiat(t1, t2);
    for (i = 1; i < 50; i++) fe_sq_fiat(t2, t2);
    fe_mul_fiat(t2, t1, t2);

    fe_sq_fiat(t2, t3);
    for (i = 1; i < 100; i++) fe_sq_fiat(t3, t3);
    fe_mul_fiat(t3, t2, t3);

    for (i = 0; i < 50; i++) fe_sq_fiat(t3, t3);
    fe_mul_fiat(t3, t1, t1);

    fe_sq_fiat(t1, t1);
    fe_sq_fiat(t1, t1);
    fe_sq_fiat(t1, t1);
    fe_sq_fiat(t1, t1);
    fe_sq_fiat(t1, t1);
    fe_mul_fiat(t1, z11, out);
}

static void fe_invert_cryptopt(fe out, const fe z) {
    fe z2, z9, z11, t, t0, t1, t2, t3;
    int i;

    fe_sq_cryptopt(z, z2);
    fe_sq_cryptopt(z2, t);
    fe_sq_cryptopt(t, t);
    fe_mul_cryptopt(t, z, z9);
    fe_mul_cryptopt(z9, z2, z11);
    fe_sq_cryptopt(z11, t);
    fe_mul_cryptopt(t, z9, t0);

    fe_sq_cryptopt(t0, t1);
    for (i = 1; i < 5; i++) fe_sq_cryptopt(t1, t1);
    fe_mul_cryptopt(t1, t0, t1);

    fe_sq_cryptopt(t1, t2);
    for (i = 1; i < 10; i++) fe_sq_cryptopt(t2, t2);
    fe_mul_cryptopt(t2, t1, t2);

    fe_sq_cryptopt(t2, t3);
    for (i = 1; i < 20; i++) fe_sq_cryptopt(t3, t3);
    fe_mul_cryptopt(t3, t2, t3);

    for (i = 0; i < 10; i++) fe_sq_cryptopt(t3, t3);
    fe_mul_cryptopt(t3, t1, t1);

    fe_sq_cryptopt(t1, t2);
    for (i = 1; i < 50; i++) fe_sq_cryptopt(t2, t2);
    fe_mul_cryptopt(t2, t1, t2);

    fe_sq_cryptopt(t2, t3);
    for (i = 1; i < 100; i++) fe_sq_cryptopt(t3, t3);
    fe_mul_cryptopt(t3, t2, t3);

    for (i = 0; i < 50; i++) fe_sq_cryptopt(t3, t3);
    fe_mul_cryptopt(t3, t1, t1);

    fe_sq_cryptopt(t1, t1);
    fe_sq_cryptopt(t1, t1);
    fe_sq_cryptopt(t1, t1);
    fe_sq_cryptopt(t1, t1);
    fe_sq_cryptopt(t1, t1);
    fe_mul_cryptopt(t1, z11, out);
}

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

static void x25519_fiat(uint8_t out[32], const uint8_t scalar[32],
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
        fe_sq_fiat(A, AA);
        fe_sub(B, x2, z2);
        fe_sq_fiat(B, BB);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul_fiat(D, A, DA);
        fe_mul_fiat(C, B, CB);

        fe_add(t0, DA, CB);
        fe_sq_fiat(t0, x3);

        fe_sub(t0, DA, CB);
        fe_sq_fiat(t0, z3);
        fe_mul_fiat(x1, z3, z3);

        fe_mul_fiat(AA, BB, x2);

        fe_mul121665(t0, E);
        fe_add(t0, AA, t0);
        fe_mul_fiat(E, t0, z2);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert_fiat(z2, z2);
    fe_mul_fiat(x2, z2, x2);
    fe_tobytes(out, x2);
}

static void x25519_cryptopt(uint8_t out[32], const uint8_t scalar[32],
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
        fe_sq_cryptopt(A, AA);
        fe_sub(B, x2, z2);
        fe_sq_cryptopt(B, BB);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul_cryptopt(D, A, DA);
        fe_mul_cryptopt(C, B, CB);

        fe_add(t0, DA, CB);
        fe_sq_cryptopt(t0, x3);

        fe_sub(t0, DA, CB);
        fe_sq_cryptopt(t0, z3);
        fe_mul_cryptopt(x1, z3, z3);

        fe_mul_cryptopt(AA, BB, x2);

        fe_mul121665(t0, E);
        fe_add(t0, AA, t0);
        fe_mul_cryptopt(E, t0, z2);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert_cryptopt(z2, z2);
    fe_mul_cryptopt(x2, z2, x2);
    fe_tobytes(out, x2);
}

/* ════════════════════════════════════════════════════════════════════
 * amd64-51/ucode ladderstep — REWRITTEN in the inline-asm style.
 *
 * The amd64-51-ucode hybrid (SUPERCOP amd64-51 driver/invert/pack +
 * microcode field ops) used a C ladderstep with memory-roundtrip field
 * ops. Here we replace it with the register-chained inline-asm ladder
 * (ladder_step, above) so amd64-51/ucode and ours/ucode differ only in
 * the surrounding framework, not the ladder coding style.
 *
 * mont25519.o calls this via the amd64-51-ucode namespace symbol; the C
 * ladderstep.o is excluded from this binary's link (see Makefile). work
 * is fe25519[5] = {x1,x2,z2,x3,z3}; ladder_step keeps x1 read-only and
 * writes x2,z2,x3,z3, so we copy those four back. The per-step copy is
 * hidden by store-to-load forwarding (≈45 movs vs 15 microcode firings).
 * ════════════════════════════════════════════════════════════════════ */
typedef struct { unsigned long long v[5]; } a51u_fe25519;

void supercop_amd64_51_ucode_ladderstep(a51u_fe25519 *work) {
    ladder_state_t st;
    memcpy(st.x1, work[0].v, 5 * sizeof(uint64_t));
    memcpy(st.x2, work[1].v, 5 * sizeof(uint64_t));
    memcpy(st.z2, work[2].v, 5 * sizeof(uint64_t));
    memcpy(st.x3, work[3].v, 5 * sizeof(uint64_t));
    memcpy(st.z3, work[4].v, 5 * sizeof(uint64_t));

    ladder_step(&st);

    memcpy(work[1].v, st.x2, 5 * sizeof(uint64_t));
    memcpy(work[2].v, st.z2, 5 * sizeof(uint64_t));
    memcpy(work[3].v, st.x3, 5 * sizeof(uint64_t));
    memcpy(work[4].v, st.z3, 5 * sizeof(uint64_t));
}

/* Nearest-rank percentile of an ascending-sorted array (0<=pct<=100). */
static uint64_t pctl_u64(const uint64_t *sorted, int n, double pct) {
    if (n <= 0) return 0;
    int idx = (int)(pct / 100.0 * (n - 1) + 0.5);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}
/* median is the headline; min for reference; p10/p90 for the dispersion columns. */
static void bench_stats(uint64_t *samples, int n, uint64_t *out_min, uint64_t *out_median,
                        uint64_t *out_p10, uint64_t *out_p90) {
    qsort(samples, n, sizeof(uint64_t), cmp_u64);
    *out_min    = samples[0];
    *out_median = samples[n / 2];
    *out_p10    = pctl_u64(samples, n, 10.0);
    *out_p90    = pctl_u64(samples, n, 90.0);
}

/* ════════════════════════════════════════════════════════════════════
 * RFC 7748 TEST VECTORS — verifies every contender against the spec.
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

        uint8_t kf[32], uf[32];
        memcpy(kf, k, 32);
        memcpy(uf, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_fiat(r, kf, uf);
            memcpy(uf, kf, 32);
            memcpy(kf, r, 32);
        }
        print_hex("fiat   after 1000", kf, 32);
        if (memcmp_hex(kf,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  fiat:   PASS\n"); pass++;
        } else {
            printf("  fiat:   FAIL\n"); fail++;
        }

        uint8_t kd[32], ud[32];
        memcpy(kd, k, 32);
        memcpy(ud, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_donna_c64(r, kd, ud);
            memcpy(ud, kd, 32);
            memcpy(kd, r, 32);
        }
        print_hex("donna  after 1000", kd, 32);
        if (memcmp_hex(kd,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  donna:  PASS\n"); pass++;
        } else {
            printf("  donna:  FAIL\n"); fail++;
        }

        uint8_t ka[32], ua[32];
        memcpy(ka, k, 32);
        memcpy(ua, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_amd64_51(r, ka, ua);
            memcpy(ua, ka, 32);
            memcpy(ka, r, 32);
        }
        print_hex("amd51  after 1000", ka, 32);
        if (memcmp_hex(ka,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  amd51:  PASS\n"); pass++;
        } else {
            printf("  amd51:  FAIL\n"); fail++;
        }

        uint8_t kau[32], uau[32];
        memcpy(kau, k, 32);
        memcpy(uau, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_amd64_51_ucode(r, kau, uau);
            memcpy(uau, kau, 32);
            memcpy(kau, r, 32);
        }
        print_hex("a51u   after 1000", kau, 32);
        if (memcmp_hex(kau,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  a51u:   PASS\n"); pass++;
        } else {
            printf("  a51u:   FAIL\n"); fail++;
        }

        uint8_t ka4[32], ua4[32];
        memcpy(ka4, k, 32);
        memcpy(ua4, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_amd64_64(r, ka4, ua4);
            memcpy(ua4, ka4, 32);
            memcpy(ka4, r, 32);
        }
        print_hex("amd64  after 1000", ka4, 32);
        if (memcmp_hex(ka4,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  amd64:  PASS\n"); pass++;
        } else {
            printf("  amd64:  FAIL\n"); fail++;
        }

        uint8_t kc[32], uc[32];
        memcpy(kc, k, 32);
        memcpy(uc, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_cryptopt(r, kc, uc);
            memcpy(uc, kc, 32);
            memcpy(kc, r, 32);
        }
        print_hex("crypto after 1000", kc, 32);
        if (memcmp_hex(kc,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  crypto: PASS\n"); pass++;
        } else {
            printf("  crypto: FAIL\n"); fail++;
        }

        /* The canonical inline-asm 5×51 ladder (ours/ucode in the table;
         * labeled "inline" here to distinguish from the legacy C-wrapper
         * "ucode" cross-check above). */
        uint8_t kil[32], uil[32];
        memcpy(kil, k, 32);
        memcpy(uil, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519(r, kil, uil);
            memcpy(uil, kil, 32);
            memcpy(kil, r, 32);
        }
        print_hex("inline after 1000", kil, 32);
        if (memcmp_hex(kil,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  inline: PASS\n"); pass++;
        } else {
            printf("  inline: FAIL\n"); fail++;
        }

        if (memcmp(kn, ku, 32) == 0) {
            printf("  native==ucode: PASS\n"); pass++;
        } else {
            printf("  native==ucode: FAIL\n"); fail++;
        }
        if (memcmp(kn, kf, 32) == 0) {
            printf("  native==fiat:  PASS\n"); pass++;
        } else {
            printf("  native==fiat:  FAIL\n"); fail++;
        }
        if (memcmp(kn, kd, 32) == 0) {
            printf("  native==donna: PASS\n"); pass++;
        } else {
            printf("  native==donna: FAIL\n"); fail++;
        }
        if (memcmp(kn, ka, 32) == 0) {
            printf("  native==amd51: PASS\n"); pass++;
        } else {
            printf("  native==amd51: FAIL\n"); fail++;
        }
        if (memcmp(kn, kau, 32) == 0) {
            printf("  native==a51u:  PASS\n"); pass++;
        } else {
            printf("  native==a51u:  FAIL\n"); fail++;
        }
        if (memcmp(kn, ka4, 32) == 0) {
            printf("  native==amd64: PASS\n"); pass++;
        } else {
            printf("  native==amd64: FAIL\n"); fail++;
        }
        if (memcmp(kn, kc, 32) == 0) {
            printf("  native==crypto: PASS\n"); pass++;
        } else {
            printf("  native==crypto: FAIL\n"); fail++;
        }
        if (memcmp(kn, kil, 32) == 0) {
            printf("  native==inline: PASS\n"); pass++;
        } else {
            printf("  native==inline: FAIL\n"); fail++;
        }
    }

    printf("\n=== RFC 7748: %d passed, %d failed ===\n\n", pass, fail);
    return fail;
}

/* ════════════════════════════════════════════════════════════════════
 * BENCHMARK — every contender (amd64-64/ucode lives in its own binary)
 * ════════════════════════════════════════════════════════════════════ */
static void benchmark(void) {
    uint8_t scalar[32] = {0}, point[32] = {0}, out[32];
    uint64_t t0, t1, mn, med, p10, p90;
    uint64_t samples[BENCH_REPS];

    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);

    printf("=== X25519 Benchmark (%d repetitions) ===\n\n", BENCH_REPS);

    /* Warm up every contender. */
    x25519_native(out, scalar, point);
    x25519_fiat(out, scalar, point);
    x25519_cryptopt(out, scalar, point);
    x25519_donna_c64(out, scalar, point);
    x25519_amd64_51(out, scalar, point);
    x25519_amd64_51_ucode(out, scalar, point);
    x25519_amd64_64(out, scalar, point);
    x25519_ucode(out, scalar, point);
    x25519(out, scalar, point);

#define BENCH_ONE(LABEL, CALL) do {                                         \
        for (int r = 0; r < BENCH_REPS; r++) {                              \
            t0 = rdtsc_start(); CALL; t1 = rdtsc_end();                     \
            samples[r] = t1 - t0;                                           \
        }                                                                   \
        bench_stats(samples, BENCH_REPS, &mn, &med, &p10, &p90);            \
        printf("%-20s median %8" PRIu64 "  min %8" PRIu64 "  p10 %8" PRIu64 \
               "  p90 %8" PRIu64 " cycles\n", LABEL, med, mn, p10, p90);    \
    } while (0)

    BENCH_ONE("ours/hand-C:",       x25519_native(out, scalar, point));
    BENCH_ONE("ours/fiat:",         x25519_fiat(out, scalar, point));
    BENCH_ONE("ours/cryptopt:",     x25519_cryptopt(out, scalar, point));
    BENCH_ONE("donna_c64:",         x25519_donna_c64(out, scalar, point));
    BENCH_ONE("amd64-51/asm:",      x25519_amd64_51(out, scalar, point));
    BENCH_ONE("amd64-51/ucode:",    x25519_amd64_51_ucode(out, scalar, point));
    BENCH_ONE("amd64-64/asm:",      x25519_amd64_64(out, scalar, point));
    /* "ours/ucode" is the inline-asm register-chained ladder (the canonical
     * implementation). "ucode/C-ladder" is the SAME microcode field ops on
     * the identical C ladder as ours/hand-C/fiat/cryptopt — it exists only
     * to isolate the field-op backend end-to-end (the same-ladder field-op
     * swap table), NOT as a headline contender. */
    BENCH_ONE("ours/ucode:",        x25519(out, scalar, point));
    BENCH_ONE("ucode/C-ladder:",    x25519_ucode(out, scalar, point));
#undef BENCH_ONE

    printf("\n");
}

/* ════════════════════════════════════════════════════════════════════
 * MAIN — the inline-asm 5×51 ladder (x25519) is the canonical "ours".
 * ════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("=== Full X25519: every contender (inline-asm 5×51 is canonical ours) ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();
    printf("\n");

    int failures = test_rfc7748();
    if (failures) {
        printf("Test vectors FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    benchmark();

    init_match_and_patch();
    do_fix_IN_patch();
    printf("Done.\n");
    return 0;
}
#endif /* !INLINE2_PROFILE */

#if defined(INLINE2_PROFILE) && !defined(INLINE2_LIB)
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
