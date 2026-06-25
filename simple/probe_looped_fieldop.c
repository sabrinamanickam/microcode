/* probe_looped_fieldop.c — THE decisive experiment for "can microcode beat
 * amd64-64 on X25519".
 *
 * Background (why_speedup.html / project_microcode_firing_latency):
 *   A standalone fe_sq firing costs ~123 cyc, of which ~80 is a FIXED per-firing
 *   "tax" (vmwrite trigger -> sequencer spin-up -> drain -> writeback) and the
 *   rest is the 5x51 arithmetic. X25519 issues ~2561 such firings, so the tax
 *   IS the end-to-end number, and that tax is why we lose to amd64-64 (272k).
 *
 *   The Keccak harness proved you can LOOP a body inside ONE vmwrite, paying the
 *   tax once. Keccak LOST (it is xor/and-bound; ~1.34 cyc/triad latency loses to
 *   native ILP). But X25519 field ops are MUL-bound: each triad carries a full
 *   64x64 multiply Goldmont cannot issue faster than ~1/cyc. So a LOOPED field op
 *   might run at its true critical-path cost (well under 123) -- where Keccak
 *   couldn't win, this could.
 *
 * What this measures:
 *   Loop the production 43-triad fe_sq body N times inside ONE firing, as a true
 *   dependency chain (x -> x^2 -> x^4 -> ...). A 4-triad "re-prep" rebuilds the
 *   sq input registers (2*xi, 19*xi, clears) from the previous square's outputs,
 *   so iteration N+1 genuinely waits on iteration N (NOT throughput -- that is
 *   the methodology trap that made looped Keccak look fast at first).
 *
 *   Sweep N. cyc(N) = FIRING_TAX + N * per_op.   The SLOPE between N values is
 *   the looped per-op latency with the tax removed -- the number that decides it:
 *       slope ~= 56  -> tax fully amortizes; looped sq beats native sq (86 cyc)
 *                       -> field-op interpreter loop is worth building, win is on
 *       slope ~= 123 -> no amortization; the firing tax is intrinsic per-op
 *                       -> structural ceiling confirmed, stop.
 *   (Native amd64-64: fe_mul=100, fe_square=86 cyc. fe_sq=43 tri, fe_mul=66 tri,
 *    so a mul slope ~= sq_slope * 66/43.)
 *
 * Build: make PROG=probe_looped_fieldop \
 *          CFLAGS="-O3 -march=native -masm=intel -fwrapv -fPIE -I include/"
 * Run:   sudo taskset -c 0 ./probe_looped_fieldop_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL
#define REGION 0x7c00
#define T(n)   (REGION + (n)*4)

/* Production 5x51 squaring body (43 triads), verbatim from
 * full_curve25519_inline2.c / probe_sq_latency.c. The LAST triad here is emitted
 * separately with NOP_SEQWORD (so the loop continues) instead of END_SEQWORD. */
static const ucode_t sq_body[] = {
    /* c0 */
    { ZEROEXT_DSZ64_DR(TMP0, RAX), MUL_DSZ64_DRR(RCX, RDI, RDI), NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDI), SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP9, TMP15), MUL_DSZ64_DRR(RCX, RBX, R13), ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RDX, R9), ADD_DSZ64_DRR(TMP0, TMP0, R9), SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHL_DSZ64_DRI(TMP6, TMP0, 13), SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(RDI, TMP6, 13), ZEROEXT_DSZ64_DR(R13, RSI), NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    /* c1 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, R13), ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(R9, R12, R12), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP9, TMP15), MUL_DSZ64_DRR(RCX, R11, RDX), ADD_DSZ64_DRR(TMP0, TMP0, RDX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RBX, R9), ADD_DSZ64_DRR(TMP0, TMP0, R9), SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHL_DSZ64_DRI(TMP6, TMP0, 13), SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(R9, TMP6, 13), ZEROEXT_DSZ64_DR(RDX, R12), NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    /* c2 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, RDX), ADD_DSZ64_DRR(TMP0, TMP0, RDX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), ZEROEXT_DSZ64_DR(R13, RSI), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP9, TMP15), MUL_DSZ64_DRR(RCX, RSI, R13), ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RBX, R10), ADD_DSZ64_DRR(TMP0, TMP0, R10), SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHL_DSZ64_DRI(TMP6, TMP0, 13), SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(R10, TMP6, 13), ZEROEXT_DSZ64_DR(RDX, R11), NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    /* c3 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, RDX), ADD_DSZ64_DRR(R13, RSI, RSI), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RAX, R12), MUL_DSZ64_DRR(RCX, R13, RAX), ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RAX), SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, R14, RBX), ADD_DSZ64_DRR(TMP0, TMP0, RBX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(TMP8, TMP0, 51), SHL_DSZ64_DRI(TMP6, TMP0, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(RBX, TMP6, 13), SHL_DSZ64_DRI(TMP1, R8, 13), NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    /* c4 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, R14), ADD_DSZ64_DRR(TMP0, TMP0, R14), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, R13, R11), ADD_DSZ64_DRR(TMP0, TMP0, R11), SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), MUL_DSZ64_DRR(RCX, R12, R12), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, R12), SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHR_DSZ64_DRI(TMP8, TMP0, 51), ADD_DSZ64_DRR(R8, R8, TMP9), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP6, TMP0, 13), SHL_DSZ64_DRI(TMP1, R8, 13), SHR_DSZ64_DRI(RAX, TMP6, 13), NOP_SEQWORD },
    /* final reduction (2 of 3 triads; the 3rd is emitted by build_loop) */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DIR(TMP6, 19, TMP0), ADD_DSZ64_DRR(RDI, RDI, TMP0), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, RDI, 51), ADD_DSZ64_DRR(R9, R9, TMP0), NOP, NOP_SEQWORD },
};
#define SQ_BODY_N ((int)(sizeof(sq_body)/sizeof(sq_body[0])))   /* 42 */

