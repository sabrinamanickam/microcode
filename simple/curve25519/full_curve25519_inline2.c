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
#include "freq_guard.h"

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

/* fe_sq wrapper contract, selectable so the parked five-accumulator patch
 * can be TESTED without changing what production ships. Undefined (the
 * default) this is byte-identical to the shipped wrapper: the serial fe_sq
 * uses R8 as a zero-initialised accumulator. Defined, R8 carries 2^51-1 as
 * the AND mask that sq_patch_5acc wants.
 *   make PROG=<test> EXTRA_CPPFLAGS="-DSQ_MASK_R8 -DENABLE_SQ_5ACC" */
#ifdef SQ_MASK_R8
#define FE_SQ_R8 "mov r8, 0x7FFFFFFFFFFFF\n\t"
#else
#define FE_SQ_R8 "xor r8d, r8d\n\t"
#endif

/* _IMUL64L_DSZ64 (0x264) is in opcode.h but inst.h generates no macro for it.
 * It is a NON-DESTRUCTIVE 64-bit low multiply: both sources survive, unlike
 * MUL_DSZ64_*, whose srcB receives the low half. Verified on hardware by
 * probe_opsem entries [2] and [3] (probe_opsem_out.txt).
 * ucode_sim.py and ucode_critpath.py both understand these names. */
#define IMUL64L_DSZ64_DRR(d, a, b) (_IMUL64L_DSZ64 | INSTR_DRR(d, a, b))
#define IMUL64L_DSZ64_DRI(d, a, i) (_IMUL64L_DSZ64 | INSTR_DRI(d, a, i))

/* ════════════════════════════════════════════════════════════════════
 * MICROCODE PATCH INSTALLATION
 *
 * fe_mul: five independent 128-bit accumulators (PLAN_kernel_optimization.md
 * section 5.0), replacing the single serial accumulator and its
 * 0->1->2->3->4 carry chain. Structure follows OpenSSL x25519_fe51_mul:
 *
 *   PREP      g_j = 19*b_j for j=1..4, one non-destructive IMUL64L each.
 *   row 0     a0*b_j initialises accumulator j. MUL writes its low half into
 *             srcB, so staging b_j into lo_j IS the initialisation: no ADD
 *             and no SETCC for the first product of each limb (PLAN 5.1B).
 *   rows 1-4  the other 20 products accumulate with NO inter-limb carry
 *             propagation. Each accumulator runs its own SETCC chain --
 *             domain #1 flags are PER-REGISTER, so five chains coexist and
 *             stay mutually independent. One accumulate is
 *                 ADD(lo_j, lo_j, lo_p)   SETCC(c, lo_j)
 *                 ADD(hi_p, hi_p, c)      ADD(hi_j, hi_j, hi_p)
 *             folding the carry into the product's high half rather than
 *             into hi_j, which halves the hi_j chain (4 deep, not 8).
 *
 *             This is 4 ops where the GENARITHFLAGS carry bridge would be
 *             3 (ADD / GFL_RR(lo_j,lo_j) / ADC), and the bridge was built
 *             and MEASURED: 52 triads, 154 ops, and 104.0 cyc against this
 *             version's 102.8. It loses because all 20 bridged carries
 *             funnel through the ONE architectural CF, so they serialise,
 *             where five SETCC domains do not. Per-register flags are the
 *             whole reason the parallel structure pays off; do not "save"
 *             the fourth op. See PLAN 5.0-lever-1.
 *   reduce    two fully parallel passes, NOT OpenSSL's partial tree.
 *             OpenSSL propagates each carry into the 128-bit accumulator
 *             (add+adc, two instructions); in microcode that costs
 *             ADD+SETCC+ADD. Splitting every accumulator into
 *                 r_j = lo_j & MASK        q_j = (lo_j>>51)|(hi_j<<13)
 *             first makes every carry add a plain 64-bit ADD, and all five
 *             splits issue in parallel. Pass 1 lands t_j < 2^63.6, pass 2
 *             lands every limb at < 2^51 + 2^17.
 *
 * INPUT BOUND: limbs must be < 2^54. The binding constraint is
 * 19*q_4 < 2^64 with q_4 = acc_4 >> 51 and acc_4 < 5*2^(2L), which needs
 * L < 54.2; hi_j<<13 < 2^64 needs L < 54.4. The ladder feeds this at most
 * 2^53.1 (FE_SUB's 2p bias on top of a < 2^52 limb).
 *
 * The wrapper must pass 2^51-1 in RCX (see FE_MUL): the patch masks with a
 * single AND against that register instead of a SHL13/SHR13 pair (PLAN
 * 5.1E). RCX was free -- the old patch used it as MUL's high destination,
 * and MUL's destination is free to be any register.
 *
 * fe_sq: UNCHANGED, still the serial single-accumulator design at 42
 * triads / 81.8 cyc. The five-accumulator rewrite is parked below as
 * sq_patch_5acc: verified correct, but it hard-reset the machine three
 * times from inside fe_invert_ucode and the cause was never found. Its
 * wrapper contract (2^51-1 in R8) has been reverted with it, so every
 * fe_sq firing site zeroes R8 again as the serial patch expects.
 *
 * Register map (fe_mul):
 *   a0..a4  RDI RSI R12 R11 R14   always MUL srcA, so preserved
 *   b0..b4  R15 R13 R9  R10 RBX   from the wrapper, consumed as srcB
 *   g1..g4  TMP0..TMP3            19*b_j
 *   lo0..4  TMP4..TMP8            MUST be TMP: SETCC reads domain #1
 *   hi0..4  TMP9..TMP13
 *   mask    RCX
 *   carry   TMP14 TMP15 (alternating SETCC destinations)
 *   scratch RAX R8 RDX, plus every register as it dies: RBX and RDI after
 *           row 0, then b_j and a_i as their last product is issued.
 * ════════════════════════════════════════════════════════════════════ */

