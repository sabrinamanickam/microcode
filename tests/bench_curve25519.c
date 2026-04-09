/*
 * bench_curve25519.c — Full X25519 with microcode-accelerated field arithmetic
 *
 * Two monolithic microcode patches installed simultaneously:
 *   Slot 0: vmwrite (0x0cd8) → fe_sq  at 0x7c00  (~62 triads)
 *   Slot 1: rdrand  (0x0428) → fe_mul at 0x7cf8  (~75 triads)
 *
 * Both patches do all 15 MACs + carry propagation + reduction in microcode.
 * Only 1 redirect per field op (~5 cy).
 *
 * With 128 seqword entries in bank 0:
 *   fe_sq:  seq_start=0,   uses 0..61   (62 entries)
 *   fe_mul: base=0xf8, seq_start=0x3e=62, uses 62..136
 *           BUT 136 > 128 → need to check!
 *   Actually: 0x7cf8 - 0x7c00 = 0xf8, bank = 0xf8 % 4 = 0,
 *   seq_start = (0xf8/4) % 0x80 = 0x3e = 62.
 *   fe_mul needs ~75 entries: 62+75 = 137 > 128 → WRAPS!
 *
 * Fix: put fe_mul in a different bank.
 *   0x7cfa: bank = 0xfa%4 = 2, seq_start = (0xfa/4)%0x80 = 0x3e = 62
 *   Still bank 2 is empty, so 128-62 = 66 entries available. Need ~75. Tight.
 *
 *   0x7c01: bank=1, seq_start=0. 128 entries. But NOT 4-aligned, bad.
 *   0x7c04: bank=0, seq_start=1. Same bank as fe_sq!
 *   0x7c01: not aligned.
 *   0x7cfd: bank=1, seq_start=(0xfd/4)%0x80=0x3f=63. Only 65 slots.
 *   0x7c01: bank 1, seq 0 → 128 slots, but addr must be even for hook.
 *           0x7c02: even, bank=2, seq_start=0 → 128 slots! ✓
 *           But 0x7c02 is within fe_sq's uop range (0x7c00-0x7cf7).
 *
 * REVISED LAYOUT:
 *   fe_sq:  0x7c00, 62 triads = 248 uop slots (0x7c00-0x7cf7)
 *           bank 0, seq 0-61
 *   fe_mul: 0x7cf9, needs even addr → 0x7cfa
 *           bank 2 (0xfa%4=2), seq_start=62 ((0xfa/4)%0x80)
 *           Need ≤66 entries. Must fit fe_mul in 66 triads.
 *
 * ALTERNATIVE: Use register I/O for both (like bench_mono).
 * This avoids load/store overhead. Proven to work.
 *
 * Build: make PROG=bench_curve25519
 * Run:   sudo taskset -c 0 ./bench_curve25519_static
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

/* Segment selector for LDZX/STAD in microcode.
 * SEG_DS (0x18) confirmed working via test_ldzx.c.
 * The old SEG=3 from mac_ldzx.c was wrong. */
#define SEG SEG_DS
#include "../../include/misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL

/* Forward declarations — smoke_test_vmwrite needs these before definition */
static void fe_sq(const uint64_t *a, uint64_t *out) __attribute__((noinline));
static void fe_sq_ref(const uint64_t *a, uint64_t *out) __attribute__((noinline));
static void fe_mul_ref(uint64_t *h, const uint64_t *f, const uint64_t *g) __attribute__((noinline));

/* ══════════════════════════════════════════════════════════════════
 *  CRASH PROTECTION — signal handler + setjmp/longjmp recovery
 *
 *  Every dangerous section (vmwrite smoke test, correctness tests,
 *  benchmarks) is wrapped in SAFE_BEGIN / SAFE_END.  A crash inside
 *  the block jumps back to the SAFE_BEGIN point with got_signal != 0.
 * ══════════════════════════════════════════════════════════════════ */
static sigjmp_buf jmpbuf;
static volatile sig_atomic_t got_signal = 0;

static void crash_handler(int sig) {
    got_signal = sig;
    siglongjmp(jmpbuf, sig);
}

static void install_crash_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;   /* allow re-entry after recovery */
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);  /* timeout watchdog */
}

/* Arm a watchdog: if the block doesn't finish in `sec` seconds,
 * SIGALRM fires → crash_handler → siglongjmp back to SAFE_BEGIN.
 * Call watchdog_disarm() on success. */
static inline void watchdog_arm(unsigned sec) { alarm(sec); }
static inline void watchdog_disarm(void)      { alarm(0); }

/* Usage:
 *   SAFE_BEGIN("description") {
 *       ... dangerous code ...
 *   } SAFE_END
 *
 *   If the block crashes, prints the signal and continues after SAFE_END.
 */
#define SAFE_BEGIN(desc) \
    do { \
        const char *_safe_desc = (desc); \
        got_signal = 0; \
        if (sigsetjmp(jmpbuf, 1) != 0) { \
            fprintf(stderr, "  [CRASH] %s — caught signal %d, recovering\n", \
                    _safe_desc, (int)got_signal); \
            init_match_and_patch(); \
            do_fix_IN_patch(); \
        } else

#define SAFE_END \
    } while (0)

/* ══════════════════════════════════════════════════════════════════
 *  MICROCODE PATCHES
 *
 *  fe_sq: 62 triads at 0x7c00.  Hooked on vmwrite (0x0cd8).
 *  Identical to bench_mono.c — all 15 MACs + carry + reduction.
 *
 *  Register convention (caller → microcode):
 *    RDI = a0        RSI = a1        R12 = a2
 *    R11 = a3        R14 = a4        R15 = d0 = 2*a0
 *    R13 = d1 = 2*a1 R9  = d2 = 2*a2 R10 = d3 = 2*a3
 *    RBX = r4 = 19*a4 RDX = r3 = 19*a3
 *    RAX = 0 (acc_lo)  R8 = 0 (acc_hi)
 *
 *  Output: RDI=out[0] R9=out[1] R10=out[2] RBX=out[3] RAX=out[4]
 * ══════════════════════════════════════════════════════════════════ */
static ucode_t g_sq_patch[64];
static int g_sq_patch_len = 0;