/* Production 5x51 multiply body (66 triads), verbatim from
 * full_curve25519_inline2.c install_field_patches() mul_patch. Inputs:
 *   a = rdi,rsi,r12,r11,r14 ; b = r15,r13,r9,r10,rbx ; rax=0,r8=0.
 * Prologue saves b -> TMP10..TMP14 and derives 19*b. Output: r15=h0,r13=h1,
 * r9=h2,r10=h3,rax=h4. The patch USES TMP10..TMP14 + TMP0,1,2,8,9,15, so the
 * mul loop's counter/scratch must avoid those (use TMP7/TMP5). The last triad
 * here ends the patch; build_mul_loop re-emits it with NOP_SEQWORD. */
static const ucode_t mul_body[] = {
    { ZEROEXT_DSZ64_DR(TMP10, R15), ZEROEXT_DSZ64_DR(TMP11, R13), ZEROEXT_DSZ64_DR(TMP12, R9), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R10), ZEROEXT_DSZ64_DR(TMP14, RBX), MUL_DSZ64_DIR(RCX, 19, R13), NOP_SEQWORD },
    { MUL_DSZ64_DIR(RCX, 19, R9), MUL_DSZ64_DIR(RCX, 19, R10), MUL_DSZ64_DIR(RCX, 19, RBX), NOP_SEQWORD },
    /* c0 — RESCHEDULED: 3-deep MUL pipeline. Products: M0=a0*b0, M1=a1*19b4,
     * M2=a2*19b3, M3=a3*19b2, M4=a4*19b1. lo/hi reg pairs:
     * M0(TMP3,TMP5) M1(TMP4,TMP6) M2(TMP7,RCX) M3(TMP3,TMP5) M4(TMP4,TMP6).
     * MUL->consume distance >=3 triads (vs ~2 in production) to hide ~6c latency.
     * Acc: TMP0=lo, R8=hi, TMP9=lo-carry count (folded R8+=TMP9 at end). 13 triads. */
    { NOTAND_DSZ64_DRR(TMP0, TMP0, TMP0), ZEROEXT_DSZ64_DR(TMP3, TMP10), ZEROEXT_DSZ64_DR(TMP4, RBX), NOP_SEQWORD },
    { MUL_DSZ64_DRR(TMP5, RDI, TMP3), ZEROEXT_DSZ64_DR(TMP7, R10), NOTAND_DSZ64_DRR(TMP9, TMP9, TMP9), NOP_SEQWORD },
    { MUL_DSZ64_DRR(TMP6, RSI, TMP4), MUL_DSZ64_DRR(RCX, R12, TMP7), NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(R8, TMP5), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP9, TMP9, TMP15), ZEROEXT_DSZ64_DR(TMP3, R9), MUL_DSZ64_DRR(TMP5, R11, TMP3), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP4), SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, TMP6), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP9, TMP9, TMP15), ZEROEXT_DSZ64_DR(TMP4, R13), MUL_DSZ64_DRR(TMP6, R14, TMP4), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, TMP7), SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP9, TMP9, TMP15), ADD_DSZ64_DRR(TMP0, TMP0, TMP3), SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP5), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), ADD_DSZ64_DRR(TMP0, TMP0, TMP4), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, TMP6), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(TMP8, TMP0, 51), SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP1, R8, 13), SHR_DSZ64_DRI(R15, TMP2, 13), NOP, NOP_SEQWORD },
    /* c1 */
    { ZEROEXT_DSZ64_DR(RDX, TMP11), MUL_DSZ64_DRR(RCX, RDI, RDX), OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX), ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, R9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R13, TMP2, 13), SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    /* c2 */
    { ZEROEXT_DSZ64_DR(RDX, TMP12), MUL_DSZ64_DRR(RCX, RDI, RDX), OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX), ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R9, TMP2, 13), SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    /* c3 */
    { ZEROEXT_DSZ64_DR(RDX, TMP13), MUL_DSZ64_DRR(RCX, RDI, RDX), OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP12), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX), ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R10, TMP2, 13), SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    /* c4 */
    { ZEROEXT_DSZ64_DR(RDX, TMP14), MUL_DSZ64_DRR(RCX, RDI, RDX), OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP13), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX), ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP12), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(RAX, TMP2, 13), SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    /* final reduction */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DIR(TMP2, 19, TMP0), ADD_DSZ64_DRR(R15, R15, TMP0), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, R15, 51), ADD_DSZ64_DRR(R13, R13, TMP0), NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP2, R15, 13), SHR_DSZ64_DRI(R15, TMP2, 13), NOP, END_SEQWORD },
};
#define MUL_BODY_N ((int)(sizeof(mul_body)/sizeof(mul_body[0])))   /* 66 */

