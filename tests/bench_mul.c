/*
 * bench_mul.c — Monolithic fe_mul in microcode via rdrand hook
 *
 * 25 MACs + carry + reduction, all in one rdrand trigger.
 * f[5] in GP registers, g values loaded via LDZX from static buffer.
 *
 * g_buf layout (static, <4GB for ASZ32):
 *   [0]  g0      [8]  g1      [16] g2      [24] g3      [32] g4
 *   [40] 19*g1   [48] 19*g2   [56] 19*g3   [64] 19*g4
 *
 * Entry convention (rdrand trigger):
 *   RDI=f0  RSI=f1  R12=f2  R11=f3  R14=f4
 *   RDX=g0 (preloaded for first MUL — avoids 1-triad LDZX latency)
 *   RCX=&g_buf  RAX=0  R8=0
 *
 * Output: RDI=h[0] R9=h[1] R10=h[2] RBX=h[3] RAX=h[4]
 *
 * Build: make PROG=bench_mul
 * Run:   sudo taskset -c 0 ./bench_mul_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>
#include <setjmp.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/ldat.h"
#include "../../include/misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL
#define S SEG_DS

/* ── g operand buffer (static, <4GB) ─────────────────────────── */
static uint64_t g_buf[9] __attribute__((aligned(64)));
/* Offsets: g0=0 g1=8 g2=16 g3=24 g4=32 g1_19=40 g2_19=48 g3_19=56 g4_19=64 */

/*
 * Per-limb MAC offsets into g_buf:
 *   c0: f0*g0(0)     f1*g4_19(64) f2*g3_19(56) f3*g2_19(48) f4*g1_19(40)
 *   c1: f0*g1(8)     f1*g0(0)     f2*g4_19(64) f3*g3_19(56) f4*g2_19(48)
 *   c2: f0*g2(16)    f1*g1(8)     f2*g0(0)     f3*g4_19(64) f4*g3_19(56)
 *   c3: f0*g3(24)    f1*g2(16)    f2*g1(8)     f3*g0(0)     f4*g4_19(64)
 *   c4: f0*g4(32)    f1*g3(24)    f2*g2(16)    f3*g1(8)     f4*g0(0)
 */

/* ── Macro: one complete non-terminal limb (14 triads) ───────────
 *
 * Expects: TMP0 = carry from prev (or 0), RDX = first g operand
 *          TMP11 = g_buf pointer
 * Produces: out_limb in specified register, TMP0 = new carry
 *           R8 = 0 (cleared for next limb)
 *           RDX = next limb's first g operand (preloaded)
 *
 * The 5 MAC offsets (m1..m5) index into g_buf.
 * m1's value is already in RDX from the previous transition.
 * m2..m5 are loaded via LDZX overlapped in SETCC triads.
 */