static void install_field_patches(void) {
    ucode_t mul_patch[] = {
    /* PREP: g_j = 19*b_j, non-destructive, both sources survive */
    { IMUL64L_DSZ64_DRI(TMP3, RBX, 19), IMUL64L_DSZ64_DRI(TMP2, R10, 19),
      IMUL64L_DSZ64_DRI(TMP1, R9, 19), NOP_SEQWORD },
    /* row 0: a0*b_j initialises accumulator j -- no ADD, no SETCC */
    { IMUL64L_DSZ64_DRI(TMP0, R13, 19), ZEROEXT_DSZ64_DR(TMP4, R15),
      MUL_DSZ64_DRR(TMP9, RDI, TMP4), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP5, R13), MUL_DSZ64_DRR(TMP10, RDI, TMP5),
      ZEROEXT_DSZ64_DR(TMP6, R9), NOP_SEQWORD },
    { MUL_DSZ64_DRR(TMP11, RDI, TMP6), ZEROEXT_DSZ64_DR(TMP7, R10),
      MUL_DSZ64_DRR(TMP12, RDI, TMP7), NOP_SEQWORD },
    /* row 1: a1 x b, five independent accumulates */
    { ZEROEXT_DSZ64_DR(TMP8, RBX), MUL_DSZ64_DRR(TMP13, RDI, TMP8),
      ZEROEXT_DSZ64_DR(RAX, R15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(R8, RSI, RAX), ZEROEXT_DSZ64_DR(RDX, R13),
      MUL_DSZ64_DRR(RBX, RSI, RDX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, TMP5, RAX), SETCC_CONDB_DR(TMP14, TMP5),
      ADD_DSZ64_DRR(R8, R8, TMP14), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP10, TMP10, R8), ZEROEXT_DSZ64_DR(RDI, R9),
      MUL_DSZ64_DRR(RAX, RSI, RDI), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP6, TMP6, RDX), SETCC_CONDB_DR(TMP15, TMP6),
      ADD_DSZ64_DRR(RBX, RBX, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP11, TMP11, RBX), MUL_DSZ64_DRR(R8, RSI, R10),
      ADD_DSZ64_DRR(TMP7, TMP7, RDI), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP14, TMP7), ADD_DSZ64_DRR(RAX, RAX, TMP14),
      ADD_DSZ64_DRR(TMP12, TMP12, RAX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RDX, TMP3), MUL_DSZ64_DRR(RBX, RSI, RDX),
      ADD_DSZ64_DRR(TMP8, TMP8, R10), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP8), ADD_DSZ64_DRR(R8, R8, TMP15),
      ADD_DSZ64_DRR(TMP13, TMP13, R8), NOP_SEQWORD },
    /* row 2: a2 x b, five independent accumulates */
    { ZEROEXT_DSZ64_DR(RDI, R15), MUL_DSZ64_DRR(RSI, R12, RDI),
      ADD_DSZ64_DRR(TMP4, TMP4, RDX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP14, TMP4), ADD_DSZ64_DRR(RBX, RBX, TMP14),
      ADD_DSZ64_DRR(TMP9, TMP9, RBX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R10, R13), MUL_DSZ64_DRR(RAX, R12, R10),
      ADD_DSZ64_DRR(TMP6, TMP6, RDI), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP6), ADD_DSZ64_DRR(RSI, RSI, TMP15),
      ADD_DSZ64_DRR(TMP11, TMP11, RSI), NOP_SEQWORD },
    { MUL_DSZ64_DRR(R8, R12, R9), ADD_DSZ64_DRR(TMP7, TMP7, R10),
      SETCC_CONDB_DR(TMP14, TMP7), NOP_SEQWORD },
    { ADD_DSZ64_DRR(RAX, RAX, TMP14), ADD_DSZ64_DRR(TMP12, TMP12, RAX),
      ZEROEXT_DSZ64_DR(RDX, TMP2), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RBX, R12, RDX), ADD_DSZ64_DRR(TMP8, TMP8, R9),
      SETCC_CONDB_DR(TMP15, TMP8), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP15), ADD_DSZ64_DRR(TMP13, TMP13, R8),
      ZEROEXT_DSZ64_DR(RDI, TMP3), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RSI, R12, RDI), ADD_DSZ64_DRR(TMP4, TMP4, RDX),
      SETCC_CONDB_DR(TMP14, TMP4), NOP_SEQWORD },
    /* row 3: a3 x b, five independent accumulates */
    { ADD_DSZ64_DRR(RBX, RBX, TMP14), ADD_DSZ64_DRR(TMP9, TMP9, RBX),
      ZEROEXT_DSZ64_DR(R10, R15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(R9, R11, R10), ADD_DSZ64_DRR(TMP5, TMP5, RDI),
      SETCC_CONDB_DR(TMP15, TMP5), NOP_SEQWORD },
    { ADD_DSZ64_DRR(RSI, RSI, TMP15), ADD_DSZ64_DRR(TMP10, TMP10, RSI),
      MUL_DSZ64_DRR(R12, R11, R13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP7, TMP7, R10), SETCC_CONDB_DR(TMP14, TMP7),
      ADD_DSZ64_DRR(R9, R9, TMP14), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP12, TMP12, R9), ZEROEXT_DSZ64_DR(RAX, TMP1),
      MUL_DSZ64_DRR(R8, R11, RAX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP8, TMP8, R13), SETCC_CONDB_DR(TMP15, TMP8),
      ADD_DSZ64_DRR(R12, R12, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP13, TMP13, R12), ZEROEXT_DSZ64_DR(R13, TMP2),
      MUL_DSZ64_DRR(RDX, R11, R13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, TMP4, RAX), SETCC_CONDB_DR(TMP14, TMP4),
      ADD_DSZ64_DRR(R8, R8, TMP14), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP9, TMP9, R8), ZEROEXT_DSZ64_DR(RBX, TMP3),
      MUL_DSZ64_DRR(RDI, R11, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, TMP5, R13), SETCC_CONDB_DR(TMP15, TMP5),
      ADD_DSZ64_DRR(RDX, RDX, TMP15), NOP_SEQWORD },
    /* row 4: a4 x b, five independent accumulates */
    { ADD_DSZ64_DRR(TMP10, TMP10, RDX), MUL_DSZ64_DRR(RDX, R14, R15),
      ADD_DSZ64_DRR(TMP6, TMP6, RBX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP14, TMP6), ADD_DSZ64_DRR(RDI, RDI, TMP14),
      ADD_DSZ64_DRR(TMP11, TMP11, RDI), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RBX, R14, TMP0), ADD_DSZ64_DRR(TMP8, TMP8, R15),
      SETCC_CONDB_DR(TMP15, TMP8), NOP_SEQWORD },
    { ADD_DSZ64_DRR(RDX, RDX, TMP15), ADD_DSZ64_DRR(TMP13, TMP13, RDX),
      MUL_DSZ64_DRR(R8, R14, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP0), SETCC_CONDB_DR(TMP14, TMP4),
      ADD_DSZ64_DRR(RBX, RBX, TMP14), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP9, TMP9, RBX), MUL_DSZ64_DRR(TMP0, R14, TMP2),
      ADD_DSZ64_DRR(TMP5, TMP5, TMP1), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP5), ADD_DSZ64_DRR(R8, R8, TMP15),
      ADD_DSZ64_DRR(TMP10, TMP10, R8), NOP_SEQWORD },
    { MUL_DSZ64_DRR(R15, R14, TMP3), ADD_DSZ64_DRR(TMP6, TMP6, TMP2),
      SETCC_CONDB_DR(TMP14, TMP6), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP14), ADD_DSZ64_DRR(TMP11, TMP11, TMP0),
      ADD_DSZ64_DRR(TMP7, TMP7, TMP3), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP7), ADD_DSZ64_DRR(R15, R15, TMP15),
      ADD_DSZ64_DRR(TMP12, TMP12, R15), NOP_SEQWORD },
    /* reduce pass 1: split all five accs, r_j = acc_j & M, q_j = acc_j >> 51 */
    { SHL_DSZ64_DRI(TMP9, TMP9, 13), SHR_DSZ64_DRI(TMP0, TMP4, 51),
      AND_DSZ64_DRR(TMP4, TMP4, RCX), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP9, TMP0, TMP9), SHL_DSZ64_DRI(TMP10, TMP10, 13),
      SHR_DSZ64_DRI(TMP1, TMP5, 51), NOP_SEQWORD },
    { AND_DSZ64_DRR(TMP5, TMP5, RCX), OR_DSZ64_DRR(TMP10, TMP1, TMP10),
      SHL_DSZ64_DRI(TMP11, TMP11, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP2, TMP6, 51), AND_DSZ64_DRR(TMP6, TMP6, RCX),
      OR_DSZ64_DRR(TMP11, TMP2, TMP11), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP12, TMP12, 13), SHR_DSZ64_DRI(TMP0, TMP7, 51),
      AND_DSZ64_DRR(TMP7, TMP7, RCX), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP12, TMP0, TMP12), SHL_DSZ64_DRI(TMP13, TMP13, 13),
      SHR_DSZ64_DRI(TMP1, TMP8, 51), NOP_SEQWORD },
    /* t_j = r_j + q_{j-1}, and t_0 = r_0 + 19*q_4 */
    { AND_DSZ64_DRR(TMP8, TMP8, RCX), OR_DSZ64_DRR(TMP13, TMP1, TMP13),
      ADD_DSZ64_DRR(TMP5, TMP5, TMP9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP6, TMP6, TMP10), ADD_DSZ64_DRR(TMP7, TMP7, TMP11),
      ADD_DSZ64_DRR(TMP8, TMP8, TMP12), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP3, TMP13, 4), ADD_DSZ64_DRR(TMP14, TMP13, TMP13),
      ADD_DSZ64_DRR(TMP3, TMP3, TMP14), NOP_SEQWORD },
    /* reduce pass 2: same split again, t_j < 2^63.6 -> limbs < 2^51 + 2^17 */
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP13), ADD_DSZ64_DRR(TMP4, TMP4, TMP3),
      SHR_DSZ64_DRI(TMP9, TMP4, 51), NOP_SEQWORD },
    { AND_DSZ64_DRR(TMP4, TMP4, RCX), SHR_DSZ64_DRI(TMP10, TMP5, 51),
      AND_DSZ64_DRR(TMP5, TMP5, RCX), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP11, TMP6, 51), AND_DSZ64_DRR(TMP6, TMP6, RCX),
      SHR_DSZ64_DRI(TMP12, TMP7, 51), NOP_SEQWORD },
    { AND_DSZ64_DRR(TMP7, TMP7, RCX), SHR_DSZ64_DRI(TMP13, TMP8, 51),
      AND_DSZ64_DRR(TMP8, TMP8, RCX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R13, TMP5, TMP9), ADD_DSZ64_DRR(R9, TMP6, TMP10),
      ADD_DSZ64_DRR(R10, TMP7, TMP11), NOP_SEQWORD },
    { ADD_DSZ64_DRR(RAX, TMP8, TMP12), SHL_DSZ64_DRI(TMP3, TMP13, 4),
      ADD_DSZ64_DRR(TMP14, TMP13, TMP13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP14), ADD_DSZ64_DRR(TMP3, TMP3, TMP13),
      ADD_DSZ64_DRR(R15, TMP4, TMP3), END_SEQWORD },
    };