static void build_sq_patch(void) {
    if (g_sq_patch_len > 0) return;  /* already built */

    ucode_t sq_patch[] = {

    /* ═══ LIMB c0 = a0*a0 + d1*r4 + d2*r3 ═══ */
    { ZEROEXT_DSZ64_DR(TMP0, RAX),
      MUL_DSZ64_DRR(RCX, RDI, RDI),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, RDI),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R13),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, R13),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RDX, R9),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R9),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(RDI, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ c0→c1 transition ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(R13, RSI),
      NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, R13),
      ADD_DSZ64_DRR(R9, R12, R12),
      NOP_SEQWORD },

    /* ═══ LIMB c1 = d0*a1 + r3*a3 + d2*r4 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, R13),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R9),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R9),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(R9, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ c1→c2 transition ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(RDX, R12),
      NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, RDX),
      ZEROEXT_DSZ64_DR(R13, RSI),
      NOP_SEQWORD },

    /* ═══ LIMB c2 = d0*a2 + a1*a1 + d3*r4 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RSI, R13),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, R13),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R10),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R10),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(R10, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ c2→c3 transition ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(RDX, R11),
      NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, RDX),
      ADD_DSZ64_DRR(R13, RSI, RSI),
      NOP_SEQWORD },

    /* ═══ LIMB c3 = d0*a3 + d1*a2 + r4*a4 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RSI, R12),
      ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { NOP,
      MUL_DSZ64_DRR(RCX, R13, RSI),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, RSI),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R14, RBX),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, RBX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(RBX, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ c3→c4 transition ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      NOP, NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, R14),
      NOP, NOP_SEQWORD },

    /* ═══ LIMB c4 = d0*a4 + d1*a3 + a2*a2 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, R14),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R13, R11),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, R11),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R12, R12),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R12),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(RAX, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ FINAL REDUCTION ═══
     * carry = (R8 << 13) | (TMP2 >> 51) = (R8<<13) | TMP8
     * Then carry * 19 → add to out[0], propagate to out[1].
     * MUL_DSZ64_DRI: multiply reg by 5-bit immediate (19 fits).
     * MUL convention: hi→dst, lo→src. We only need lo (carry is small). */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOP, NOP, NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      NOP, NOP, NOP_SEQWORD },
    /* TMP0 = carry.  carry*19 via MUL immediate: hi→TMP1(discard), lo→TMP0 */
    { MUL_DSZ64_DIR(TMP1, 19, TMP0),
      NOP, NOP, NOP_SEQWORD },
    /* TMP0 now = carry*19 (lo result). Add to out[0] */
    { ADD_DSZ64_DRR(RDI, RDI, TMP0),
      NOP, NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, RDI, 51),
      NOP, NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP1, RDI, 13),
      ADD_DSZ64_DRR(R9, R9, TMP0),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(RDI, TMP1, 13),
      NOP, NOP, END_SEQWORD }

    }; /* end sq_patch */

    g_sq_patch_len = ARRAY_SZ(sq_patch);
    memcpy(g_sq_patch, sq_patch, sizeof(sq_patch));
    printf("fe_sq patch: %d triads\n", g_sq_patch_len);
}

/* ══════════════════════════════════════════════════════════════════
 *  LDZX/STAD variant — loads, precompute, MAC body, stores all in
 *  microcode.  x86 side: 1 instruction (vmwrite).
 *
 *  Entry: RCX = pointer to uint64_t[5] (in-place, read+write)
 *  The pointer is saved in TMP11.  All loads use ASZ32 (SEG=3).
 *
 *  Prologue (7 triads): LDZX a[0..4], compute d0-d3 overlapped,
 *    copy a4→RBX, a3→RDX, MUL×19 for r4/r3, clear accumulator.
 *  Body: identical 50-triad MAC + transitions (c0 through c4).
 *  Reduction: 7 triads (MUL_DRI ×19).
 *  Epilogue (5 triads): STAD out[0..4] back to [TMP11].
 * ══════════════════════════════════════════════════════════════════ */
static ucode_t g_sq_mem_patch[80];
static int g_sq_mem_patch_len = 0;

static void build_sq_mem_patch(void) {
    if (g_sq_mem_patch_len > 0) return;

    ucode_t patch[] = {

    /* ═══ PROLOGUE: load + precompute (7 triads) ═══ */

    /* P0: save pointer, load a[0] */
    { ZEROEXT_DSZ64_DR(TMP11, RCX),
      LDZX_DSZ64_ASZ32_SC1_DR(RDI, RCX, SEG),
      NOP, NOP_SEQWORD },

    /* P1: load a[1], d0 = 2*a0 */
    { LDZX_DSZ64_ASZ32_SC1_DRI(RSI, RCX, 8, SEG),
      ADD_DSZ64_DRR(R15, RDI, RDI),
      NOP, NOP_SEQWORD },

    /* P2: load a[2], d1 = 2*a1 */
    { LDZX_DSZ64_ASZ32_SC1_DRI(R12, RCX, 16, SEG),
      ADD_DSZ64_DRR(R13, RSI, RSI),
      NOP, NOP_SEQWORD },

    /* P3: load a[3], d2 = 2*a2 */
    { LDZX_DSZ64_ASZ32_SC1_DRI(R11, RCX, 24, SEG),
      ADD_DSZ64_DRR(R9, R12, R12),
      NOP, NOP_SEQWORD },

    /* P4: load a[4], d3 = 2*a3, copy a3→RDX for ×19 */
    { LDZX_DSZ64_ASZ32_SC1_DRI(R14, RCX, 32, SEG),
      ADD_DSZ64_DRR(R10, R11, R11),
      ZEROEXT_DSZ64_DR(RDX, R11),
      NOP_SEQWORD },

    /* P5: copy a4→RBX for ×19, r3 = 19*a3 (MUL imm, lo→RDX) */
    { ZEROEXT_DSZ64_DR(RBX, R14),
      MUL_DSZ64_DIR(TMP1, 19, RDX),
      NOP, NOP_SEQWORD },

    /* P6: r4 = 19*a4 (MUL imm, lo→RBX), clear accumulator */
    { NOTAND_DSZ64_DRR(RAX, RAX, RAX),
      MUL_DSZ64_DIR(TMP1, 19, RBX),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      NOP_SEQWORD },


    /* ═══ MAC BODY (same as register-I/O version) ═══ */

    /* ═══ LIMB c0 = a0*a0 + d1*r4 + d2*r3 ═══ */
    { ZEROEXT_DSZ64_DR(TMP0, RAX),
      MUL_DSZ64_DRR(RCX, RDI, RDI),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, RDI),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R13),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, R13),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RDX, R9),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R9),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(RDI, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* c0→c1 transition */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(R13, RSI),
      NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, R13),
      ADD_DSZ64_DRR(R9, R12, R12),
      NOP_SEQWORD },

    /* ═══ LIMB c1 = d0*a1 + r3*a3 + d2*r4 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, R13),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R9),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R9),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(R9, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* c1→c2 transition */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(RDX, R12),
      NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, RDX),
      ZEROEXT_DSZ64_DR(R13, RSI),
      NOP_SEQWORD },

    /* ═══ LIMB c2 = d0*a2 + a1*a1 + d3*r4 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RSI, R13),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, R13),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R10),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R10),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(R10, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* c2→c3 transition */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(RDX, R11),
      NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, RDX),
      ADD_DSZ64_DRR(R13, RSI, RSI),
      NOP_SEQWORD },

    /* ═══ LIMB c3 = d0*a3 + d1*a2 + r4*a4 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RSI, R12),
      ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { NOP,
      MUL_DSZ64_DRR(RCX, R13, RSI),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, RSI),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R14, RBX),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, RBX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(RBX, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* c3→c4 transition */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      NOP, NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, R14),
      NOP, NOP_SEQWORD },

    /* ═══ LIMB c4 = d0*a4 + d1*a3 + a2*a2 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, R14),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R13, R11),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, R11),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R12, R12),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R12),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(RAX, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ FINAL REDUCTION (7 triads, MUL×19 immediate) ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOP, NOP, NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      NOP, NOP, NOP_SEQWORD },
    { MUL_DSZ64_DIR(TMP1, 19, TMP0),
      NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(RDI, RDI, TMP0),
      NOP, NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, RDI, 51),
      NOP, NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP1, RDI, 13),
      ADD_DSZ64_DRR(R9, R9, TMP0),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(RDI, TMP1, 13),
      NOP, NOP, NOP_SEQWORD },


    /* ═══ EPILOGUE: store results to [TMP11] (5 triads) ═══ */
    { STAD_DSZ64_ASZ32_SC1_RR(RDI, TMP11, SEG),
      NOP, NOP, NOP_SEQWORD },
    { STAD_DSZ64_ASZ32_SC1_RRI(R9,  TMP11, 8,  SEG),
      NOP, NOP, NOP_SEQWORD },
    { STAD_DSZ64_ASZ32_SC1_RRI(R10, TMP11, 16, SEG),
      NOP, NOP, NOP_SEQWORD },
    { STAD_DSZ64_ASZ32_SC1_RRI(RBX, TMP11, 24, SEG),
      NOP, NOP, NOP_SEQWORD },
    { STAD_DSZ64_ASZ32_SC1_RRI(RAX, TMP11, 32, SEG),
      NOP, NOP, END_SEQWORD },

    }; /* end patch */

    g_sq_mem_patch_len = ARRAY_SZ(patch);
    memcpy(g_sq_mem_patch, patch, sizeof(patch));
    printf("fe_sq_mem patch: %d triads\n", g_sq_mem_patch_len);
}