static void install_mul_patch(void) {
    ucode_t mul_patch[] = {

    /* ═══ INIT: save g_buf ptr, start c0 ═══
     * RDX already has g0 from x86.  RCX = g_buf ptr. */
    /* T0 */ { ZEROEXT_DSZ64_DR(TMP11, RCX),
               MUL_DSZ64_DRR(RCX, RDI, RDX),
               ZEROEXT_DSZ64_DR(TMP0, RAX),
               NOP_SEQWORD },

    /* ═══ LIMB c0: f0*g0 + f1*g4_19 + f2*g3_19 + f3*g2_19 + f4*g1_19 ═══ */
    /* MAC1 done in T0. lo=RDX, hi=RCX */
    /* T1 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 64, S),  /* g4_19 for MAC2 */
               NOP_SEQWORD },
    /* T2 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, RSI, RDX),    /* MAC2: f1*g4_19 */
               NOP, NOP_SEQWORD },
    /* T3 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 56, S),  /* g3_19 for MAC3 */
               NOP_SEQWORD },
    /* T4 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R12, RDX),    /* MAC3: f2*g3_19 */
               ADD_DSZ64_DRR(R8, R8, TMP4),
               NOP_SEQWORD },
    /* T5 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 48, S),  /* g2_19 for MAC4 */
               NOP_SEQWORD },
    /* T6 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R11, RDX),    /* MAC4: f3*g2_19 */
               ADD_DSZ64_DRR(R8, R8, TMP5),
               NOP_SEQWORD },
    /* T7 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
               SETCC_CONDB_DR(TMP3, TMP0),
               LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 40, S),  /* g1_19 for MAC5 */
               NOP_SEQWORD },
    /* T8 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
               MUL_DSZ64_DRR(RCX, R14, RDX),    /* MAC5: f4*g1_19 */
               ADD_DSZ64_DRR(R8, R8, TMP4),
               NOP_SEQWORD },
    /* T9 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
               SETCC_CONDB_DR(TMP3, TMP2),
               NOP, NOP_SEQWORD },
    /* carry extraction */
    /* T10 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                ADD_DSZ64_DRR(R8, R8, TMP5),
                NOP_SEQWORD },
    /* T11 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                ADD_DSZ64_DRR(R8, R8, TMP6),
                NOP, NOP_SEQWORD },
    /* T12 */ { SHR_DSZ64_DRI(RDI, TMP9, 13),    /* out[0] → RDI */
                NOP, NOP, NOP_SEQWORD },
    /* transition to c1 */
    /* T13 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 8, S),  /* g1 for c1 MAC1 */
                NOP_SEQWORD },
    /* T14 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                MUL_DSZ64_DRR(RCX, RDI, RDX),    /* c1 MAC1: f0*g1 */
                NOP, NOP_SEQWORD },

    /* ═══ LIMB c1: f0*g1 + f1*g0 + f2*g4_19 + f3*g3_19 + f4*g2_19 ═══ */
    /* T15 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 0, S),  /* g0 */
                NOP_SEQWORD },
    /* T16 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, RSI, RDX),    /* f1*g0 */
                NOP, NOP_SEQWORD },
    /* T17 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                SETCC_CONDB_DR(TMP3, TMP0),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 64, S), /* g4_19 */
                NOP_SEQWORD },
    /* T18 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R12, RDX),    /* f2*g4_19 */
                ADD_DSZ64_DRR(R8, R8, TMP4),
                NOP_SEQWORD },
    /* T19 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 56, S), /* g3_19 */
                NOP_SEQWORD },
    /* T20 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R11, RDX),    /* f3*g3_19 */
                ADD_DSZ64_DRR(R8, R8, TMP5),
                NOP_SEQWORD },
    /* T21 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                SETCC_CONDB_DR(TMP3, TMP0),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 48, S), /* g2_19 */
                NOP_SEQWORD },
    /* T22 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R14, RDX),    /* f4*g2_19 */
                ADD_DSZ64_DRR(R8, R8, TMP4),
                NOP_SEQWORD },
    /* T23 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                NOP, NOP_SEQWORD },
    /* T24 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                ADD_DSZ64_DRR(R8, R8, TMP5),
                NOP_SEQWORD },
    /* T25 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                ADD_DSZ64_DRR(R8, R8, TMP6),
                NOP, NOP_SEQWORD },
    /* T26 */ { SHR_DSZ64_DRI(R9, TMP9, 13),     /* out[1] → R9 */
                NOP, NOP, NOP_SEQWORD },
    /* T27 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 16, S), /* g2 for c2 */
                NOP_SEQWORD },
    /* T28 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                MUL_DSZ64_DRR(RCX, RDI, RDX),    /* c2 MAC1: f0*g2 */
                NOP, NOP_SEQWORD },

    /* ═══ LIMB c2: f0*g2 + f1*g1 + f2*g0 + f3*g4_19 + f4*g3_19 ═══ */
    /* T29 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 8, S),  /* g1 */
                NOP_SEQWORD },
    /* T30 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, RSI, RDX),    /* f1*g1 */
                NOP, NOP_SEQWORD },
    /* T31 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                SETCC_CONDB_DR(TMP3, TMP0),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 0, S),  /* g0 */
                NOP_SEQWORD },
    /* T32 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R12, RDX),    /* f2*g0 */
                ADD_DSZ64_DRR(R8, R8, TMP4),
                NOP_SEQWORD },
    /* T33 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 64, S), /* g4_19 */
                NOP_SEQWORD },
    /* T34 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R11, RDX),    /* f3*g4_19 */
                ADD_DSZ64_DRR(R8, R8, TMP5),
                NOP_SEQWORD },
    /* T35 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                SETCC_CONDB_DR(TMP3, TMP0),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 56, S), /* g3_19 */
                NOP_SEQWORD },
    /* T36 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R14, RDX),    /* f4*g3_19 */
                ADD_DSZ64_DRR(R8, R8, TMP4),
                NOP_SEQWORD },
    /* T37 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                NOP, NOP_SEQWORD },
    /* T38 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                ADD_DSZ64_DRR(R8, R8, TMP5),
                NOP_SEQWORD },
    /* T39 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                ADD_DSZ64_DRR(R8, R8, TMP6),
                NOP, NOP_SEQWORD },
    /* T40 */ { SHR_DSZ64_DRI(R10, TMP9, 13),    /* out[2] → R10 */
                NOP, NOP, NOP_SEQWORD },
    /* T41 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 24, S), /* g3 for c3 */
                NOP_SEQWORD },
    /* T42 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                MUL_DSZ64_DRR(RCX, RDI, RDX),    /* c3 MAC1: f0*g3 */
                NOP, NOP_SEQWORD },

    /* ═══ LIMB c3: f0*g3 + f1*g2 + f2*g1 + f3*g0 + f4*g4_19 ═══ */
    /* T43 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 16, S), /* g2 */
                NOP_SEQWORD },
    /* T44 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, RSI, RDX),    /* f1*g2 */
                NOP, NOP_SEQWORD },
    /* T45 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                SETCC_CONDB_DR(TMP3, TMP0),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 8, S),  /* g1 */
                NOP_SEQWORD },
    /* T46 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R12, RDX),    /* f2*g1 */
                ADD_DSZ64_DRR(R8, R8, TMP4),
                NOP_SEQWORD },
    /* T47 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 0, S),  /* g0 */
                NOP_SEQWORD },
    /* T48 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R11, RDX),    /* f3*g0 */
                ADD_DSZ64_DRR(R8, R8, TMP5),
                NOP_SEQWORD },
    /* T49 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                SETCC_CONDB_DR(TMP3, TMP0),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 64, S), /* g4_19 */
                NOP_SEQWORD },
    /* T50 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R14, RDX),    /* f4*g4_19 */
                ADD_DSZ64_DRR(R8, R8, TMP4),
                NOP_SEQWORD },
    /* T51 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                NOP, NOP_SEQWORD },
    /* T52 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                ADD_DSZ64_DRR(R8, R8, TMP5),
                NOP_SEQWORD },
    /* T53 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                ADD_DSZ64_DRR(R8, R8, TMP6),
                NOP, NOP_SEQWORD },
    /* T54 */ { SHR_DSZ64_DRI(RBX, TMP9, 13),    /* out[3] → RBX */
                NOP, NOP, NOP_SEQWORD },
    /* T55 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
                NOTAND_DSZ64_DRR(R8, R8, R8),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 32, S), /* g4 for c4 */
                NOP_SEQWORD },
    /* T56 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                MUL_DSZ64_DRR(RCX, RDI, RDX),    /* c4 MAC1: f0*g4 */
                NOP, NOP_SEQWORD },

    /* ═══ LIMB c4: f0*g4 + f1*g3 + f2*g2 + f3*g1 + f4*g0 ═══ */
    /* T57 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 24, S), /* g3 */
                NOP_SEQWORD },
    /* T58 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, RSI, RDX),    /* f1*g3 */
                NOP, NOP_SEQWORD },
    /* T59 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                SETCC_CONDB_DR(TMP3, TMP0),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 16, S), /* g2 */
                NOP_SEQWORD },
    /* T60 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R12, RDX),    /* f2*g2 */
                ADD_DSZ64_DRR(R8, R8, TMP4),
                NOP_SEQWORD },
    /* T61 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 8, S),  /* g1 */
                NOP_SEQWORD },
    /* T62 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R11, RDX),    /* f3*g1 */
                ADD_DSZ64_DRR(R8, R8, TMP5),
                NOP_SEQWORD },
    /* T63 */ { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
                SETCC_CONDB_DR(TMP3, TMP0),
                LDZX_DSZ64_ASZ32_SC1_DRI(RDX, TMP11, 0, S),  /* g0 */
                NOP_SEQWORD },
    /* T64 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                MUL_DSZ64_DRR(RCX, R14, RDX),    /* f4*g0 */
                ADD_DSZ64_DRR(R8, R8, TMP4),
                NOP_SEQWORD },
    /* T65 */ { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
                SETCC_CONDB_DR(TMP3, TMP2),
                NOP, NOP_SEQWORD },
    /* T66 */ { SHR_DSZ64_DRI(TMP8, TMP2, 51),
                ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                ADD_DSZ64_DRR(R8, R8, TMP5),
                NOP_SEQWORD },
    /* T67 */ { SHL_DSZ64_DRI(TMP9, TMP2, 13),
                ADD_DSZ64_DRR(R8, R8, TMP6),
                NOP, NOP_SEQWORD },
    /* T68 */ { SHR_DSZ64_DRI(RAX, TMP9, 13),    /* out[4] → RAX */
                NOP, NOP, NOP_SEQWORD },

    /* ═══ FINAL REDUCTION: carry*19 → out[0] ═══ */
    /* T69 */ { SHL_DSZ64_DRI(TMP1, R8, 13),
                NOP, NOP, NOP_SEQWORD },
    /* T70 */ { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
                NOP, NOP, NOP_SEQWORD },
    /* T71 */ { MUL_DSZ64_DIR(TMP1, 19, TMP0),   /* TMP0 = carry*19 */
                NOP, NOP, NOP_SEQWORD },
    /* T72 */ { ADD_DSZ64_DRR(RDI, RDI, TMP0),
                NOP, NOP, NOP_SEQWORD },
    /* T73 */ { SHR_DSZ64_DRI(TMP0, RDI, 51),
                NOP, NOP, NOP_SEQWORD },
    /* T74 */ { SHL_DSZ64_DRI(TMP1, RDI, 13),
                ADD_DSZ64_DRR(R9, R9, TMP0),
                NOP, NOP_SEQWORD },
    /* T75 */ { SHR_DSZ64_DRI(RDI, TMP1, 13),
                NOP, NOP, END_SEQWORD },

    }; /* end mul_patch */

    printf("fe_mul patch: %zu triads\n", ARRAY_SZ(mul_patch));

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, mul_patch, ARRAY_SZ(mul_patch));
    hook_match_and_patch(1, 0x0428, 0x7c00);  /* slot 1, rdrand hook */
    printf("  Hooked rdrand (0x0428) -> U7c00\n");
}