static ucode_t lp[128];

/* Minimal bounded loop self-test (acc += 7, COUNT times => 7*COUNT).
 * Uses the EXACT loop idiom the sq loop uses (all confirmed-working primitives):
 *   - forward exit: same-triad XOR(sets ZF) + UJMPCC CONDZ -> exit when counter==N
 *   - backward:     unconditional SEQ_GOTO0(loop_top) in the seqword
 * (Backward UJMPCC crashes the core per ujmp_test.c; SEQ_GOTO0 is the proven
 *  backward jump, per probe_loop.c T2 -- the Keccak loop basis.)
 * Isolates loop control from the fe_sq body so one run says which half is broken. */
static uint64_t selftest_loop(int count) {
    ucode_t p[8];
    int n = 0;
    p[n++] = (ucode_t){ ZEROEXT_DSZ32_DI(TMP0, 0), NOP, NOP, NOP_SEQWORD };  /* counter */
    p[n++] = (ucode_t){ ZEROEXT_DSZ32_DI(TMP1, 0), NOP, NOP, NOP_SEQWORD };  /* acc */
    int loop_top = n;
    p[n++] = (ucode_t){ ADD_DSZ64_DRI(TMP0, TMP0, 1), ADD_DSZ64_DRI(TMP1, TMP1, 7), NOP, NOP_SEQWORD };
    int exit_idx = n + 2;   /* XOR/UJMPCC triad, then SEQ_GOTO0 triad, then exit */
    p[n++] = (ucode_t){ XOR_DSZ64_DRI(TMP2, TMP0, count), NOP,
                        UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(TMP2, T(exit_idx)), NOP_SEQWORD };
    p[n++] = (ucode_t){ NOP, NOP, NOP, SEQ_GOTO0(T(loop_top)) };
    p[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(RAX, TMP1), NOP, NOP, END_SEQWORD };
    init_match_and_patch(); do_fix_IN_patch();
    hook_match_and_patch(0, 0x0cd8, REGION);
    patch_ucode(REGION, p, n);
    uint64_t r;
    asm volatile("vmwrite rcx, rdx\n\t" : "=a"(r) ::
        "rbx","rcx","rdx","rsi","rdi","rbp","r8","r9","r10","r11",
        "r12","r13","r14","r15","memory","cc");
    return r;
}

/* Build a single-firing patch that squares the input N times in a dependency
 * chain. Returns triad count; *loop_top_out gets the loop-top triad index.
 * loop_top = the benign counter-increment triad (NOT the sq body's MUL triad):
 * re-entering a backward branch onto a MUL micro-op crashes the sequencer. */