/* Install LDZX/STAD variant at U7c00 */
static void install_mem_patches(void) {
    build_sq_mem_patch();
    patch_ucode(0x7c00, g_sq_mem_patch, g_sq_mem_patch_len);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("  Hooked vmwrite (0x0cd8) -> U7c00 [LDZX/STAD, %d triads]\n",
           g_sq_mem_patch_len);
}

static void install_patches(void) {
    build_sq_patch();
    patch_ucode(0x7c00, g_sq_patch, g_sq_patch_len);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("  Hooked vmwrite (0x0cd8) -> U7c00 [register I/O, %d triads]\n",
           g_sq_patch_len);
}

/* ══════════════════════════════════════════════════════════════════
 *  PROBE — find max triad capacity at U7c00 before installing full patch
 *
 *  Installs progressively larger test chains and verifies each one
 *  returns correctly.  Same approach as multi_capacity_test.c but
 *  targeted at U7c00.
 * ══════════════════════════════════════════════════════════════════ */
static inline uint64_t do_vmwrite_probe(uint64_t val) {
    uint64_t result;
    asm volatile(
        "mov rcx, %[v]\n\t"
        "vmwrite rcx, rcx\n\t"
        : "=a"(result)
        : [v] "r"(val)
        : "rbx", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory"
    );
    return result;
}

static int probe_capacity(uint64_t start_addr, int max_triads) {
    int last_pass = 0;

    printf("  Probing U%04" PRIx64 " capacity (max %d triads)...\n",
           start_addr, max_triads);
    fflush(stdout);

    for (int len = 1; len <= max_triads; len++) {
        init_match_and_patch();
        do_fix_IN_patch();

        /* Build a chain: set RAX = len, then END */
        ucode_t *patch = (ucode_t *)calloc(len, sizeof(ucode_t));
        if (!patch) { perror("calloc"); return last_pass; }

        for (int i = 0; i < len - 1; i++) {
            patch[i].uop0 = NOP;
            patch[i].uop1 = NOP;
            patch[i].uop2 = NOP;
            patch[i].seqw = NOP_SEQWORD;
        }
        /* Last triad: set RAX = len, END */
        patch[len-1].uop0 = ZEROEXT_DSZ32_DI(RAX, len & 0xFF);
        patch[len-1].uop1 = NOP;
        patch[len-1].uop2 = NOP;
        patch[len-1].seqw = END_SEQWORD;

        patch_ucode(start_addr, patch, len);
        hook_match_and_patch(0, 0x0cd8, start_addr);
        free(patch);

        uint64_t result = do_vmwrite_probe(0);

        if (result == (uint64_t)(len & 0xFF)) {
            last_pass = len;
        } else {
            printf("  Capacity limit: %d triads (len=%d returned %" PRIu64
                   ", expected %d)\n", last_pass, len, result, len & 0xFF);
            break;
        }

        /* Print progress every 10 */
        if (len % 10 == 0) {
            printf("    %d triads OK\n", len);
            fflush(stdout);
        }
    }

    if (last_pass == max_triads)
        printf("  All %d triads OK\n", max_triads);

    /* Clean up */
    init_match_and_patch();
    do_fix_IN_patch();
    return last_pass;
}

/* ══════════════════════════════════════════════════════════════════
 *  BISECT — find which triad in the real fe_sq patch causes hang
 *
 *  Installs the first N triads of sq_patch with the Nth triad's
 *  seqword forced to END_SEQWORD.  If N triads work, the hang is
 *  in triad N+1 or later.
 * ══════════════════════════════════════════════════════════════════ */

/* g_sq_patch / g_sq_patch_len are defined in build_sq_patch() above */

static int bisect_patch(void) {
    build_sq_patch();
    int total = g_sq_patch_len;

    printf("  Bisecting fe_sq patch (%d triads) at U7c00...\n", total);
    fflush(stdout);

    int last_pass = 0;

    for (int len = 1; len <= total; len++) {
        init_match_and_patch();
        do_fix_IN_patch();

        /* Copy first `len` triads, force last seqword to END */
        ucode_t *test = (ucode_t *)malloc(len * sizeof(ucode_t));
        if (!test) { perror("malloc"); return last_pass; }
        memcpy(test, g_sq_patch, len * sizeof(ucode_t));
        test[len-1].seqw = END_SEQWORD;
        test[len-1].uop2 = NOP;  /* UEND0(2) targets slot 2 — must be NOP */

        patch_ucode(0x7c00, test, len);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
        free(test);

        printf("    T%02d: ", len - 1);
        fflush(stdout);

        uint64_t result = do_vmwrite_probe(0);
        (void)result;

        printf("OK (RAX=0x%" PRIx64 ")\n", result);
        fflush(stdout);
        last_pass = len;
    }

    printf("  All %d triads pass individually!\n", total);
    init_match_and_patch();
    do_fix_IN_patch();
    return last_pass;
}

/* ══════════════════════════════════════════════════════════════════
 *  SMOKE TEST — verifies hook is alive with the fe_sq patch
 * ══════════════════════════════════════════════════════════════════ */
static int smoke_test_vmwrite(void) {
    uint64_t a[5] = {1, 0, 0, 0, 0};
    uint64_t out[5];
    memcpy(out, a, sizeof(out));
    fe_sq(out, out);

    uint64_t ref[5];
    fe_sq_ref(a, ref);

    if (memcmp(out, ref, sizeof(ref)) == 0) {
        printf("  Smoke test: PASS  (fe_sq({1,0,0,0,0}) matches ref)\n");
        return 1;
    } else {
        printf("  Smoke test: FAIL\n");
        for (int i = 0; i < 5; i++)
            printf("    [%d] ref=%016" PRIx64 " got=%016" PRIx64 " %s\n",
                   i, ref[i], out[i], ref[i]==out[i] ? "" : "***");
        return 0;
    }
}