/* ── x86 wrapper ─────────────────────────────────────────────── */
static struct {
    uint64_t f_ptr;
    uint64_t out_ptr;
} mul_args;

static void fe_mul_ucode(uint64_t *h, const uint64_t *f, const uint64_t *g) {
    /* Fill g_buf */
    g_buf[0]=g[0]; g_buf[1]=g[1]; g_buf[2]=g[2]; g_buf[3]=g[3]; g_buf[4]=g[4];
    g_buf[5]=19*g[1]; g_buf[6]=19*g[2]; g_buf[7]=19*g[3]; g_buf[8]=19*g[4];

    mul_args.f_ptr = (uint64_t)f;
    mul_args.out_ptr = (uint64_t)h;

    asm volatile(
        /* Load f[] into registers */
        "mov rax, [%[args]]\n\t"       /* rax = f_ptr */
        "mov rdi, [rax]\n\t"
        "mov rsi, [rax+8]\n\t"
        "mov r12, [rax+16]\n\t"
        "mov r11, [rax+24]\n\t"
        "mov r14, [rax+32]\n\t"

        /* RCX = g_buf ptr, RDX = g0 (preloaded for first MUL) */
        "lea rcx, [%[gb]]\n\t"
        "mov rdx, [rcx]\n\t"           /* RDX = g0 */

        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* Trigger rdrand → microcode fe_mul */
        "rdrand r15\n\t"

        /* Store results */
        "mov rcx, [%[args]+8]\n\t"     /* out_ptr */
        "mov [rcx],    rdi\n\t"
        "mov [rcx+8],  r9\n\t"
        "mov [rcx+16], r10\n\t"
        "mov [rcx+24], rbx\n\t"
        "mov [rcx+32], rax\n\t"
        :
        : [args] "r"(&mul_args), [gb] "m"(g_buf[0])
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory"
    );
}


