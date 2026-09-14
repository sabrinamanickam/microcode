/* probe_sq_latency.c — does microcode firing latency scale with patch triad
 * count, or is it ~fixed (overhead-bound)?
 *
 * This is the gating experiment for the "sqmul / fewer-firings" ladder redesign.
 * The 5x51 fe_sq patch is 43 triads and fires at ~123 cyc (dependent). If a
 * ~90-triad patch ALSO fires at ~123, the per-firing cost is overhead-bound and
 * a bigger 2-output "sqmul" patch is ~free → packing 2 field ops per firing
 * wins. If a 90-triad patch costs ~2x, latency is triad-bound and the
 * fewer-firings lever is a wash.
 *
 * Method: install ONLY the sq patch (no mul → full 128-triad budget), padded
 * with N benign filler triads (ADD TMP7,TMP7,TMP7 — TMP7 is unused by sq, so
 * the computed square is unchanged). Sweep N, verify each padded patch produces
 * the same square as N=0, and time a dependent FE_SQ chain (same methodology as
 * inline2_profile: 16000 ops, min of 50 trials).
 *
 * Build: make PROG=probe_sq_latency CFLAGS="-O3 -march=native -masm=intel -fwrapv -fPIE -I include/"
 * Run:   sudo taskset -c 0 ./probe_sq_latency_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL

/* The production 5x51 squaring patch (43 triads), verbatim from
 * full_curve25519_inline2.c install_field_patches(). Last triad ends the patch. */
static ucode_t sq_patch[] = {
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
#define SQ_N ((int)ARRAY_SZ(sq_patch))   /* 43 */

/* INDEPENDENT filler triads — model ILP-rich compute, not a serial chain.
 * Each op reads TMP8 (a live carry value the sq computed; read-only here, so
 * preserved for the final reduction) and writes a DEAD TMP (sq uses only
 * TMP0/1/6/8/9/15). No filler reads a value another filler wrote -> no RAW
 * chain. The two variants use disjoint dest sets so alternating them avoids
 * any adjacent write-after-write either. This is the faithful test: do extra
 * triads with normal ILP pipeline (flat), or cost ~1 cyc each (sequencer-bound)?
 *
 * (The earlier ADD TMP7,TMP7,TMP7 filler accidentally formed a RAW chain on
 *  TMP7 and measured the fully-serial worst case — not representative.) */
static const ucode_t FILLERS[2] = {
    { ZEROEXT_DSZ64_DR(TMP2, TMP8), ZEROEXT_DSZ64_DR(TMP3, TMP8),
      ZEROEXT_DSZ64_DR(TMP4, TMP8), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP5, TMP8), ZEROEXT_DSZ64_DR(TMP7, TMP8),
      ZEROEXT_DSZ64_DR(TMP10, TMP8), NOP_SEQWORD },
};

static ucode_t padded[160];

/* APPENDED: all `pad` fillers in a block just before the END triad. */
static int build_appended(int pad) {
    int n = 0;
    for (int i = 0; i < SQ_N - 1; i++) padded[n++] = sq_patch[i];
    for (int i = 0; i < pad; i++)      padded[n++] = FILLERS[i & 1];
    padded[n++] = sq_patch[SQ_N - 1];
    return n;
}

/* INTERLEAVED: the same `pad` fillers spread evenly THROUGH the compute body,
 * so they land in the sq's dependency-stall cycles. If the sequencer can issue
 * them during stalls they hide (cheaper than appended); if it's strictly
 * serial they cost the same as appended. This is the make-or-break test for the
 * interleaved-sqmul idea. */
static int build_interleaved(int pad) {
    int n = 0, placed = 0;
    int gaps = SQ_N - 1;
    for (int i = 0; i < gaps; i++) {
        padded[n++] = sq_patch[i];
        int target = (int)((long)pad * (i + 1) / gaps);   /* cumulative even spread */
        while (placed < target) padded[n++] = FILLERS[placed++ & 1];
    }
    while (placed < pad) padded[n++] = FILLERS[placed++ & 1];
    padded[n++] = sq_patch[SQ_N - 1];
    return n;
}

/* One fe_sq firing, in place on a 5-limb element at [rbp+0..32]. Mirrors
 * inline2's FE_SQ trigger sequence exactly. */