#if 0
/* Previous fe_mul: ONE 128-bit accumulator (TMP0 lo / R8 hi) with a serial
 * 0->1->2->3->4 inter-limb carry chain. 66 triads, 196 ops, dependency
 * depth 74.9 by lib/ucode_critpath.py, 122.9 cyc in bench_kernel. Kept
 * compiled out but still parseable, so the A/B stays reproducible:
 *   python3 lib/ucode_sim.py      full_curve25519_inline2.c mul_patch_serial mul
 *   python3 lib/ucode_critpath.py full_curve25519_inline2.c mul_patch_serial
 * It does not need RCX preloaded with the mask; it uses SHL13/SHR13. */
static const ucode_t mul_patch_serial[] = {
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
#endif

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


#ifdef ENABLE_SQ_5ACC
/* Five-accumulator fe_sq, 40 triads, 112 ops, depth 20.5, predicted 68.3 cyc
 * against OpenSSL's 73.2. NOT SHIPPED: it is CORRECT -- it matched
 * fiat-crypto on every path where a result could be checked, including
 * inputs to 2^53-1, standalone, through both macros, mixed with fe_mul in
 * one asm block, and looped to n=100 -- but fe_invert_ucode, which is
 * nothing but those same calls in sequence, hard-resets the machine. Three
 * boots were spent localising that and the cause is still unknown; see
 * PLAN_kernel_optimization.md section 3. Requires 2^51-1 in R8, which the
 * wrapper no longer supplies, so simulate it with:
 *   python3 lib/ucode_sim.py full_curve25519_inline2.c sq_patch_5acc sq --mask-r8
 * Regenerate with lib/gen_sq_patch.py. Do not ship without an explanation
 * for the reset. */
static const ucode_t sq_patch_5acc[] = {
    { ZEROEXT_DSZ64_DR(TMP4, RDX), MUL_DSZ64_DRR(TMP9, R9, TMP4),
      ZEROEXT_DSZ64_DR(TMP5, RSI), NOP_SEQWORD },
    { MUL_DSZ64_DRR(TMP10, R15, TMP5), ZEROEXT_DSZ64_DR(TMP6, R12),
      NOP, NOP_SEQWORD },
    { MUL_DSZ64_DRR(TMP11, R15, TMP6), ZEROEXT_DSZ64_DR(TMP7, R11),
      NOP, NOP_SEQWORD },
    { MUL_DSZ64_DRR(TMP12, R15, TMP7), ZEROEXT_DSZ64_DR(TMP8, R14),
      NOP, NOP_SEQWORD },
    { MUL_DSZ64_DRR(TMP13, R15, TMP8), NOP,
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R15, RDI), MUL_DSZ64_DRR(RAX, R15, RDI),
      ZEROEXT_DSZ64_DR(TMP2, RSI), NOP_SEQWORD },
    { MUL_DSZ64_DRR(TMP0, R11, RDX), ADD_DSZ64_DRR(TMP4, TMP4, RDI),
      SETCC_CONDB_DR(TMP14, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(RAX, RAX, TMP14), ADD_DSZ64_DRR(TMP9, TMP9, RAX),
      MUL_DSZ64_DRR(TMP1, TMP2, RSI), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, TMP5, RDX), SETCC_CONDB_DR(TMP15, TMP5),
      ADD_DSZ64_DRR(TMP0, TMP0, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP10, TMP10, TMP0), ZEROEXT_DSZ64_DR(TMP2, R12),
      MUL_DSZ64_DRR(TMP3, R13, TMP2), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP6, TMP6, RSI), SETCC_CONDB_DR(TMP14, TMP6),
      ADD_DSZ64_DRR(TMP1, TMP1, TMP14), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP11, TMP11, TMP1), MUL_DSZ64_DRR(RDI, R13, R11),
      ADD_DSZ64_DRR(TMP7, TMP7, TMP2), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP7), ADD_DSZ64_DRR(TMP3, TMP3, TMP15),
      ADD_DSZ64_DRR(TMP12, TMP12, TMP3), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RDX, RBX), MUL_DSZ64_DRR(RSI, R13, RDX),
      ADD_DSZ64_DRR(TMP8, TMP8, R11), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP14, TMP8), ADD_DSZ64_DRR(RDI, RDI, TMP14),
      ADD_DSZ64_DRR(TMP13, TMP13, RDI), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R11, RBX), MUL_DSZ64_DRR(RAX, R9, R11),
      ADD_DSZ64_DRR(TMP4, TMP4, RDX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP4), ADD_DSZ64_DRR(RSI, RSI, TMP15),
      ADD_DSZ64_DRR(TMP9, TMP9, RSI), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP0, RBX), MUL_DSZ64_DRR(TMP1, R10, TMP0),
      ADD_DSZ64_DRR(TMP5, TMP5, R11), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP14, TMP5), ADD_DSZ64_DRR(RAX, RAX, TMP14),
      ADD_DSZ64_DRR(TMP10, TMP10, RAX), NOP_SEQWORD },
    { MUL_DSZ64_DRR(TMP2, R14, RBX), ADD_DSZ64_DRR(TMP6, TMP6, TMP0),
      SETCC_CONDB_DR(TMP15, TMP6), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R15, R12), NOP,
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP1, TMP1, TMP15), ADD_DSZ64_DRR(TMP11, TMP11, TMP1),
      MUL_DSZ64_DRR(TMP3, R15, R12), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP7, TMP7, RBX), SETCC_CONDB_DR(TMP14, TMP7),
      ADD_DSZ64_DRR(TMP2, TMP2, TMP14), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP12, TMP12, TMP2), ADD_DSZ64_DRR(TMP8, TMP8, R12),
      SETCC_CONDB_DR(TMP15, TMP8), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP15), ADD_DSZ64_DRR(TMP13, TMP13, TMP3),
      SHL_DSZ64_DRI(TMP9, TMP9, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, TMP4, 51), AND_DSZ64_DRR(TMP4, TMP4, R8),
      OR_DSZ64_DRR(TMP9, TMP0, TMP9), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP10, TMP10, 13), SHR_DSZ64_DRI(TMP1, TMP5, 51),
      AND_DSZ64_DRR(TMP5, TMP5, R8), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP10, TMP1, TMP10), SHL_DSZ64_DRI(TMP11, TMP11, 13),
      SHR_DSZ64_DRI(TMP2, TMP6, 51), NOP_SEQWORD },
    { AND_DSZ64_DRR(TMP6, TMP6, R8), OR_DSZ64_DRR(TMP11, TMP2, TMP11),
      SHL_DSZ64_DRI(TMP12, TMP12, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, TMP7, 51), AND_DSZ64_DRR(TMP7, TMP7, R8),
      OR_DSZ64_DRR(TMP12, TMP0, TMP12), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP13, TMP13, 13), SHR_DSZ64_DRI(TMP1, TMP8, 51),
      AND_DSZ64_DRR(TMP8, TMP8, R8), NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP13, TMP1, TMP13), ADD_DSZ64_DRR(TMP5, TMP5, TMP9),
      ADD_DSZ64_DRR(TMP6, TMP6, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP7, TMP7, TMP11), ADD_DSZ64_DRR(TMP8, TMP8, TMP12),
      SHL_DSZ64_DRI(TMP3, TMP13, 4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP14, TMP13, TMP13), ADD_DSZ64_DRR(TMP3, TMP3, TMP14),
      ADD_DSZ64_DRR(TMP3, TMP3, TMP13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, TMP4, TMP3), SHR_DSZ64_DRI(TMP9, TMP4, 51),
      AND_DSZ64_DRR(TMP4, TMP4, R8), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP10, TMP5, 51), AND_DSZ64_DRR(TMP5, TMP5, R8),
      SHR_DSZ64_DRI(TMP11, TMP6, 51), NOP_SEQWORD },
    { AND_DSZ64_DRR(TMP6, TMP6, R8), SHR_DSZ64_DRI(TMP12, TMP7, 51),
      AND_DSZ64_DRR(TMP7, TMP7, R8), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP13, TMP8, 51), AND_DSZ64_DRR(TMP8, TMP8, R8),
      ADD_DSZ64_DRR(R9, TMP5, TMP9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R10, TMP6, TMP10), ADD_DSZ64_DRR(RBX, TMP7, TMP11),
      ADD_DSZ64_DRR(RAX, TMP8, TMP12), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP3, TMP13, 4), ADD_DSZ64_DRR(TMP14, TMP13, TMP13),
      ADD_DSZ64_DRR(TMP3, TMP3, TMP14), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP13), ADD_DSZ64_DRR(RDI, TMP4, TMP3),
      NOP, END_SEQWORD },
};
#endif

    /* Patch RAM: 128 triads from U7c00, 4 address units each. U7de0-U7df0 is
     * the lib-micro bootstrap staging area, dead only because the helpers run
     * before patch_ucode here (project note patch-ram-bootstrap-reclaim).
     * 58 + 42 = 100 triads, ending at U7d90. */
    _Static_assert(ARRAY_SZ(mul_patch) + ARRAY_SZ(sq_patch) <= 128,
                   "fe_mul + fe_sq exceed the 128-triad patch RAM budget");

    patch_ucode(0x7c00, mul_patch, ARRAY_SZ(mul_patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    uint64_t sq_addr = 0x7c00 + ARRAY_SZ(mul_patch) * 4;
    patch_ucode(sq_addr, sq_patch, ARRAY_SZ(sq_patch));
    hook_match_and_patch(1, 0x0618, sq_addr);
#ifdef ENABLE_SQ_5ACC
    /* Test builds only: overwrite the fe_sq region with the parked
     * five-accumulator patch, at the same address, so a probe can study the
     * thing that resets the machine without production carrying it. Needs
     * -DSQ_MASK_R8 too, since this patch wants 2^51-1 in R8. */
    patch_ucode(sq_addr, (ucode_t *)sq_patch_5acc, ARRAY_SZ(sq_patch_5acc));
    hook_match_and_patch(1, 0x0618, sq_addr);
    printf("fe_sq : REPLACED by parked five-accumulator patch, %d triads\n",
           (int)ARRAY_SZ(sq_patch_5acc));
#endif
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
 * stores h[0..4] to [rbp+out].
 *
 * rcx carries 2^51-1: the patch masks each limb with one AND against it
 * instead of a SHL13/SHR13 pair. One extra caller instruction (~0.3 cyc of
 * invocation floor) for 7 fewer patch ops. rcx was already free — it is the
 * vmwrite operand and the old patch only used it as MUL's high destination.
 *
 * The load and store order is load-bearing: both run limb 0 -> limb 4, and
 * a descending load order puts the first load of one op one instruction
 * after the last store of the previous op, which costs ~30 cyc on Goldmont
 * (probe_ldorder; see the note above FE_SQ). Do not reorder the memory ops.
 * xor eax/r8d stay so every register the patch could read is defined, which
 * keeps lib/ucode_sim.py's zero-filled model faithful to the hardware. */
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
    "mov rcx, 0x7FFFFFFFFFFFF\n\t"       \
    "vmwrite rcx, rdx\n\t"               \
    "mov [rbp + " S(out) " + 0],  r15\n\t" \
    "mov [rbp + " S(out) " + 8],  r13\n\t" \
    "mov [rbp + " S(out) " + 16], r9\n\t"  \
    "mov [rbp + " S(out) " + 24], r10\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

/* FE_SQ(out, a) — fires the sq patch (vmread). Precompute 2*a and 19*a
 * happen inline. Output: rdi=h0, r9=h1, r10=h2, rbx=h3, rax=h4.
 *
 * The five loads MUST run limb 0 -> limb 4, matching the store order below.
 * They used to run 4 -> 0. Stores are ascending, so a descending load order
 * makes the first load of one op read [x+32] one instruction after the last
 * store of the previous op wrote it; that store-to-load distance of 1 costs
 * ~30 cyc on Goldmont. Ascending puts each load five instructions after its
 * producing store and the stall disappears: measured 122.3 -> 81.8 cyc per
 * dependent fe_sq, with the patch untouched (probe_ldorder, arms A2/A3/B1/B2).
 * Do not "tidy" these back into descending order. */
#define FE_SQ(out, a) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  \
    "mov rsi, [rbp + " S(a) " + 8]\n\t"  \
    "mov r12, [rbp + " S(a) " + 16]\n\t" \
    "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t" \
    "lea r15, [rdi + rdi]\n\t"           \
    "lea r13, [rsi + rsi]\n\t"           \
    "lea r9,  [r12 + r12]\n\t"           \
    "lea r10, [r11 + r11]\n\t"           \
    "imul rbx, r14, 19\n\t"              \
    "imul rdx, r11, 19\n\t"              \
    "xor eax, eax\n\t"                   \
    FE_SQ_R8                             \
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
    FE_SQ_R8                             \
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
    "mov rcx, 0x7FFFFFFFFFFFF\n\t"       \
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
    FE_SQ_R8                             \
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
    "mov rcx, 0x7FFFFFFFFFFFF\n\t"       \
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
    /* Five independent products, then one carry pass.
     *
     * MEASURED OUTCOME: this rewrite bought essentially nothing -- 79.96 ->
     * 79.06 cyc/op on the profiler arm, ladder_step 981.2 -> 980.2. It is
     * kept because it is verified equivalent and marginally ahead, but do
     * NOT redo this experiment expecting a win, and do not cite the 80-cycle
     * figure as a cost the ladder pays.
     *
     * Two reasons it did not pay, both worth recording:
     *
     * 1. The 80 cyc/op arm overstates the in-situ cost. TIME_NATIVE runs
     *    fe_mul121665_native(E, E) IN PLACE, so each iteration's loads read
     *    the previous iteration's stores at the same addresses -- the ~30
     *    cycle store-to-load stall documented in probe_ldorder. The ladder
     *    calls it out-of-place (t0 <- E) with E computed several ops earlier,
     *    so it never pays that. Budgeting ladder_step against its parts puts
     *    the real in-situ cost at <= 53 cycles, not 80.
     *
     * 2. The serial carry chain was not the binding constraint anyway. x86-64
     *    has no three-operand widening multiply without BMI2, which Goldmont
     *    lacks, so all five products still funnel through the one RDX:RAX
     *    pair (see the disassembly: mov rax,r12 / mul [mem], five times).
     *    Making the C independent cannot make the machine code independent.
     *    This is the same lesson as the GENARITHFLAGS carry bridge in fe_mul,
     *    from the opposite direction: there, per-register flags made five
     *    chains genuinely parallel; here, one architectural register pair
     *    keeps five chains serial no matter how the source is written.
     *
     * Bounds: the ladder feeds E = AA - BB carrying FE_SUB's 2p bias, so
     * a_i < 2^53 and p_i = a_i*121665 < 2^70 -- the 128-bit product is still
     * required. q_i = p_i >> 51 < 2^19, so 19*q_4 < 2^24 and every output
     * limb lands below 2^51 + 2^20, the same bound the serial form produced
     * and far inside fe_mul's 2^54 input limit.
     *
     * Congruence: sum_i p_i 2^(51i) = sum_i r_i 2^(51i) + sum_i q_i 2^(51(i+1)),
     * and the q_4 term is q_4 2^255 = 19 q_4 (mod p). */
    __uint128_t p0 = (__uint128_t)a[0] * 121665;
    __uint128_t p1 = (__uint128_t)a[1] * 121665;
    __uint128_t p2 = (__uint128_t)a[2] * 121665;
    __uint128_t p3 = (__uint128_t)a[3] * 121665;
    __uint128_t p4 = (__uint128_t)a[4] * 121665;

    uint64_t r0 = (uint64_t)p0 & MASK51, q0 = (uint64_t)(p0 >> 51);
    uint64_t r1 = (uint64_t)p1 & MASK51, q1 = (uint64_t)(p1 >> 51);
    uint64_t r2 = (uint64_t)p2 & MASK51, q2 = (uint64_t)(p2 >> 51);
    uint64_t r3 = (uint64_t)p3 & MASK51, q3 = (uint64_t)(p3 >> 51);
    uint64_t r4 = (uint64_t)p4 & MASK51, q4 = (uint64_t)(p4 >> 51);

    uint64_t t0    = r0 + 19 * q4;
    uint64_t carry = t0 >> 51;
    out[0] = t0 & MASK51;
    out[1] = r1 + q0 + carry;
    out[2] = r2 + q1;
    out[3] = r3 + q2;
    out[4] = r4 + q3;
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

/* INLINE2_CONTENDERS_ONLY keeps this whole block (the contender backends,
 * the RFC 7748 verification and the bench harness) but strips main(), so a
 * probe binary can reuse a contender directly. INLINE2_LIB, by contrast,
 * strips the block entirely — bench_attrib wants only the field-op macros. */
#if (!defined(INLINE2_PROFILE) && !defined(INLINE2_LIB)) || defined(INLINE2_CONTENDERS_ONLY)
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

        /* 2^51-1: the patch masks limbs with a single AND against rcx. */
        "mov rcx, 0x7FFFFFFFFFFFF\n\t"

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

        /* Load ascending, limb 0 -> limb 4. a[0] goes via rcx because rdi
         * still holds the input pointer; the extra reg-move is issue-bound
         * and overlaps the firing. Loading limb 4 first instead (the old
         * order) puts the first load one instruction after the previous
         * call's last store to the same address, which costs ~30 cyc on
         * Goldmont -- see the FE_SQ macro comment and probe_ldorder. */
        "mov rcx, [rdi]\n\t"
        "mov rsi, [rdi + 8]\n\t"
        "mov r12, [rdi + 16]\n\t"
        "mov r11, [rdi + 24]\n\t"
        "mov r14, [rdi + 32]\n\t"
        "mov rdi, rcx\n\t"

        /* Precompute doubled (2*a_i) and reduced (19*a_i) operands */
        "lea r15, [rdi + rdi]\n\t"
        "lea r13, [rsi + rsi]\n\t"
        "lea r9,  [r12 + r12]\n\t"
        "lea r10, [r11 + r11]\n\t"
        "imul rbx, r14, 19\n\t"
        "imul rdx, r11, 19\n\t"

        /* Clear accumulators */
        "xor eax, eax\n\t"
        FE_SQ_R8

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
        FE_SQ_R8

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
 * The two 5×51 ladder controls (CONTROLS.md).
 *
 * x25519_amd64_51_ucode above uses the register-chained inline-asm ladder
 * defined further down, so `amd64-51/asm vs amd64-51/ucode` moves both the
 * field ops and the ladder. These two arms complete the square inside
 * amd64-51's own framework, both on the SAME C ladderstep.c:
 *
 *   amd64-51/asm-Clad    C ladder + amd64-51's qhasm mul/square  (native)
 *   amd64-51/ucode-Clad  C ladder + 5×51 microcode field ops
 *
 *   asm-Clad  ÷ asm        = ladder tax, framework held constant
 *   asm-Clad  ÷ ucode-Clad = field-op effect, ladder AND framework constant
 *   ucode-Clad ÷ ucode     = ladder coding style, field ops held constant
 * ════════════════════════════════════════════════════════════════════ */
extern int x25519_amd64_51_asmclad(unsigned char *out,
                                   const unsigned char *scalar,
                                   const unsigned char *point);
extern int x25519_amd64_51_ucode_clad(unsigned char *out,
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
    /* Five independent products, then one carry pass.
     *
     * MEASURED OUTCOME: this rewrite bought essentially nothing -- 79.96 ->
     * 79.06 cyc/op on the profiler arm, ladder_step 981.2 -> 980.2. It is
     * kept because it is verified equivalent and marginally ahead, but do
     * NOT redo this experiment expecting a win, and do not cite the 80-cycle
     * figure as a cost the ladder pays.
     *
     * Two reasons it did not pay, both worth recording:
     *
     * 1. The 80 cyc/op arm overstates the in-situ cost. TIME_NATIVE runs
     *    fe_mul121665_native(E, E) IN PLACE, so each iteration's loads read
     *    the previous iteration's stores at the same addresses -- the ~30
     *    cycle store-to-load stall documented in probe_ldorder. The ladder
     *    calls it out-of-place (t0 <- E) with E computed several ops earlier,
     *    so it never pays that. Budgeting ladder_step against its parts puts
     *    the real in-situ cost at <= 53 cycles, not 80.
     *
     * 2. The serial carry chain was not the binding constraint anyway. x86-64
     *    has no three-operand widening multiply without BMI2, which Goldmont
     *    lacks, so all five products still funnel through the one RDX:RAX
     *    pair (see the disassembly: mov rax,r12 / mul [mem], five times).
     *    Making the C independent cannot make the machine code independent.
     *    This is the same lesson as the GENARITHFLAGS carry bridge in fe_mul,
     *    from the opposite direction: there, per-register flags made five
     *    chains genuinely parallel; here, one architectural register pair
     *    keeps five chains serial no matter how the source is written.
     *
     * Bounds: the ladder feeds E = AA - BB carrying FE_SUB's 2p bias, so
     * a_i < 2^53 and p_i = a_i*121665 < 2^70 -- the 128-bit product is still
     * required. q_i = p_i >> 51 < 2^19, so 19*q_4 < 2^24 and every output
     * limb lands below 2^51 + 2^20, the same bound the serial form produced
     * and far inside fe_mul's 2^54 input limit.
     *
     * Congruence: sum_i p_i 2^(51i) = sum_i r_i 2^(51i) + sum_i q_i 2^(51(i+1)),
     * and the q_4 term is q_4 2^255 = 19 q_4 (mod p). */
    __uint128_t p0 = (__uint128_t)a[0] * 121665;
    __uint128_t p1 = (__uint128_t)a[1] * 121665;
    __uint128_t p2 = (__uint128_t)a[2] * 121665;
    __uint128_t p3 = (__uint128_t)a[3] * 121665;
    __uint128_t p4 = (__uint128_t)a[4] * 121665;

    uint64_t r0 = (uint64_t)p0 & MASK51, q0 = (uint64_t)(p0 >> 51);
    uint64_t r1 = (uint64_t)p1 & MASK51, q1 = (uint64_t)(p1 >> 51);
    uint64_t r2 = (uint64_t)p2 & MASK51, q2 = (uint64_t)(p2 >> 51);
    uint64_t r3 = (uint64_t)p3 & MASK51, q3 = (uint64_t)(p3 >> 51);
    uint64_t r4 = (uint64_t)p4 & MASK51, q4 = (uint64_t)(p4 >> 51);

    uint64_t t0    = r0 + 19 * q4;
    uint64_t carry = t0 >> 51;
    out[0] = t0 & MASK51;
    out[1] = r1 + q0 + carry;
    out[2] = r2 + q1;
    out[3] = r3 + q2;
    out[4] = r4 + q3;
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
 * amd64-51 ASM FIELD OPS ON THE SHARED C LADDER  ("a51ops/C-ladder")
 *
 * The CONTROL for the 5×51 field-op claim.
 *
 * The headline `amd64-51/ucode` contender does NOT use amd64-51's own
 * ladderstep.S — it uses the register-chained inline-asm ladder defined
 * below (supercop_amd64_51_ucode_ladderstep). So the amd64-51/asm vs
 * amd64-51/ucode ratio moves TWO things at once: the field-op backend and
 * the ladder. It cannot attribute anything to the microcode alone.
 *
 * This contender closes that hole. It is `x25519_cryptopt` with exactly one
 * substitution — Bernstein–Schwabe's hand-written amd64-51 fe25519_mul.S /
 * fe25519_square.S in place of the CryptOpt field ops — so it shares the
 * IDENTICAL C ladder, inversion chain, cswap, pack and driver with
 * `ucode/C-ladder`, and runs in the same process. The pair
 *
 *     a51ops/C-ladder  vs  ucode/C-ladder
 *
 * therefore differs in the field-op backend and nothing else: it is the
 * clean form of the "microcode beats hand-tuned asm field ops" claim.
 *
 * Representation note: amd64-51's fe25519 is 5×51 unsaturated radix 2^51,
 * the same layout as our `fe` (uint64_t[5]), so the cast is a no-op. The
 * RFC 7748 vectors below gate this — if the bound disciplines disagreed,
 * the 1000-iteration chain would diverge.
 * ════════════════════════════════════════════════════════════════════ */
typedef struct { unsigned long long v[5]; } a51_fe_t;
extern void supercop_amd64_51_fe25519_mul(a51_fe_t *r, const a51_fe_t *x, const a51_fe_t *y);
extern void supercop_amd64_51_fe25519_square(a51_fe_t *r, const a51_fe_t *x);

static inline void fe_mul_a51(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    supercop_amd64_51_fe25519_mul((a51_fe_t *)out, (const a51_fe_t *)a, (const a51_fe_t *)b);
}

static inline void fe_sq_a51(const uint64_t *a, uint64_t *out) {
    supercop_amd64_51_fe25519_square((a51_fe_t *)out, (const a51_fe_t *)a);
}

static void fe_invert_a51(fe out, const fe z) {
    fe z2, z9, z11, t, t0, t1, t2, t3;
    int i;

    fe_sq_a51(z, z2);
    fe_sq_a51(z2, t);
    fe_sq_a51(t, t);
    fe_mul_a51(t, z, z9);
    fe_mul_a51(z9, z2, z11);
    fe_sq_a51(z11, t);
    fe_mul_a51(t, z9, t0);

    fe_sq_a51(t0, t1);
    for (i = 1; i < 5; i++) fe_sq_a51(t1, t1);
    fe_mul_a51(t1, t0, t1);

    fe_sq_a51(t1, t2);
    for (i = 1; i < 10; i++) fe_sq_a51(t2, t2);
    fe_mul_a51(t2, t1, t2);

    fe_sq_a51(t2, t3);
    for (i = 1; i < 20; i++) fe_sq_a51(t3, t3);
    fe_mul_a51(t3, t2, t3);

    for (i = 0; i < 10; i++) fe_sq_a51(t3, t3);
    fe_mul_a51(t3, t1, t1);

    fe_sq_a51(t1, t2);
    for (i = 1; i < 50; i++) fe_sq_a51(t2, t2);
    fe_mul_a51(t2, t1, t2);

    fe_sq_a51(t2, t3);
    for (i = 1; i < 100; i++) fe_sq_a51(t3, t3);
    fe_mul_a51(t3, t2, t3);

    for (i = 0; i < 50; i++) fe_sq_a51(t3, t3);
    fe_mul_a51(t3, t1, t1);

    fe_sq_a51(t1, t1);
    fe_sq_a51(t1, t1);
    fe_sq_a51(t1, t1);
    fe_sq_a51(t1, t1);
    fe_sq_a51(t1, t1);
    fe_mul_a51(t1, z11, out);
}

static void x25519_a51ops(uint8_t out[32], const uint8_t scalar[32],
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
        fe_sq_a51(A, AA);
        fe_sub(B, x2, z2);
        fe_sq_a51(B, BB);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul_a51(D, A, DA);
        fe_mul_a51(C, B, CB);

        fe_add(t0, DA, CB);
        fe_sq_a51(t0, x3);

        fe_sub(t0, DA, CB);
        fe_sq_a51(t0, z3);
        fe_mul_a51(x1, z3, z3);

        fe_mul_a51(AA, BB, x2);

        fe_mul121665(t0, E);
        fe_add(t0, AA, t0);
        fe_mul_a51(E, t0, z2);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert_a51(z2, z2);
    fe_mul_a51(x2, z2, x2);
    fe_tobytes(out, x2);
}

/* ════════════════════════════════════════════════════════════════════
 * OpenSSL fe51 ASM FIELD OPS ON THE SHARED C LADDER  ("osslops/C-ladder")
 *
 * OpenSSL's hand-written 5×51 assembly (crypto/ec/asm/x25519-x86_64.pl,
 * x25519_fe51_mul / x25519_fe51_sqr) substituted into the SAME C ladder,
 * inversion chain, cswap, pack and driver used by ucode/C-ladder and
 * a51ops/C-ladder. Only the field backend differs, so the three are directly
 * comparable and run in the same process.
 *
 * This is the control that matters: at the kernel level these routines cost
 * 97.1 and 73.2 cycles against the microcode's 122.9 and 81.8, so this row
 * decides whether that per-operation deficit survives a full scalar
 * multiplication.
 *
 * OpenSSL selects this fe51 path on any x86_64 without ADX, which includes
 * this core, so it is also what OpenSSL itself would execute here.
 * ════════════════════════════════════════════════════════════════════ */
void x25519_fe51_mul(uint64_t h[5], const uint64_t f[5], const uint64_t g[5]);
void x25519_fe51_sqr(uint64_t h[5], const uint64_t f[5]);

static inline void fe_mul_ossl(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    x25519_fe51_mul(out, a, b);
}

static inline void fe_sq_ossl(const uint64_t *a, uint64_t *out) {
    x25519_fe51_sqr(out, a);
}

static void fe_invert_ossl(fe out, const fe z) {
    fe z2, z9, z11, t, t0, t1, t2, t3;
    int i;

    fe_sq_ossl(z, z2);
    fe_sq_ossl(z2, t);
    fe_sq_ossl(t, t);
    fe_mul_ossl(t, z, z9);
    fe_mul_ossl(z9, z2, z11);
    fe_sq_ossl(z11, t);
    fe_mul_ossl(t, z9, t0);

    fe_sq_ossl(t0, t1);
    for (i = 1; i < 5; i++) fe_sq_ossl(t1, t1);
    fe_mul_ossl(t1, t0, t1);

    fe_sq_ossl(t1, t2);
    for (i = 1; i < 10; i++) fe_sq_ossl(t2, t2);
    fe_mul_ossl(t2, t1, t2);

    fe_sq_ossl(t2, t3);
    for (i = 1; i < 20; i++) fe_sq_ossl(t3, t3);
    fe_mul_ossl(t3, t2, t3);

    for (i = 0; i < 10; i++) fe_sq_ossl(t3, t3);
    fe_mul_ossl(t3, t1, t1);

    fe_sq_ossl(t1, t2);
    for (i = 1; i < 50; i++) fe_sq_ossl(t2, t2);
    fe_mul_ossl(t2, t1, t2);

    fe_sq_ossl(t2, t3);
    for (i = 1; i < 100; i++) fe_sq_ossl(t3, t3);
    fe_mul_ossl(t3, t2, t3);

    for (i = 0; i < 50; i++) fe_sq_ossl(t3, t3);
    fe_mul_ossl(t3, t1, t1);

    fe_sq_ossl(t1, t1);
    fe_sq_ossl(t1, t1);
    fe_sq_ossl(t1, t1);
    fe_sq_ossl(t1, t1);
    fe_sq_ossl(t1, t1);
    fe_mul_ossl(t1, z11, out);
}

static void x25519_osslops(uint8_t out[32], const uint8_t scalar[32],
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
        fe_sq_ossl(A, AA);
        fe_sub(B, x2, z2);
        fe_sq_ossl(B, BB);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul_ossl(D, A, DA);
        fe_mul_ossl(C, B, CB);

        fe_add(t0, DA, CB);
        fe_sq_ossl(t0, x3);

        fe_sub(t0, DA, CB);
        fe_sq_ossl(t0, z3);
        fe_mul_ossl(x1, z3, z3);

        fe_mul_ossl(AA, BB, x2);

        fe_mul121665(t0, E);
        fe_add(t0, AA, t0);
        fe_mul_ossl(E, t0, z2);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert_ossl(z2, z2);
    fe_mul_ossl(x2, z2, x2);
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

        /* a51ops/C-ladder: amd64-51 asm field ops on the shared C ladder.
         * This 1000-iteration chain is the gate on the representation/bound
         * compatibility noted at the fe_mul_a51 definition. */
        uint8_t kq[32], uq[32];
        memcpy(kq, k, 32);
        memcpy(uq, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_a51ops(r, kq, uq);
            memcpy(uq, kq, 32);
            memcpy(kq, r, 32);
        }
        print_hex("a51ops after 1000", kq, 32);
        if (memcmp_hex(kq,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  a51ops: PASS\n"); pass++;
        } else {
            printf("  a51ops: FAIL\n"); fail++;
        }

        /* The two 5×51 ladder controls. */
        uint8_t kca[32], uca[32];
        memcpy(kca, k, 32); memcpy(uca, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_amd64_51_asmclad(r, kca, uca);
            memcpy(uca, kca, 32);
            memcpy(kca, r, 32);
        }
        print_hex("a51/asmCld  1000 ", kca, 32);
        if (memcmp_hex(kca,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  a51/asm-Clad: PASS\n"); pass++;
        } else {
            printf("  a51/asm-Clad: FAIL\n"); fail++;
        }

        uint8_t kcu[32], ucu[32];
        memcpy(kcu, k, 32); memcpy(ucu, u, 32);
        for (int i = 0; i < 1000; i++) {
            x25519_amd64_51_ucode_clad(r, kcu, ucu);
            memcpy(ucu, kcu, 32);
            memcpy(kcu, r, 32);
        }
        print_hex("a51/ucodeCld 1000", kcu, 32);
        if (memcmp_hex(kcu,
                       "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51",
                       32) == 0) {
            printf("  a51/ucode-Clad: PASS\n"); pass++;
        } else {
            printf("  a51/ucode-Clad: FAIL\n"); fail++;
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
 *
 * Contenders are timed INTERLEAVED: one repetition of every contender per
 * round, round-robin, rather than BENCH_REPS reps of one contender before
 * moving on to the next. Under block-sequential timing any slow drift over
 * the run — thermal, frequency, or accumulated machine state — lands unevenly
 * across the contenders and is indistinguishable from a real difference
 * between them. Round-robin spreads such drift equally over all of them, so
 * the ranking cannot be an artefact of measurement order. The amd64-64
 * control binary (full_curve25519_amd64_64_asmclad.c) has always timed its
 * two arms this way; this brings the main harness into line with it.
 *
 * Output format is unchanged — lib/build_run.sh greps "^<label>: … median N".
 * ════════════════════════════════════════════════════════════════════ */
typedef void (*bench_fn)(uint8_t *out, const uint8_t *scalar, const uint8_t *point);

/* Uniform thunks: our own contenders return void over uint8_t[32], the
 * SUPERCOP-derived ones return int over unsigned char*. Same underlying type,
 * so these only normalise the signature for the dispatch table below. */
#define BENCH_THUNK(TH, CALL)                                                \
    static void TH(uint8_t *o, const uint8_t *s, const uint8_t *p) { CALL; }

/* s2n-bignum, AWS formally verified assembly. curve25519_x25519_alt has a
 * second documented prototype taking byte arrays, backed by the same code
 * because x86 is little endian, and it performs the RFC 7748 scalar clamping
 * and point masking itself. We use the _alt variant because the base one is
 * built from MULX/ADCX/ADOX, which this core does not implement. Operands are
 * copied into 64 bit aligned buffers first, since the caller's are byte
 * arrays with no alignment guarantee. */
extern void curve25519_x25519_alt(uint64_t res[4], const uint64_t scalar[4],
                                  const uint64_t point[4]);
static void x25519_s2n(uint8_t out[32], const uint8_t scalar[32],
                       const uint8_t point[32]) {
    uint64_t r[4], n[4], q[4];
    memcpy(n, scalar, 32);
    memcpy(q, point, 32);
    curve25519_x25519_alt(r, n, q);
    memcpy(out, r, 32);
}

BENCH_THUNK(bt_s2n,      x25519_s2n(o, s, p))
/* OpenSSL's OWN implementation: its ladder, its inversion, its assembly field
 * kernels. Extracted in openssl_x25519.c; end-to-end contender only, since it
 * shares nothing with our ladder. */
extern void x25519_openssl(uint8_t out[32], const uint8_t scalar[32],
                           const uint8_t point[32]);
BENCH_THUNK(bt_openssl,  x25519_openssl(o, s, p))
BENCH_THUNK(bt_osslops,  x25519_osslops(o, s, p))
BENCH_THUNK(bt_hand_c,   x25519_native(o, s, p))
BENCH_THUNK(bt_fiat,     x25519_fiat(o, s, p))
BENCH_THUNK(bt_cryptopt, x25519_cryptopt(o, s, p))
BENCH_THUNK(bt_a51ops,   x25519_a51ops(o, s, p))
BENCH_THUNK(bt_donna,    (void)x25519_donna_c64(o, s, p))
BENCH_THUNK(bt_a51_asm,  (void)x25519_amd64_51(o, s, p))
BENCH_THUNK(bt_a51_uc,   (void)x25519_amd64_51_ucode(o, s, p))
BENCH_THUNK(bt_a51_asmc, (void)x25519_amd64_51_asmclad(o, s, p))
BENCH_THUNK(bt_a51_ucc,  (void)x25519_amd64_51_ucode_clad(o, s, p))
BENCH_THUNK(bt_a64_asm,  (void)x25519_amd64_64(o, s, p))
BENCH_THUNK(bt_ours_uc,  x25519(o, s, p))
BENCH_THUNK(bt_uc_clad,  x25519_ucode(o, s, p))

/* Order here is only the order rows are printed; it no longer affects timing. */
static const struct { const char *label; bench_fn fn; } BENCH_TAB[] = {
    { "ours/hand-C:",          bt_hand_c   },
    { "ours/fiat:",            bt_fiat     },
    { "ours/cryptopt:",        bt_cryptopt },
    /* "a51ops/C-ladder" = Bernstein-Schwabe amd64-51 asm field ops on the
     * SAME C ladder as ucode/C-ladder — the control that separates the
     * field-op backend from the ladder rewrite (see CONTROLS.md). */
    { "a51ops/C-ladder:",      bt_a51ops   },
    { "donna_c64:",            bt_donna    },
    { "amd64-51/asm:",         bt_a51_asm  },
    { "amd64-51/ucode:",       bt_a51_uc   },
    /* The two 5×51 ladder controls — same framework, same C ladder, field
     * ops the only difference between them (CONTROLS.md). */
    { "amd64-51/asm-Clad:",    bt_a51_asmc },
    { "amd64-51/ucode-Clad:",  bt_a51_ucc  },
    { "amd64-64/asm:",         bt_a64_asm  },
    /* "ours/ucode" is the inline-asm register-chained ladder (canonical).
     * "ucode/C-ladder" is the SAME microcode field ops on the identical C
     * ladder as hand-C/fiat/cryptopt — it isolates the field-op backend
     * end-to-end, and is NOT a headline contender. */
    { "ours/ucode:",           bt_ours_uc  },
    { "ucode/C-ladder:",       bt_uc_clad  },
    { "s2n-bignum/asm:",       bt_s2n      },
    { "osslops/C-ladder:",     bt_osslops  },
    { "openssl:",              bt_openssl  },
};
#define N_BENCH ((int)(sizeof BENCH_TAB / sizeof BENCH_TAB[0]))

/* [contender][rep] — ~96 KB in BSS at 12 contenders x 1000 reps. */
static uint64_t bench_samples[N_BENCH][BENCH_REPS];

/* ── contender filter, for the interleaving-sensitivity experiment ──────
 * BENCH_ONLY=<substr>[,<substr>...] restricts the round-robin to the named
 * contenders. Same binary, same compiler, same timing loop, same statistic --
 * the ONLY thing that changes is how many other contenders run between two
 * consecutive repetitions of a given one.
 *
 * Why this exists: ours/ucode measures ~280k in an isolated loop but ~300k in
 * the 15-way round-robin, while osslops moves by 73 cycles between its own min
 * and median. A 7% sensitivity that hits one contender and not another is
 * either a real property of the implementation (microcode state, or an I-cache
 * footprint that the other contenders evict) or an artefact of the harness. It
 * cannot be diagnosed by comparing two different binaries built by two
 * different compilers, which is how it was first spotted.
 *
 * Empty / unset keeps every contender, so the default path is unchanged. */
static int bench_selected(const char *label) {
    const char *only = getenv("BENCH_ONLY");
    if (!only || !*only) return 1;
    size_t n = strlen(only), i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && only[j] != ',') j++;
        if (j > i && strncmp(label, only + i, j - i) == 0) return 1;
        i = j + 1;
    }
    return 0;
}

static void benchmark(void) {
    uint8_t scalar[32] = {0}, point[32] = {0}, out[32];
    uint64_t mn, med, p10, p90;
    int sel[N_BENCH], nsel = 0;
    for (int c = 0; c < N_BENCH; c++)
        if (bench_selected(BENCH_TAB[c].label)) sel[nsel++] = c;
    if (nsel == 0) { printf("BENCH_ONLY matched no contender; nothing to do.\n"); return; }

    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);

    printf("=== X25519 Benchmark (%d repetitions, interleaved) ===\n\n", BENCH_REPS);

    /* Warm up every contender before any timing starts, and gate on RFC 7748
     * vector 1 so that no contender is reported without having been checked
     * against the specification in this very process. */
    {
        int bad = 0;
        for (int k = 0; k < nsel; k++) {
            int c = sel[k];
            BENCH_TAB[c].fn(out, scalar, point);
            if (memcmp_hex(out,
                    "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552",
                    32) != 0) {
                printf("  %-20s RFC 7748 vector 1 MISMATCH\n", BENCH_TAB[c].label);
                bad++;
            }
        }
        printf("  contender verification: %s (%d of %d failed)\n",
               bad ? "FAIL" : "all pass", bad, nsel);
        printf("  contenders in the round-robin: %d of %d%s\n\n", nsel, N_BENCH,
               nsel == N_BENCH ? "" : "  [BENCH_ONLY active]");
    }

    /* Round-robin over contenders; the repetition index is the outer loop. */
    for (int r = 0; r < BENCH_REPS; r++) {
        for (int k = 0; k < nsel; k++) {
            int c = sel[k];
            uint64_t t0 = rdtsc_start();
            BENCH_TAB[c].fn(out, scalar, point);
            uint64_t t1 = rdtsc_end();
            bench_samples[c][r] = t1 - t0;
        }
    }

    for (int k = 0; k < nsel; k++) {
        int c = sel[k];
        bench_stats(bench_samples[c], BENCH_REPS, &mn, &med, &p10, &p90);
        printf("%-20s median %8" PRIu64 "  min %8" PRIu64 "  p10 %8" PRIu64
               "  p90 %8" PRIu64 " cycles\n", BENCH_TAB[c].label, med, mn, p10, p90);
    }

    printf("\n");
}

/* ════════════════════════════════════════════════════════════════════
 * MAIN — the inline-asm 5×51 ladder (x25519) is the canonical "ours".
 * ════════════════════════════════════════════════════════════════════ */
#if !defined(INLINE2_CONTENDERS_ONLY)
int main(void) {
    printf("=== Full X25519: every contender (inline-asm 5×51 is canonical ours) ===\n\n");

    if (freq_guard()) return 2;

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
#endif /* !INLINE2_CONTENDERS_ONLY */
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
    if (freq_guard()) return 2;
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