/* ── Reference C ─────────────────────────────────────────────── */
static void fe_mul_ref(uint64_t *h, const uint64_t *f, const uint64_t *g) {
    uint64_t f0=f[0],f1=f[1],f2=f[2],f3=f[3],f4=f[4];
    uint64_t g0=g[0],g1=g[1],g2=g[2],g3=g[3],g4=g[4];
    uint64_t g1_19=19*g1,g2_19=19*g2,g3_19=19*g3,g4_19=19*g4;

    __uint128_t c0=(__uint128_t)f0*g0+(__uint128_t)f1*g4_19+(__uint128_t)f2*g3_19+(__uint128_t)f3*g2_19+(__uint128_t)f4*g1_19;
    __uint128_t c1=(__uint128_t)f0*g1+(__uint128_t)f1*g0+(__uint128_t)f2*g4_19+(__uint128_t)f3*g3_19+(__uint128_t)f4*g2_19;
    __uint128_t c2=(__uint128_t)f0*g2+(__uint128_t)f1*g1+(__uint128_t)f2*g0+(__uint128_t)f3*g4_19+(__uint128_t)f4*g3_19;
    __uint128_t c3=(__uint128_t)f0*g3+(__uint128_t)f1*g2+(__uint128_t)f2*g1+(__uint128_t)f3*g0+(__uint128_t)f4*g4_19;
    __uint128_t c4=(__uint128_t)f0*g4+(__uint128_t)f1*g3+(__uint128_t)f2*g2+(__uint128_t)f3*g1+(__uint128_t)f4*g0;
    uint64_t carry;
    carry=(uint64_t)(c0>>51);h[0]=(uint64_t)c0&MASK51;c1+=carry;
    carry=(uint64_t)(c1>>51);h[1]=(uint64_t)c1&MASK51;c2+=carry;
    carry=(uint64_t)(c2>>51);h[2]=(uint64_t)c2&MASK51;c3+=carry;
    carry=(uint64_t)(c3>>51);h[3]=(uint64_t)c3&MASK51;c4+=carry;
    carry=(uint64_t)(c4>>51);h[4]=(uint64_t)c4&MASK51;
    h[0]+=carry*19;carry=h[0]>>51;h[0]&=MASK51;h[1]+=carry;
}