static int build_loop(int N, int *loop_top_out) {
    int n = 0;
    /* counter = 0 (TMPs not guaranteed zero at entry) */
    lp[n++] = (ucode_t){ ZEROEXT_DSZ32_DI(TMP14, 0), NOP, NOP, NOP_SEQWORD };
    int loop_top = n;
    /* benign re-entry point: counter += 1 */
    lp[n++] = (ucode_t){ ADD_DSZ64_DRI(TMP14, TMP14, 1), NOP, NOP, NOP_SEQWORD };
    /* --- one squaring --- */
    for (int i = 0; i < SQ_BODY_N; i++) lp[n++] = sq_body[i];
    /* final reduction triad #3 (sq_patch's END triad, but NOP_SEQWORD so we loop):
     *   rdi = (rdi<<13)>>13  -> h0 in RDI; outputs now rdi=h0,r9=h1,r10=h2,rbx=h3,rax=h4 */
    lp[n++] = (ucode_t){ SHL_DSZ64_DRI(TMP6, RDI, 13), SHR_DSZ64_DRI(RDI, TMP6, 13), NOP, NOP_SEQWORD };
    /* --- re-prep: outputs (rdi=h0,r9=h1,r10=h2,rbx=h3,rax=h4) -> next-square
     * input form. Target: rdi=l0, rsi=l1, r12=l2, r11=l3, r14=l4,
     *   r15=2l0, r13=2l1, r9=2l2, r10=2l3, rbx=19l4, rdx=19l3, rax=0, r8=0.
     * CRITICAL: MUL_DSZ64_DIR(dst,imm,src) writes src=lo, dst=hi (two outputs).
     * So compute 19*li by copying the limb into the target reg, then multiplying
     * IN PLACE (src=target), with the dead hi going to RCX -- this preserves the
     * limb regs r14=l4 and r11=l3 that the body also needs. (lea/imul in FIRE_PREP
     * preserves the source; the microcode MUL does not, hence the copies.) */
    lp[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(RSI, R9),  ZEROEXT_DSZ64_DR(R12, R10), ZEROEXT_DSZ64_DR(R11, RBX), NOP_SEQWORD }; /* rsi=h1,r12=h2,r11=h3 */
    lp[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(R14, RAX), ADD_DSZ64_DRR(R15, RDI, RDI), ADD_DSZ64_DRR(R13, RSI, RSI), NOP_SEQWORD }; /* r14=h4, r15=2h0, r13=2h1 */
    lp[n++] = (ucode_t){ ADD_DSZ64_DRR(R9, R12, R12), ADD_DSZ64_DRR(R10, R11, R11), ZEROEXT_DSZ64_DR(RBX, R14), NOP_SEQWORD }; /* r9=2h2, r10=2h3, rbx=h4 */
    lp[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(RDX, R11), MUL_DSZ64_DIR(RCX, 19, RBX), NOP, NOP_SEQWORD }; /* rdx=h3, rbx=19h4 (rcx=hi dead) */
    lp[n++] = (ucode_t){ MUL_DSZ64_DIR(RCX, 19, RDX), ZEROEXT_DSZ32_DI(RAX, 0), ZEROEXT_DSZ32_DI(R8, 0), NOP_SEQWORD }; /* rdx=19h3, rax=0, r8=0 */
    /* --- loop control (counter counts 1..N): same-triad XOR(sets ZF)+UJMPCC
     * CONDZ exits forward when counter==N; else SEQ_GOTO0 jumps back to the
     * (benign) increment triad. Backward UJMPCC crashes the core; SEQ_GOTO0 is
     * the proven backward jump. --- */
    int exit_idx = n + 2;
    lp[n++] = (ucode_t){ XOR_DSZ64_DRI(TMP13, TMP14, N), NOP,
                         UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(TMP13, T(exit_idx)), NOP_SEQWORD };
    lp[n++] = (ucode_t){ NOP, NOP, NOP, SEQ_GOTO0(T(loop_top)) };
    /* --- exit (result in rdi,rsi,r12,r11,r14 after last re-prep) --- */
    lp[n++] = (ucode_t){ NOP, NOP, NOP, END_SEQWORD };
    if (loop_top_out) *loop_top_out = loop_top;
    return n;
}

/* Install the looped-sq patch for a given internal iteration count N. */
static int install_loop(int N) {
    int top;
    int n = build_loop(N, &top);
    init_match_and_patch();
    do_fix_IN_patch();
    hook_match_and_patch(0, 0x0cd8, REGION);   /* vmwrite hook */
    patch_ucode(REGION, lp, n);
    return n;
}

/* Build a single-firing patch that computes a = a*a, N times, in a dependency
 * chain via the production mul body (a^(2^N)). Counter=TMP7, scratch=TMP5
 * (mul body uses TMP0,1,2,8,9,10..15, so those are the only free ones).
 * re-prep: mul output (r15=h0,r13=h1,r9=h2,r10=h3,rax=h4) -> next a&b.
 * a = rdi,rsi,r12,r11,r14 ; b = r15,r13,r9,r10,rbx. The output already sits in
 * b-regs r15,r13,r9,r10 (=h0..h3), so b just needs rbx=h4; a copies from them. */