/* ══════════════════════════════════════════════════════════════════
 *  FIELD ARITHMETIC — microcode-accelerated fe_sq
 * ══════════════════════════════════════════════════════════════════ */

/* --- LDZX/STAD variant: 1 x86 instruction, all work in microcode ---
 *
 * ASZ32 truncates addresses to 32 bits.  Stack on Linux x86-64 is
 * at ~0x7fff... which doesn't fit.  So we use a STATIC scratch buffer
 * guaranteed < 4GB, and copy in/out around each call.
 */
static uint64_t fe_sq_mem_buf[5] __attribute__((aligned(64)));
static uint64_t *fe_sq_mem_ptr_storage = fe_sq_mem_buf;

static void fe_sq_mem(uint64_t *h) {
    /* Copy to static buffer, run microcode, copy back */
    for (int i = 0; i < 5; i++) fe_sq_mem_buf[i] = h[i];
    asm volatile(
        "mov rcx, [%[p]]\n\t"
        "vmwrite rcx, rcx\n\t"
        :
        : [p] "r"(&fe_sq_mem_ptr_storage)
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory"
    );
    for (int i = 0; i < 5; i++) h[i] = fe_sq_mem_buf[i];
}

/* --- Register I/O variant (original) --- */
static struct { uint64_t a; uint64_t out; } fe_sq_args;

static void fe_sq(const uint64_t *a, uint64_t *out) {
    fe_sq_args.a   = (uint64_t)a;
    fe_sq_args.out = (uint64_t)out;

    asm volatile(
        /* Load both pointers from known static address */
        "mov rax, [%[args]]\n\t"       /* rax = a_ptr */
        "mov rcx, [%[args]+8]\n\t"     /* rcx = out_ptr */

        "sub rsp, 56\n\t"
        "mov [rsp+40], rcx\n\t"        /* save out_ptr */

        /* Copy a[] to stack (handles a == out aliasing) */
        "mov rcx, [rax]\n\t"     "mov [rsp],    rcx\n\t"
        "mov rcx, [rax+8]\n\t"   "mov [rsp+8],  rcx\n\t"
        "mov rcx, [rax+16]\n\t"  "mov [rsp+16], rcx\n\t"
        "mov rcx, [rax+24]\n\t"  "mov [rsp+24], rcx\n\t"
        "mov rcx, [rax+32]\n\t"  "mov [rsp+32], rcx\n\t"

        /* Load base values */
        "mov rdi, [rsp]\n\t"
        "mov rsi, [rsp+8]\n\t"
        "mov r12, [rsp+16]\n\t"
        "mov r11, [rsp+24]\n\t"
        "mov r14, [rsp+32]\n\t"

        /* Precompute doubled values */
        "lea r15, [rdi+rdi]\n\t"
        "lea r13, [rsi+rsi]\n\t"
        "lea r9,  [r12+r12]\n\t"
        "lea r10, [r11+r11]\n\t"

        /* Precompute reduced values */
        "imul rbx, r14, 19\n\t"
        "imul rdx, r11, 19\n\t"

        /* Accumulator starts at 0 */
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* Single vmwrite: entire fe_sq in microcode */
        "vmwrite rcx, rdx\n\t"

        /* Read results from registers, store to out[] */
        "mov rcx, [rsp+40]\n\t"
        "mov [rcx],    rdi\n\t"
        "mov [rcx+8],  r9\n\t"
        "mov [rcx+16], r10\n\t"
        "mov [rcx+24], rbx\n\t"
        "mov [rcx+32], rax\n\t"

        "add rsp, 56\n\t"
        :
        : [args] "r"(&fe_sq_args)
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory"
    );
}

/* ══════════════════════════════════════════════════════════════════
 *  FIELD ARITHMETIC — native C
 * ══════════════════════════════════════════════════════════════════ */