/* ── Timing ──────────────────────────────────────────────────── */
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

    printf("=== fe_mul microcode benchmark ===\n\n");
    install_mul_patch();

    /* ── Correctness ──────────────────────────────────────────── */
    printf("\n--- Correctness ---\n");
    {
        uint64_t f[5] = { 0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                          0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                          0x000216936D3CD6E5ULL };
        uint64_t g[5] = { 0x0003A4B2E8F1C023ULL, 0x0005D1C7A3B82F4EULL,
                          0x000291E5F6D0A71BULL, 0x0006C3F4E2B598D7ULL,
                          0x000147A8C5E3D962ULL };
        uint64_t ref[5], ucd[5];
        fe_mul_ref(ref, f, g);
        fe_mul_ucode(ucd, f, g);

        int ok = memcmp(ref, ucd, sizeof(ref)) == 0;
        printf("  Single:  %s\n", ok ? "PASS" : "FAIL");
        if (!ok) {
            for (int i = 0; i < 5; i++)
                printf("    [%d] ref=%016" PRIx64 " ucd=%016" PRIx64 " %s\n",
                       i, ref[i], ucd[i], ref[i]==ucd[i]?"":"***");
            return 1;
        }

        /* Iterated: mul(result, result, g) 1000 times */
        uint64_t ri[5], ui[5];
        memcpy(ri, f, sizeof(ri));
        memcpy(ui, f, sizeof(ui));
        for (int i = 0; i < 1000; i++) {
            fe_mul_ref(ri, ri, g);
            fe_mul_ucode(ui, ui, g);
        }
        ok = memcmp(ri, ui, sizeof(ri)) == 0;
        printf("  1000 mul: %s\n", ok ? "PASS" : "FAIL");
        if (!ok) return 1;
    }

    /* ── Benchmark ────────────────────────────────────────────── */
    printf("\n--- fe_mul: %d ops/batch, %d batches ---\n\n", BATCH, REPS);
    {
        uint64_t f[5] = { 0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                          0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                          0x000216936D3CD6E5ULL };
        uint64_t g[5] = { 0x0003A4B2E8F1C023ULL, 0x0005D1C7A3B82F4EULL,
                          0x000291E5F6D0A71BULL, 0x0006C3F4E2B598D7ULL,
                          0x000147A8C5E3D962ULL };
        uint64_t tmp[5];

        /* Native C */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
            memcpy(tmp, f, sizeof(tmp));
            t0 = rdtsc_start();
            for (int i = 0; i < BATCH; i++) fe_mul_ref(tmp, tmp, g);
            t1 = rdtsc_end();
            uint64_t dt = t1 - t0;
            sum += dt; if (dt < min) min = dt;
        }
        printf("  Native C:      min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min/BATCH, sum/REPS/BATCH);

        /* Microcode */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS; r++) {
            memcpy(tmp, f, sizeof(tmp));
            t0 = rdtsc_start();
            for (int i = 0; i < BATCH; i++) fe_mul_ucode(tmp, tmp, g);
            t1 = rdtsc_end();
            uint64_t dt = t1 - t0;
            sum += dt; if (dt < min) min = dt;
        }
        printf("  Microcode:     min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min/BATCH, sum/REPS/BATCH);
    }

    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nM&P reset.\n");
    return 0;
}