static int build_mul_loop(int N) {
    int n = 0;
    lp[n++] = (ucode_t){ ZEROEXT_DSZ32_DI(TMP7, 0), NOP, NOP, NOP_SEQWORD };  /* counter */
    int loop_top = n;
    lp[n++] = (ucode_t){ ADD_DSZ64_DRI(TMP7, TMP7, 1), NOP, NOP, NOP_SEQWORD }; /* benign re-entry */
    /* --- one multiply (body minus its END triad) --- */
    for (int i = 0; i < MUL_BODY_N - 1; i++) lp[n++] = mul_body[i];
    /* final triad re-emitted with NOP_SEQWORD: r15 = (r15<<13)>>13 -> h0 */
    lp[n++] = (ucode_t){ SHL_DSZ64_DRI(TMP2, R15, 13), SHR_DSZ64_DRI(R15, TMP2, 13), NOP, NOP_SEQWORD };
    /* --- re-prep for a=a*a: output b-regs (r15,r13,r9,r10)=h0..h3 stay as b;
     * copy them to a-regs; set rbx=h4 and r14=h4; rax=0,r8=0 --- */
    lp[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(RDI, R15), ZEROEXT_DSZ64_DR(RSI, R13), ZEROEXT_DSZ64_DR(R12, R9), NOP_SEQWORD };  /* a0=h0,a1=h1,a2=h2 */
    lp[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(R11, R10), ZEROEXT_DSZ64_DR(R14, RAX), ZEROEXT_DSZ64_DR(RBX, RAX), NOP_SEQWORD }; /* a3=h3, a4=h4, b4=h4 */
    lp[n++] = (ucode_t){ ZEROEXT_DSZ32_DI(RAX, 0), ZEROEXT_DSZ32_DI(R8, 0), NOP, NOP_SEQWORD };                            /* rax=0, r8=0 */
    /* --- loop control: forward CONDZ exit + SEQ_GOTO0 backward --- */
    int exit_idx = n + 2;
    lp[n++] = (ucode_t){ XOR_DSZ64_DRI(TMP5, TMP7, N), NOP,
                         UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(TMP5, T(exit_idx)), NOP_SEQWORD };
    lp[n++] = (ucode_t){ NOP, NOP, NOP, SEQ_GOTO0(T(loop_top)) };
    /* exit: result a in rdi,rsi,r12,r11,r14 after last re-prep */
    lp[n++] = (ucode_t){ NOP, NOP, NOP, END_SEQWORD };
    return n;
}

static int install_mul_loop(int N) {
    int n = build_mul_loop(N);
    init_match_and_patch();
    do_fix_IN_patch();
    hook_match_and_patch(0, 0x0cd8, REGION);   /* vmwrite hook */
    patch_ucode(REGION, lp, n);
    return n;
}

#define CLOBBERS "rax","rbx","rcx","rdx","rsi","rdi", \
    "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"

/* Prep input regs from [rbp+..], fire (loops internally), then NO store-back
 * (input stays constant -> every firing is identical work, for timing). */
#define FIRE_PREP \
    "mov r14, [rbp + 32]\n\t"  "mov r11, [rbp + 24]\n\t" \
    "mov r12, [rbp + 16]\n\t"  "mov rsi, [rbp + 8]\n\t"  "mov rdi, [rbp + 0]\n\t" \
    "lea r15, [rdi + rdi]\n\t" "lea r13, [rsi + rsi]\n\t" \
    "lea r9,  [r12 + r12]\n\t" "lea r10, [r11 + r11]\n\t" \
    "imul rbx, r14, 19\n\t"    "imul rdx, r11, 19\n\t" \
    "xor eax, eax\n\t"         "xor r8d, r8d\n\t" \
    "vmwrite rcx, rdx\n\t"

/* same, but store the squared result (rdi,rsi,r12,r11,r14) back -- for verify */
#define FIRE_STORE \
    FIRE_PREP \
    "mov [rbp + 0],  rdi\n\t" "mov [rbp + 8],  rsi\n\t" "mov [rbp + 16], r12\n\t" \
    "mov [rbp + 24], r11\n\t" "mov [rbp + 32], r14\n\t"

/* mul-as-square fire: a = b = element; fire (loops a=a*a internally). No store
 * (constant input, for timing). a=rdi..r14, b=r15,r13,r9,r10,rbx. */
#define FIRE_MUL_PREP \
    "mov rdi, [rbp + 0]\n\t"  "mov rsi, [rbp + 8]\n\t"  "mov r12, [rbp + 16]\n\t" \
    "mov r11, [rbp + 24]\n\t" "mov r14, [rbp + 32]\n\t" \
    "mov r15, [rbp + 0]\n\t"  "mov r13, [rbp + 8]\n\t"  "mov r9,  [rbp + 16]\n\t" \
    "mov r10, [rbp + 24]\n\t" "mov rbx, [rbp + 32]\n\t" \
    "xor eax, eax\n\t"        "xor r8d, r8d\n\t" \
    "vmwrite rcx, rdx\n\t"

/* same, store result (rdi,rsi,r12,r11,r14) back -- for verify */
#define FIRE_MUL_STORE \
    FIRE_MUL_PREP \
    "mov [rbp + 0],  rdi\n\t" "mov [rbp + 8],  rsi\n\t" "mov [rbp + 16], r12\n\t" \
    "mov [rbp + 24], r11\n\t" "mov [rbp + 32], r14\n\t"