#define SQ_INPLACE \
    "mov r14, [rbp + 32]\n\t" \
    "mov r11, [rbp + 24]\n\t" \
    "mov r12, [rbp + 16]\n\t" \
    "mov rsi, [rbp + 8]\n\t"  \
    "mov rdi, [rbp + 0]\n\t"  \
    "lea r15, [rdi + rdi]\n\t" \
    "lea r13, [rsi + rsi]\n\t" \
    "lea r9,  [r12 + r12]\n\t" \
    "lea r10, [r11 + r11]\n\t" \
    "imul rbx, r14, 19\n\t"    \
    "imul rdx, r11, 19\n\t"    \
    "xor eax, eax\n\t"         \
    "xor r8d, r8d\n\t"         \
    ".byte 0x0f, 0x78, 0xca\n\t" \
    "mov [rbp + 0],  rdi\n\t"  \
    "mov [rbp + 8],  r9\n\t"   \
    "mov [rbp + 16], r10\n\t"  \
    "mov [rbp + 24], rbx\n\t"  \
    "mov [rbp + 32], rax\n\t"

#define CLOBBERS \
    "rax","rbx","rcx","rdx","rsi","rdi", \
    "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"

#define REP2(x)  x x
#define REP4(x)  REP2(x) REP2(x)
#define REP8(x)  REP4(x) REP4(x)
#define REP16(x) REP8(x) REP8(x)

#define UNROLL 16
#define REPS   1000
#define TRIALS 50

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

static void sq_once(uint64_t *v) {
    register uint64_t *p asm("rbp") = v;
    asm volatile(SQ_INPLACE : : "r"(p) : CLOBBERS);
}

static uint64_t time_sq(uint64_t *v) {
    uint64_t best = ~0ULL;
    for (int t = 0; t < TRIALS; t++) {
        register uint64_t *p asm("rbp") = v;
        uint64_t a = rdtsc_start();
        for (int r = 0; r < REPS; r++)
            asm volatile(REP16(SQ_INPLACE) : : "r"(p) : CLOBBERS);
        uint64_t c = rdtsc_end() - a;
        if (c < best) best = c;
    }
    return best;
}

static const uint64_t g_input[5] = {
    0x123456789ABCDULL & MASK51, 0x2468ACE13579BULL & MASK51,
    0x13579BDF02468ULL & MASK51, 0x0FEDCBA987654ULL & MASK51,
    0x1111122222333ULL & MASK51
};
static uint64_t g_ref[5];

/* install the just-built `padded`[0..n), verify result vs g_ref, return cyc/firing */
static double measure(int n, int set_ref, const char **verdict) {
    patch_ucode(0x7c00, padded, n);
    hook_match_and_patch(1, 0x0618, 0x7c00);
    uint64_t buf[5];
    memcpy(buf, g_input, sizeof buf);
    sq_once(buf);
    if (set_ref) { memcpy(g_ref, buf, sizeof g_ref); *verdict = "REF"; }
    else *verdict = (memcmp(buf, g_ref, sizeof g_ref) == 0) ? "OK" : "MISMATCH";
    memcpy(buf, g_input, sizeof buf);
    return (double)time_sq(buf) / ((double)REPS * UNROLL);
}

int main(void) {
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    printf("=== fe_sq filler placement: APPENDED vs INTERLEAVED ===\n");
    printf("(do extra independent triads hide in the sq's stall cycles?)\n\n");

    const int pads[] = {0, 14, 28, 42, 51};
    printf("  %5s %7s | %12s %4s | %12s %4s\n",
           "pad", "triads", "appended", "vf", "interleaved", "vf");
    printf("  %5s %7s + %12s %4s + %12s %4s\n",
           "---", "------", "---------", "--", "-----------", "--");

    /* establish g_ref from pad=0 (appended==interleaved at pad 0) */
    const char *v;
    int n0 = build_appended(0);
    double base = measure(n0, 1, &v);
    printf("  %5d %7d | %12.2f %4s | %12s %4s\n", 0, n0, base, v, "(same)", v);

    for (unsigned k = 1; k < sizeof(pads)/sizeof(pads[0]); k++) {
        int pad = pads[k];
        const char *va, *vi;
        int na = build_appended(pad);    double ap = measure(na, 0, &va);
        int ni = build_interleaved(pad); double il = measure(ni, 0, &vi);
        printf("  %5d %7d | %12.2f %4s | %12.2f %4s\n", pad, na, ap, va, il, vi);
    }

    printf("\nIf INTERLEAVED << APPENDED: triads hide in stalls -> interleaved\n");
    printf("sqmul could reach ~150 -> proceed to author it. If INTERLEAVED ~=\n");
    printf("APPENDED: sequencer is serial -> 207 is the floor -> stop.\n");

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