static void fe_sq_ref(const uint64_t *a, uint64_t *out) {
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

static void fe_mul_ref(uint64_t *h, const uint64_t *f, const uint64_t *g) {
    uint64_t f0=f[0], f1=f[1], f2=f[2], f3=f[3], f4=f[4];
    uint64_t g0=g[0], g1=g[1], g2=g[2], g3=g[3], g4=g[4];
    uint64_t g1_19=19*g1, g2_19=19*g2, g3_19=19*g3, g4_19=19*g4;

    __uint128_t c0 = (__uint128_t)f0*g0 + (__uint128_t)f1*g4_19
                   + (__uint128_t)f2*g3_19 + (__uint128_t)f3*g2_19
                   + (__uint128_t)f4*g1_19;
    __uint128_t c1 = (__uint128_t)f0*g1 + (__uint128_t)f1*g0
                   + (__uint128_t)f2*g4_19 + (__uint128_t)f3*g3_19
                   + (__uint128_t)f4*g2_19;
    __uint128_t c2 = (__uint128_t)f0*g2 + (__uint128_t)f1*g1
                   + (__uint128_t)f2*g0 + (__uint128_t)f3*g4_19
                   + (__uint128_t)f4*g3_19;
    __uint128_t c3 = (__uint128_t)f0*g3 + (__uint128_t)f1*g2
                   + (__uint128_t)f2*g1 + (__uint128_t)f3*g0
                   + (__uint128_t)f4*g4_19;
    __uint128_t c4 = (__uint128_t)f0*g4 + (__uint128_t)f1*g3
                   + (__uint128_t)f2*g2 + (__uint128_t)f3*g1
                   + (__uint128_t)f4*g0;

    uint64_t carry;
    carry = (uint64_t)(c0>>51); h[0] = (uint64_t)c0 & MASK51;
    c1 += carry;
    carry = (uint64_t)(c1>>51); h[1] = (uint64_t)c1 & MASK51;
    c2 += carry;
    carry = (uint64_t)(c2>>51); h[2] = (uint64_t)c2 & MASK51;
    c3 += carry;
    carry = (uint64_t)(c3>>51); h[3] = (uint64_t)c3 & MASK51;
    c4 += carry;
    carry = (uint64_t)(c4>>51); h[4] = (uint64_t)c4 & MASK51;
    h[0] += carry * 19;
    carry = h[0] >> 51; h[0] &= MASK51;
    h[1] += carry;
}

static inline void fe_add(uint64_t *h, const uint64_t *f, const uint64_t *g) {
    for (int i = 0; i < 5; i++) h[i] = f[i] + g[i];
}

static inline void fe_sub(uint64_t *h, const uint64_t *f, const uint64_t *g) {
    /* Add 4p to avoid underflow.  2p is NOT enough: after fe_add, limbs
     * can reach ~2^52, but 2p[0]=2^52-38 < 2^52, causing underflow.
     * 4p[0]=2^53-76 >> 2^52, safe for all ladder inputs. */
    h[0] = f[0] + 0x1FFFFFFFFFFFB4ULL - g[0];  /* 4*(2^51-19) = 2^53-76 */
    h[1] = f[1] + 0x1FFFFFFFFFFFFCULL - g[1];  /* 4*(2^51-1)  = 2^53-4  */
    h[2] = f[2] + 0x1FFFFFFFFFFFFCULL - g[2];
    h[3] = f[3] + 0x1FFFFFFFFFFFFCULL - g[3];
    h[4] = f[4] + 0x1FFFFFFFFFFFFCULL - g[4];
}

static inline void fe_copy(uint64_t *h, const uint64_t *f) {
    for (int i = 0; i < 5; i++) h[i] = f[i];
}

static inline void fe_cswap(uint64_t *f, uint64_t *g, uint64_t b) {
    uint64_t mask = -(uint64_t)(b & 1);
    for (int i = 0; i < 5; i++) {
        uint64_t x = mask & (f[i] ^ g[i]);
        f[i] ^= x;
        g[i] ^= x;
    }
}

static void fe_reduce(uint64_t *h) {
    uint64_t carry;
    for (int i = 0; i < 4; i++) {
        carry = h[i] >> 51; h[i] &= MASK51;
        h[i+1] += carry;
    }
    carry = h[4] >> 51; h[4] &= MASK51;
    h[0] += carry * 19;
    carry = h[0] >> 51; h[0] &= MASK51;
    h[1] += carry;
}

static void fe_mul121665(uint64_t *h, const uint64_t *f) {
    __uint128_t c;
    uint64_t carry;
    c = (__uint128_t)f[0] * 121665; h[0] = (uint64_t)c & MASK51; carry = (uint64_t)(c >> 51);
    c = (__uint128_t)f[1] * 121665 + carry; h[1] = (uint64_t)c & MASK51; carry = (uint64_t)(c >> 51);
    c = (__uint128_t)f[2] * 121665 + carry; h[2] = (uint64_t)c & MASK51; carry = (uint64_t)(c >> 51);
    c = (__uint128_t)f[3] * 121665 + carry; h[3] = (uint64_t)c & MASK51; carry = (uint64_t)(c >> 51);
    c = (__uint128_t)f[4] * 121665 + carry; h[4] = (uint64_t)c & MASK51; carry = (uint64_t)(c >> 51);
    h[0] += carry * 19;
}

/* ══════════════════════════════════════════════════════════════════
 *  ENCODING
 * ══════════════════════════════════════════════════════════════════ */
static void fe_frombytes(uint64_t *h, const uint8_t *s) {
    /* Load 32 bytes as 4 × 64-bit little-endian words */
    uint64_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
    for (int i = 0; i < 8; i++) w0 |= (uint64_t)s[i]    << (8*i);
    for (int i = 0; i < 8; i++) w1 |= (uint64_t)s[8+i]  << (8*i);
    for (int i = 0; i < 8; i++) w2 |= (uint64_t)s[16+i] << (8*i);
    for (int i = 0; i < 8; i++) w3 |= (uint64_t)s[24+i] << (8*i);

    /* Unpack 256 bits into 5 × 51-bit limbs:
     *   h[0] = bits   0..50   (from w0)
     *   h[1] = bits  51..101  (from w0[51..63] + w1[0..37])
     *   h[2] = bits 102..152  (from w1[38..63] + w2[0..24])
     *   h[3] = bits 153..203  (from w2[25..63] + w3[0..11])
     *   h[4] = bits 204..254  (from w3[12..62])
     */
    h[0] = w0 & MASK51;
    h[1] = ((w0 >> 51) | (w1 << 13)) & MASK51;
    h[2] = ((w1 >> 38) | (w2 << 26)) & MASK51;
    h[3] = ((w2 >> 25) | (w3 << 39)) & MASK51;
    h[4] = (w3 >> 12) & MASK51;
}

static void fe_tobytes(uint8_t *s, const uint64_t *h) {
    uint64_t t[5];
    fe_copy(t, h);
    fe_reduce(t);
    fe_reduce(t);

    /* Fully reduce mod p */
    uint64_t m = (t[0] >= 0x7FFFFFFFFFFED) ? 1 : 0;
    for (int i = 1; i < 4; i++)
        m &= (t[i] == MASK51) ? 1 : 0;
    m &= (t[4] >= MASK51) ? 1 : 0;

    t[0] -= m * 0x7FFFFFFFFFFED;
    for (int i = 1; i < 5; i++) t[i] -= m * MASK51;

    /* Pack 51-bit limbs into 32 bytes */
    uint64_t bits = t[0] | (t[1] << 51);
    for (int i = 0; i < 8; i++) s[i] = (bits >> (8*i)) & 0xFF;
    bits = (t[1] >> 13) | (t[2] << 38);
    for (int i = 0; i < 8; i++) s[8+i] = (bits >> (8*i)) & 0xFF;
    bits = (t[2] >> 26) | (t[3] << 25);
    for (int i = 0; i < 8; i++) s[16+i] = (bits >> (8*i)) & 0xFF;
    bits = (t[3] >> 39) | (t[4] << 12);
    for (int i = 0; i < 8; i++) s[24+i] = (bits >> (8*i)) & 0xFF;
}


/* ══════════════════════════════════════════════════════════════════
 *  INVERSION via Fermat's little theorem: z^(p-2) mod p
 *  Addition chain for p-2 = 2^255 - 21
 * ══════════════════════════════════════════════════════════════════ */
static void fe_invert_impl(uint64_t *out, const uint64_t *z,
                           void (*sq_fn)(const uint64_t*, uint64_t*)) {
    uint64_t z2[5], z9[5], z11[5];
    uint64_t z_5_0[5], z_10_0[5], z_20_0[5], z_40_0[5];
    uint64_t z_50_0[5], z_100_0[5], t[5];

    /* z^2 */
    sq_fn(z, z2);

    /* z^9 = z^8 * z = ((z^2)^2)^2 * z */
    sq_fn(z2, t);          /* z^4 */
    sq_fn(t, t);           /* z^8 */
    fe_mul_ref(z9, t, z);  /* z^9 */

    /* z^11 = z^9 * z^2 */
    fe_mul_ref(z11, z9, z2);

    /* z^(2^5-1) = z^31 = (z^11)^2 * z^9 */
    sq_fn(z11, t);
    fe_mul_ref(z_5_0, t, z9);

    /* z^(2^10-1) */
    sq_fn(z_5_0, t);
    for (int i = 0; i < 4; i++) sq_fn(t, t);
    fe_mul_ref(z_10_0, t, z_5_0);

    /* z^(2^20-1) */
    sq_fn(z_10_0, t);
    for (int i = 0; i < 9; i++) sq_fn(t, t);
    fe_mul_ref(z_20_0, t, z_10_0);

    /* z^(2^40-1) */
    sq_fn(z_20_0, t);
    for (int i = 0; i < 19; i++) sq_fn(t, t);
    fe_mul_ref(z_40_0, t, z_20_0);

    /* z^(2^50-1) */
    sq_fn(z_40_0, t);
    for (int i = 0; i < 9; i++) sq_fn(t, t);
    fe_mul_ref(z_50_0, t, z_10_0);

    /* z^(2^100-1) */
    sq_fn(z_50_0, t);
    for (int i = 0; i < 49; i++) sq_fn(t, t);
    fe_mul_ref(z_100_0, t, z_50_0);

    /* z^(2^200-1) */
    sq_fn(z_100_0, t);
    for (int i = 0; i < 99; i++) sq_fn(t, t);
    fe_mul_ref(t, t, z_100_0);

    /* z^(2^250-1) */
    for (int i = 0; i < 50; i++) sq_fn(t, t);
    fe_mul_ref(t, t, z_50_0);

    /* z^(2^255-21) = z^(p-2)
     * z^(2^250-1) squared 5 times = z^(2^255-32)
     * * z^11 = z^(2^255-21) ✓ */
    for (int i = 0; i < 5; i++) sq_fn(t, t);
    fe_mul_ref(out, t, z11);
}

static void fe_invert(uint64_t *out, const uint64_t *z) {
    fe_invert_impl(out, z, fe_sq);
}

static void fe_invert_ref(uint64_t *out, const uint64_t *z) {
    fe_invert_impl(out, z, fe_sq_ref);
}


/* ══════════════════════════════════════════════════════════════════
 *  MONTGOMERY LADDER
 * ══════════════════════════════════════════════════════════════════ */
static void x25519_impl(uint8_t out[32], const uint8_t scalar[32],
                         const uint8_t point[32],
                         void (*sq_fn)(const uint64_t*, uint64_t*),
                         void (*inv_fn)(uint64_t*, const uint64_t*)) {
    uint8_t e[32];
    memcpy(e, scalar, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    uint64_t u[5];
    fe_frombytes(u, point);

    uint64_t x2[5] = {1,0,0,0,0};   /* x2 = 1 */
    uint64_t z2[5] = {0,0,0,0,0};   /* z2 = 0 */
    uint64_t x3[5], z3[5] = {1,0,0,0,0};
    fe_copy(x3, u);                   /* x3 = u, z3 = 1 */

    uint64_t swap = 0;
    uint64_t A[5], B[5], C[5], D[5], AA[5], BB[5], E[5], DA[5], CB[5], t[5];

    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (e[pos / 8] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(A, x2, z2);
        sq_fn(A, AA);
        fe_sub(B, x2, z2);
        sq_fn(B, BB);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul_ref(DA, D, A);
        fe_mul_ref(CB, C, B);

        fe_add(t, DA, CB);
        sq_fn(t, x3);

        fe_sub(t, DA, CB);
        sq_fn(t, t);
        fe_mul_ref(z3, t, u);

        fe_mul_ref(x2, AA, BB);
        fe_mul121665(t, E);
        fe_add(t, t, AA);
        fe_mul_ref(z2, E, t);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    uint64_t recip[5];
    inv_fn(recip, z2);
    fe_mul_ref(x2, x2, recip);
    fe_reduce(x2);
    fe_reduce(x2);
    fe_tobytes(out, x2);
}

static void x25519_ucode(uint8_t out[32], const uint8_t scalar[32],
                          const uint8_t point[32]) {
    x25519_impl(out, scalar, point, fe_sq, fe_invert);
}

/* ── LDZX/STAD X25519 variant ────────────────────────────────────
 * All field element arrays are STATIC so addresses fit ASZ32.
 * Uses fe_sq_mem (in-place) for squarings.
 * fe_sq_mem_2(a, out): copy a→static buf, sq in-place, copy→out.
 */
static uint64_t _sq2_buf[5] __attribute__((aligned(64)));

static void fe_sq_mem_2(const uint64_t *a, uint64_t *out) {
    for (int i = 0; i < 5; i++) _sq2_buf[i] = a[i];
    fe_sq_mem(_sq2_buf);
    for (int i = 0; i < 5; i++) out[i] = _sq2_buf[i];
}

static void fe_invert_mem(uint64_t *out, const uint64_t *z) {
    fe_invert_impl(out, z, fe_sq_mem_2);
}

/* Full X25519 with LDZX/STAD fe_sq.
 * Static arrays for all intermediates (ASZ32 safe). */
static void x25519_ucode_mem(uint8_t out[32], const uint8_t scalar[32],
                               const uint8_t point[32]) {
    /* Static workspace — all < 4GB */
    static uint64_t u[5];
    static uint64_t x2[5], z2[5], x3[5], z3[5];
    static uint64_t A[5], B[5], C[5], D[5], AA[5], BB[5], E[5], DA[5], CB[5], t[5];

    uint8_t e[32];
    memcpy(e, scalar, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    fe_frombytes(u, point);

    x2[0]=1; x2[1]=x2[2]=x2[3]=x2[4]=0;
    z2[0]=z2[1]=z2[2]=z2[3]=z2[4]=0;
    fe_copy(x3, u);
    z3[0]=1; z3[1]=z3[2]=z3[3]=z3[4]=0;

    uint64_t swap = 0;

    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (e[pos / 8] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(A, x2, z2);
        fe_copy(AA, A); fe_sq_mem(AA);
        fe_sub(B, x2, z2);
        fe_copy(BB, B); fe_sq_mem(BB);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3);
        fe_sub(D, x3, z3);
        fe_mul_ref(DA, D, A);
        fe_mul_ref(CB, C, B);

        fe_add(t, DA, CB);
        fe_copy(x3, t); fe_sq_mem(x3);

        fe_sub(t, DA, CB);
        fe_sq_mem(t);
        fe_mul_ref(z3, t, u);

        fe_mul_ref(x2, AA, BB);
        fe_mul121665(t, E);
        fe_add(t, t, AA);
        fe_mul_ref(z2, E, t);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    static uint64_t recip[5];
    fe_invert_mem(recip, z2);
    fe_mul_ref(x2, x2, recip);
    fe_reduce(x2);
    fe_reduce(x2);
    fe_tobytes(out, x2);
}

static void x25519_native(uint8_t out[32], const uint8_t scalar[32],
                           const uint8_t point[32]) {
    x25519_impl(out, scalar, point, fe_sq_ref, fe_invert_ref);
}


/* ══════════════════════════════════════════════════════════════════
 *  TIMING
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

#define BATCH_SQ 1000
#define REPS_SQ  100
#define REPS_X25519 20

int main(void) {
    uint64_t t0, t1, min, sum;
    int phase_ok;

    printf("=== Curve25519 X25519: microcode vs native ===\n\n");

    install_crash_handlers();
    assign_to_core(0);

    /* ── Step 0: clean M&P reset — crash+hang protected ────────── */
    printf("--- Init ---\n");
    fflush(stdout);
    SAFE_BEGIN("init_match_and_patch") {
        watchdog_arm(5);
        init_match_and_patch();
        do_fix_IN_patch();
        watchdog_disarm();
        printf("  M&P reset: OK\n");
    } SAFE_END;
    if (got_signal) {
        fprintf(stderr, "FATAL: init_match_and_patch %s (signal %d). NUC may need reboot.\n",
                got_signal == SIGALRM ? "HUNG" : "CRASHED", (int)got_signal);
        return 1;
    }

    /* ── Step 1: probe capacity at U7c00 ────────────────────── */
    printf("\n--- Capacity probe at U7c00 ---\n");
    fflush(stdout);
    int capacity = probe_capacity(0x7c00, 70);
    printf("  Result: %d triads usable at U7c00\n\n", capacity);
    fflush(stdout);

    if (capacity < 59) {
        fprintf(stderr, "FATAL: need 59 triads for fe_sq, only %d available.\n", capacity);
        fprintf(stderr, "Cannot proceed with monolithic patch.\n");
        return 1;
    }

    /* ── Step 1b: bisect the REAL patch to find which triad hangs ── */
    printf("--- Bisecting real fe_sq patch ---\n");
    fflush(stdout);
    {
        int bisect_result = bisect_patch();
        printf("  Bisect result: %d/%d triads pass\n\n", bisect_result, g_sq_patch_len);
        fflush(stdout);
        if (bisect_result < g_sq_patch_len) {
            fprintf(stderr, "FATAL: fe_sq patch hangs at triad %d.\n", bisect_result);
            return 1;
        }
    }

    /* ── Step 2: install fe_sq patch ─────────────────────────── */
    phase_ok = 0;
    SAFE_BEGIN("install fe_sq patch") {
        watchdog_arm(5);
        init_match_and_patch();
        do_fix_IN_patch();
        install_patches();
        watchdog_disarm();
        phase_ok = 1;
    } SAFE_END;
    if (!phase_ok) {
        fprintf(stderr, "\nPatch install %s — aborting.\n",
                got_signal == SIGALRM ? "HUNG" : "CRASHED");
        SAFE_BEGIN("recovery reset") {
            watchdog_arm(5);
            init_match_and_patch();
            do_fix_IN_patch();
            watchdog_disarm();
        } SAFE_END;
        return 1;
    }

    /* ── Step 3: smoke test — abort early if hook is broken ──── */
    printf("\n--- Smoke test ---\n");
    fflush(stdout);
    phase_ok = 0;
    SAFE_BEGIN("vmwrite smoke test") {
        watchdog_arm(5);
        phase_ok = smoke_test_vmwrite();
        watchdog_disarm();
    } SAFE_END;
    if (!phase_ok) {
        fprintf(stderr, "\nSmoke test %s — aborting.\n",
                got_signal == SIGALRM ? "HUNG" :
                got_signal ? "CRASHED" : "FAILED");
        SAFE_BEGIN("recovery reset") {
            watchdog_arm(5);
            init_match_and_patch();
            do_fix_IN_patch();
            watchdog_disarm();
        } SAFE_END;
        return 1;
    }

    /* ── fe_sq correctness ───────────────────────────────────── */
    printf("\n--- fe_sq correctness ---\n");
    fflush(stdout);
    phase_ok = 0;
    SAFE_BEGIN("fe_sq correctness") {
        watchdog_arm(10);
        uint64_t state[5] = { 0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                              0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                              0x000216936D3CD6E5ULL };
        uint64_t ref[5], ucd[5];
        fe_sq_ref(state, ref);
        memcpy(ucd, state, sizeof(ucd));
        fe_sq(ucd, ucd);
        int ok = memcmp(ref, ucd, sizeof(ref)) == 0;
        printf("  Single:  %s\n", ok ? "PASS" : "FAIL");
        if (!ok) {
            for (int i = 0; i < 5; i++)
                printf("    [%d] ref=%016" PRIx64 " ucd=%016" PRIx64 " %s\n",
                       i, ref[i], ucd[i], ref[i]==ucd[i] ? "" : "***");
        }

        if (ok) {
            /* Iterated */
            uint64_t ri[5], ui[5];
            memcpy(ri, state, sizeof(ri));
            memcpy(ui, state, sizeof(ui));
            for (int i = 0; i < 1000; i++) {
                fe_sq_ref(ri, ri);
                fe_sq(ui, ui);
            }
            ok = memcmp(ri, ui, sizeof(ri)) == 0;
            printf("  1000 sq: %s\n", ok ? "PASS" : "FAIL");
        }
        watchdog_disarm();
        phase_ok = ok;
    } SAFE_END;
    if (!phase_ok) {
        fprintf(stderr, "\nfe_sq correctness %s — skipping microcode benchmark.\n",
                got_signal == SIGALRM ? "HUNG" :
                got_signal ? "CRASHED" : "FAILED");
        fprintf(stderr, "Will still run native-only X25519 for reference.\n");
        /* Re-install patches in case crash corrupted state */
        install_patches();
    }

    /* ── X25519 correctness (RFC 7748 test vector) ───────────── */
    printf("\n--- X25519 correctness ---\n");

    /* First run native-only to validate our C code independently */
    phase_ok = 0;
    {
        uint8_t scalar[32] = {
            0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d,
            0x3b, 0x16, 0x15, 0x4b, 0x82, 0x46, 0x5e, 0xdd,
            0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18,
            0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4
        };
        uint8_t basepoint[32] = {
            0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb,
            0x35, 0x94, 0xc1, 0xa4, 0x24, 0xb1, 0x5f, 0x7c,
            0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b,
            0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c
        };
        uint8_t expected[32] = {
            0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90,
            0x8e, 0x94, 0xea, 0x4d, 0xf2, 0x8d, 0x08, 0x4f,
            0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c, 0x71, 0xf7,
            0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52
        };

        uint8_t out_ref[32];
        x25519_native(out_ref, scalar, basepoint);
        int ok_ref = memcmp(out_ref, expected, 32) == 0;
        printf("  Native:    %s\n", ok_ref ? "PASS" : "FAIL");
        if (!ok_ref) {
            printf("  Native result: ");
            for (int i = 0; i < 32; i++) printf("%02x", out_ref[i]);
            printf("\n  Expected:      ");
            for (int i = 0; i < 32; i++) printf("%02x", expected[i]);
            printf("\n");
            fprintf(stderr, "Native X25519 FAILED — C code bug, aborting.\n");
            return 1;
        }

        /* Now try microcode-assisted X25519 with crash protection */
        SAFE_BEGIN("X25519 microcode correctness") {
            watchdog_arm(30);
            uint8_t out_ucd[32];
            x25519_ucode(out_ucd, scalar, basepoint);
            watchdog_disarm();
            int ok_ucd = memcmp(out_ucd, expected, 32) == 0;
            printf("  Microcode: %s\n", ok_ucd ? "PASS" : "FAIL");
            if (!ok_ucd) {
                printf("  Microcode result: ");
                for (int i = 0; i < 32; i++) printf("%02x", out_ucd[i]);
                printf("\n  Expected:         ");
                for (int i = 0; i < 32; i++) printf("%02x", expected[i]);
                printf("\n");
            }
            phase_ok = ok_ucd;
        } SAFE_END;

        if (!phase_ok) {
            fprintf(stderr, "\nMicrocode X25519 FAILED or CRASHED.\n");
            fprintf(stderr, "Will run native-only benchmark for reference.\n");
            install_patches();  /* reinstall in case crash corrupted */
        }
    }

    /* ── fe_sq benchmark ─────────────────────────────────────── */
    printf("\n--- fe_sq: %d ops/batch, %d batches ---\n\n", BATCH_SQ, REPS_SQ);
    {
        uint64_t state[5] = { 0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                              0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                              0x000216936D3CD6E5ULL };
        uint64_t tmp[5];

        /* Native C — always safe */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS_SQ; r++) {
            memcpy(tmp, state, sizeof(tmp));
            t0 = rdtsc_start();
            for (int i = 0; i < BATCH_SQ; i++) fe_sq_ref(tmp, tmp);
            t1 = rdtsc_end();
            uint64_t dt = t1 - t0;
            sum += dt; if (dt < min) min = dt;
        }
        printf("  Native C:      min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
               min/BATCH_SQ, sum/REPS_SQ/BATCH_SQ);

        /* Microcode register-I/O — crash-protected */
        SAFE_BEGIN("fe_sq benchmark (register I/O)") {
            watchdog_arm(30);
            min = UINT64_MAX; sum = 0;
            for (int r = 0; r < REPS_SQ; r++) {
                memcpy(tmp, state, sizeof(tmp));
                t0 = rdtsc_start();
                for (int i = 0; i < BATCH_SQ; i++) fe_sq(tmp, tmp);
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt; if (dt < min) min = dt;
            }
            watchdog_disarm();
            printf("  Reg I/O:       min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
                   min/BATCH_SQ, sum/REPS_SQ/BATCH_SQ);
        } SAFE_END;

        /* Microcode LDZX/STAD — reinstall mem patch, benchmark, restore */
        SAFE_BEGIN("fe_sq benchmark (LDZX/STAD)") {
            watchdog_arm(30);
            init_match_and_patch();
            do_fix_IN_patch();
            install_mem_patches();

            /* Correctness check first */
            uint64_t mref[5], mtest[5];
            memcpy(mtest, state, sizeof(mtest));
            fe_sq_ref(state, mref);
            fe_sq_mem(mtest);
            if (memcmp(mref, mtest, sizeof(mref)) != 0) {
                printf("  LDZX/STAD:     CORRECTNESS FAIL — skipping\n");
                for (int i = 0; i < 5; i++)
                    printf("    [%d] ref=%016" PRIx64 " got=%016" PRIx64 " %s\n",
                           i, mref[i], mtest[i], mref[i]==mtest[i]?"":"***");
            } else {
                min = UINT64_MAX; sum = 0;
                for (int r = 0; r < REPS_SQ; r++) {
                    memcpy(tmp, state, sizeof(tmp));
                    t0 = rdtsc_start();
                    for (int i = 0; i < BATCH_SQ; i++) fe_sq_mem(tmp);
                    t1 = rdtsc_end();
                    uint64_t dt = t1 - t0;
                    sum += dt; if (dt < min) min = dt;
                }
                printf("  LDZX/STAD:     min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
                       min/BATCH_SQ, sum/REPS_SQ/BATCH_SQ);
            }

            /* Restore register-I/O patch for subsequent X25519 tests */
            init_match_and_patch();
            do_fix_IN_patch();
            install_patches();
            watchdog_disarm();
        } SAFE_END;
    }

    /* ── Full X25519 benchmark ───────────────────────────────── */
    printf("\n--- X25519: %d iterations ---\n\n", REPS_X25519);
    {
        uint8_t scalar[32] = {
            0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d,
            0x3b, 0x16, 0x15, 0x4b, 0x82, 0x46, 0x5e, 0xdd,
            0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18,
            0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4
        };
        uint8_t basepoint[32] = {
            0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb,
            0x35, 0x94, 0xc1, 0xa4, 0x24, 0xb1, 0x5f, 0x7c,
            0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b,
            0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c
        };
        uint8_t out[32];

        /* Native — always safe */
        min = UINT64_MAX; sum = 0;
        for (int r = 0; r < REPS_X25519; r++) {
            t0 = rdtsc_start();
            x25519_native(out, scalar, basepoint);
            t1 = rdtsc_end();
            uint64_t dt = t1 - t0;
            sum += dt; if (dt < min) min = dt;
        }
        printf("  Native:    min %8" PRIu64 "  avg %8" PRIu64 " cycles\n",
               min, sum / REPS_X25519);

        /* Microcode reg-I/O — crash-protected */
        SAFE_BEGIN("X25519 benchmark (reg I/O)") {
            watchdog_arm(60);
            min = UINT64_MAX; sum = 0;
            for (int r = 0; r < REPS_X25519; r++) {
                t0 = rdtsc_start();
                x25519_ucode(out, scalar, basepoint);
                t1 = rdtsc_end();
                uint64_t dt = t1 - t0;
                sum += dt; if (dt < min) min = dt;
            }
            watchdog_disarm();
            printf("  Reg I/O:   min %8" PRIu64 "  avg %8" PRIu64 " cycles\n",
                   min, sum / REPS_X25519);
        } SAFE_END;

        /* Microcode LDZX/STAD — install mem patch, correctness, benchmark */
        SAFE_BEGIN("X25519 benchmark (LDZX/STAD)") {
            watchdog_arm(120);
            init_match_and_patch();
            do_fix_IN_patch();
            install_mem_patches();

            /* Quick correctness check */
            uint8_t expected[32] = {
                0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90,
                0x8e, 0x94, 0xea, 0x4d, 0xf2, 0x8d, 0x08, 0x4f,
                0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c, 0x71, 0xf7,
                0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52
            };
            uint8_t test_out[32];
            x25519_ucode_mem(test_out, scalar, basepoint);
            if (memcmp(test_out, expected, 32) != 0) {
                printf("  LDZX/STAD: X25519 CORRECTNESS FAIL\n");
                printf("    got:      ");
                for (int i=0;i<32;i++) printf("%02x",test_out[i]);
                printf("\n");
            } else {
                min = UINT64_MAX; sum = 0;
                for (int r = 0; r < REPS_X25519; r++) {
                    t0 = rdtsc_start();
                    x25519_ucode_mem(out, scalar, basepoint);
                    t1 = rdtsc_end();
                    uint64_t dt = t1 - t0;
                    sum += dt; if (dt < min) min = dt;
                }
                printf("  LDZX/STAD: min %8" PRIu64 "  avg %8" PRIu64 " cycles\n",
                       min, sum / REPS_X25519);
            }
            watchdog_disarm();
        } SAFE_END;
    }

    printf("\n--- Notes ---\n");
    printf("  fe_sq: monolithic microcode (62 triads, 1 vmwrite)\n");
    printf("  fe_mul: native C (__uint128_t) -- shared by both paths\n");
    printf("  Microcode advantage: MUL throughput in ucode pipeline\n");
    printf("  If mono fe_sq beats native fe_sq, total X25519 is faster\n");
    printf("  because inversion alone has ~252 consecutive squarings.\n");

    /* Clean up: leave M&P in a known-good state */
    init_match_and_patch();
    do_fix_IN_patch();
    printf("\n  M&P reset to clean state.\n");

    return 0;
}