#define REP2(x)  x x
#define REP4(x)  REP2(x) REP2(x)
#define REP8(x)  REP4(x) REP4(x)
#define REP16(x) REP8(x) REP8(x)
#define UNROLL 16
#define REPS   2000
#define TRIALS 60

static inline uint64_t rdtsc_start(void){uint32_t lo,hi;asm volatile("cpuid\n\trdtsc":"=a"(lo),"=d"(hi)::"rbx","rcx","memory");return((uint64_t)hi<<32)|lo;}
static inline uint64_t rdtsc_end(void){uint32_t lo,hi;asm volatile("rdtscp":"=a"(lo),"=d"(hi)::"rcx","memory");asm volatile("cpuid":::"rax","rbx","rcx","rdx","memory");return((uint64_t)hi<<32)|lo;}

/* one store-back firing (verification helper) */
static void sq_n_store(uint64_t *v) {
    register uint64_t *p asm("rbp") = v;
    asm volatile(FIRE_STORE : : "r"(p) : CLOBBERS);
}

/* min cyc per firing over a unrolled dependent batch (input constant) */
static uint64_t time_fire(uint64_t *v) {
    uint64_t best = ~0ULL;
    for (int t = 0; t < TRIALS; t++) {
        register uint64_t *p asm("rbp") = v;
        uint64_t a = rdtsc_start();
        for (int r = 0; r < REPS; r++)
            asm volatile(REP16(FIRE_PREP) : : "r"(p) : CLOBBERS);
        uint64_t c = rdtsc_end() - a;
        if (c < best) best = c;
    }
    return best / ((uint64_t)REPS * UNROLL);
}

/* one mul-as-square store-back firing (verification helper) */
static void mul_n_store(uint64_t *v) {
    register uint64_t *p asm("rbp") = v;
    asm volatile(FIRE_MUL_STORE : : "r"(p) : CLOBBERS);
}

static uint64_t time_mul_fire(uint64_t *v) {
    uint64_t best = ~0ULL;
    for (int t = 0; t < TRIALS; t++) {
        register uint64_t *p asm("rbp") = v;
        uint64_t a = rdtsc_start();
        for (int r = 0; r < REPS; r++)
            asm volatile(REP16(FIRE_MUL_PREP) : : "r"(p) : CLOBBERS);
        uint64_t c = rdtsc_end() - a;
        if (c < best) best = c;
    }
    return best / ((uint64_t)REPS * UNROLL);
}

/* ---- independent C oracle: 5x51 mul + canonical freeze (for verifying a
 * RESCHEDULED mul body — the looped-vs-chained check uses the same body twice
 * so it can't catch an arithmetic bug). ---- */
typedef unsigned __int128 u128;
static void fe_mul_ref(uint64_t h[5], const uint64_t a[5], const uint64_t b[5]) {
    u128 t0 = (u128)a[0]*b[0] + 19*((u128)a[1]*b[4] + (u128)a[2]*b[3] + (u128)a[3]*b[2] + (u128)a[4]*b[1]);
    u128 t1 = (u128)a[0]*b[1] + (u128)a[1]*b[0] + 19*((u128)a[2]*b[4] + (u128)a[3]*b[3] + (u128)a[4]*b[2]);
    u128 t2 = (u128)a[0]*b[2] + (u128)a[1]*b[1] + (u128)a[2]*b[0] + 19*((u128)a[3]*b[4] + (u128)a[4]*b[3]);
    u128 t3 = (u128)a[0]*b[3] + (u128)a[1]*b[2] + (u128)a[2]*b[1] + (u128)a[3]*b[0] + 19*((u128)a[4]*b[4]);
    u128 t4 = (u128)a[0]*b[4] + (u128)a[1]*b[3] + (u128)a[2]*b[2] + (u128)a[3]*b[1] + (u128)a[4]*b[0];
    uint64_t c;
    t1 += (uint64_t)(t0 >> 51); h[0] = (uint64_t)t0 & MASK51;
    t2 += (uint64_t)(t1 >> 51); h[1] = (uint64_t)t1 & MASK51;
    t3 += (uint64_t)(t2 >> 51); h[2] = (uint64_t)t2 & MASK51;
    t4 += (uint64_t)(t3 >> 51); h[3] = (uint64_t)t3 & MASK51;
    c = (uint64_t)(t4 >> 51);   h[4] = (uint64_t)t4 & MASK51;
    h[0] += 19 * c; c = h[0] >> 51; h[0] &= MASK51; h[1] += c;
}
/* fully reduce mod 2^255-19 to the unique canonical limbs */
static void fe_freeze(uint64_t h[5]) {
    const uint64_t m = MASK51;
    for (int r = 0; r < 3; r++) {
        uint64_t c;
        c = h[0] >> 51; h[0] &= m; h[1] += c;
        c = h[1] >> 51; h[1] &= m; h[2] += c;
        c = h[2] >> 51; h[2] &= m; h[3] += c;
        c = h[3] >> 51; h[3] &= m; h[4] += c;
        c = h[4] >> 51; h[4] &= m; h[0] += 19 * c;
    }
    /* conditional subtract p: t = h + 19; if it carries out of bit 255, h>=p */
    uint64_t cc, t0,t1,t2,t3,t4;
    t0 = h[0] + 19; cc = t0 >> 51; t0 &= m;
    t1 = h[1] + cc; cc = t1 >> 51; t1 &= m;
    t2 = h[2] + cc; cc = t2 >> 51; t2 &= m;
    t3 = h[3] + cc; cc = t3 >> 51; t3 &= m;
    t4 = h[4] + cc; cc = t4 >> 51; t4 &= m;   /* cc=1 -> h>=p, use t (=h-p) */
    uint64_t mask = ~(cc - 1);
    h[0] = (h[0] & ~mask) | (t0 & mask);
    h[1] = (h[1] & ~mask) | (t1 & mask);
    h[2] = (h[2] & ~mask) | (t2 & mask);
    h[3] = (h[3] & ~mask) | (t3 & mask);
    h[4] = (h[4] & ~mask) | (t4 & mask);
}
static int fe_eq_canon(const uint64_t x[5], const uint64_t y[5]) {
    uint64_t a[5], b[5];
    for (int i=0;i<5;i++){a[i]=x[i];b[i]=y[i];}
    fe_freeze(a); fe_freeze(b);
    return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2]&&a[3]==b[3]&&a[4]==b[4];
}

int main(void) {
    assign_to_core(0);

    const uint64_t input[5] = {
        0x123456789ABCDULL & MASK51, 0x2468ACE13579BULL & MASK51,
        0x13579BDF02468ULL & MASK51, 0x0FEDCBA987654ULL & MASK51,
        0x1111122222333ULL & MASK51
    };

    printf("=== probe_looped_fieldop: fe_sq looped inside ONE firing ===\n");
    printf("(dependency chain x->x^2->...; slope = looped per-sq latency, tax removed)\n\n");

    /* ---- loop-control self-test (bounded; isolates control from sq body) ---- */
    printf("[selftest] backward-loop control (acc += 7):\n");
    int st_ok = 1;
    int counts[] = {1, 2, 10};
    for (unsigned k = 0; k < sizeof(counts)/sizeof(counts[0]); k++) {
        uint64_t r = selftest_loop(counts[k]);
        int ok = (r == (uint64_t)(7 * counts[k]));
        st_ok &= ok;
        printf("   count=%-2d  RAX=%-3" PRIu64 " exp %-3d  %s\n",
               counts[k], r, 7*counts[k], ok ? "PASS" : "FAIL");
    }
    if (!st_ok) {
        printf("\nloop control itself is broken -- not the sq body. Stopping before sq loop.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }
    printf("\n");

    /* ---- correctness: looped N == N separate single-sq firings ---- */
    printf("[verify] looped-N result vs N chained single firings:\n");
    int ok_all = 1;
    int vns[] = {1, 2, 4, 8};
    for (unsigned k = 0; k < sizeof(vns)/sizeof(vns[0]); k++) {
        int N = vns[k];
        /* reference: N applications of a single sq (install N=1, fire N times) */
        uint64_t ref[5]; memcpy(ref, input, sizeof ref);
        install_loop(1);
        for (int i = 0; i < N; i++) sq_n_store(ref);
        /* test: one firing that loops N times internally */
        uint64_t got[5]; memcpy(got, input, sizeof got);
        install_loop(N);
        sq_n_store(got);
        int ok = (memcmp(ref, got, sizeof ref) == 0);
        ok_all &= ok;
        printf("   N=%-2d  %s\n", N, ok ? "OK" : "MISMATCH");
        if (!ok) {
            printf("      ref:"); for (int j=0;j<5;j++) printf(" %013" PRIx64, ref[j]); printf("\n");
            printf("      got:"); for (int j=0;j<5;j++) printf(" %013" PRIx64, got[j]); printf("\n");
        }
    }
    if (!ok_all) {
        printf("\nloop mechanics broken -- timing below is meaningless, fix first.\n");
    }

    /* ---- timing: cyc vs internal iteration count ---- */
    printf("\n[timing] cyc per firing as N (internal squarings) grows:\n");
    printf("   %4s %8s %12s %14s\n", "N", "triads", "cyc/firing", "marginal cyc/sq");
    printf("   %4s %8s %12s %14s\n", "--", "------", "----------", "---------------");
    int Ns[] = {1, 2, 4, 8, 16, 32};
    double prev_cyc = 0; int prev_N = 0;
    uint64_t buf[5];
    for (unsigned k = 0; k < sizeof(Ns)/sizeof(Ns[0]); k++) {
        int N = Ns[k];
        int n = install_loop(N);
        memcpy(buf, input, sizeof buf);
        double cyc = (double)time_fire(buf);
        char slope[32] = "  (baseline)";
        if (prev_N) snprintf(slope, sizeof slope, "%12.2f", (cyc - prev_cyc)/(N - prev_N));
        printf("   %4d %8d %12.2f %14s\n", N, n, cyc, slope);
        prev_cyc = cyc; prev_N = N;
    }

    printf("\nINTERPRET: marginal cyc/sq is the looped per-op latency (firing tax\n");
    printf("amortized). standalone fe_sq=123, native amd64-64 fe_square=86.\n");
    printf("  <~86  -> looped microcode beats native per-op; interpreter-loop wins.\n");
    printf("  ~123  -> tax is intrinsic per-op; structural ceiling, stop.\n");

    /* ================= fe_mul (66 triads), chained as a=a*a ================= */
    printf("\n========================================================\n");
    printf("[verify] mul-as-square looped-N vs N chained single mul firings:\n");
    int mok_all = 1;
    for (unsigned k = 0; k < sizeof(vns)/sizeof(vns[0]); k++) {
        int N = vns[k];
        uint64_t ref[5]; memcpy(ref, input, sizeof ref);
        install_mul_loop(1);
        for (int i = 0; i < N; i++) mul_n_store(ref);
        uint64_t got[5]; memcpy(got, input, sizeof got);
        install_mul_loop(N);
        mul_n_store(got);
        int ok = (memcmp(ref, got, sizeof ref) == 0);
        mok_all &= ok;
        printf("   N=%-2d  %s\n", N, ok ? "OK" : "MISMATCH");
        if (!ok) {
            printf("      ref:"); for (int j=0;j<5;j++) printf(" %013" PRIx64, ref[j]); printf("\n");
            printf("      got:"); for (int j=0;j<5;j++) printf(" %013" PRIx64, got[j]); printf("\n");
        }
    }
    if (!mok_all) printf("\nmul loop mechanics broken -- mul timing below is meaningless.\n");

    /* independent oracle: microcode mul(in,in) must equal C-ref in^2 (canonical).
     * This is what actually validates a RESCHEDULED mul body. */
    {
        uint64_t got[5]; memcpy(got, input, sizeof got);
        install_mul_loop(1);
        mul_n_store(got);                       /* got = microcode mul(input,input) */
        uint64_t ref[5];
        fe_mul_ref(ref, input, input);          /* ref = C-ref input^2 */
        int ok = fe_eq_canon(got, ref);
        printf("[verify] microcode mul(in,in) vs C-ref in^2 (canonical):  %s\n",
               ok ? "OK" : "MISMATCH  <-- mul body arithmetic is WRONG");
        if (!ok) {
            uint64_t g[5],r[5]; memcpy(g,got,sizeof g); memcpy(r,ref,sizeof r);
            fe_freeze(g); fe_freeze(r);
            printf("      ucode:"); for(int j=0;j<5;j++) printf(" %013" PRIx64, g[j]); printf("\n");
            printf("      cref :"); for(int j=0;j<5;j++) printf(" %013" PRIx64, r[j]); printf("\n");
            mok_all = 0;
        }
    }

    printf("\n[timing] cyc per firing as N (internal a=a*a) grows:\n");
    printf("   %4s %8s %12s %14s\n", "N", "triads", "cyc/firing", "marginal cyc/mul");
    printf("   %4s %8s %12s %14s\n", "--", "------", "----------", "----------------");
    prev_cyc = 0; prev_N = 0;
    for (unsigned k = 0; k < sizeof(Ns)/sizeof(Ns[0]); k++) {
        int N = Ns[k];
        int n = install_mul_loop(N);
        memcpy(buf, input, sizeof buf);
        double cyc = (double)time_mul_fire(buf);
        char slope[32] = "  (baseline)";
        if (prev_N) snprintf(slope, sizeof slope, "%12.2f", (cyc - prev_cyc)/(N - prev_N));
        printf("   %4d %8d %12.2f %14s\n", N, n, cyc, slope);
        prev_cyc = cyc; prev_N = N;
    }

    printf("\nINTERPRET: marginal cyc/mul is the looped per-mul latency (tax amortized).\n");
    printf("standalone fe_mul=123, native amd64-64 fe_mul=100.  Ladder step = 4 mul + 5 sq;\n");
    printf("native step=947. If 4*mul_slope + 5*sq_slope < ~947 (pre-I/O), the looped\n");
    printf("field-op ladder can beat amd64-64 (272k) -- modulo per-op operand I/O.\n");

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
