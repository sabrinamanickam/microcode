/* probe_carry_isolate.c — does the microcode field-op win come from the
 * CARRY-CHAIN handling (per-TMP SETCC flag domain, triple-packed, independent
 * lanes) rather than from the MUL primitive?
 *
 * Motivation: against amd64-51/asm (unsaturated SUPERCOP) the same-ladder swap
 * to microcode field ops is ~6.5-6.9% (RESULTS.md, "Same-ladder amd64-51").
 * The amd64-51 asm propagates carries with a SERIAL add/adc chain (each adc
 * reads the prior adc's arch RFLAGS — one dependency chain through all limbs).
 * Our microcode instead uses SETCC in the per-TMP flag domain (domain #1, see
 * CLAUDE.md / adc_findings.md): each limb tracks its own carry, so independent
 * limb columns do NOT serialize. This probe asks how much that buys.
 *
 * Method: two patches with IDENTICAL op count and IDENTICAL triad count, the
 * ONLY difference being carry-dependency DEPTH:
 *
 *   SERIAL   — one accumulator threaded through all T carry steps. Triad i+1's
 *              ADD reads triad i's accumulator -> dependency depth = T.
 *              This is the adc-chain shape.
 *   PARALLEL — LANES independent accumulators, T/LANES carry steps each.
 *              Triads in different lanes share no register -> dependency
 *              depth = T/LANES. This is the production field-op shape.
 *
 * Each carry step is the production triple-pack idiom (1 triad, 3 ops):
 *     { ADD(acc, acc, ONES), SETCC(c, acc), ADD(hi, hi, c) }
 * with ONES = 0xFFFF...FF so CF fires every step after the first. Each variant
 * computes a deterministic value we verify (per-variant formula below) — this
 * guards against the patch being silently short-circuited.
 *
 * Read the result:
 *   - If PARALLEL fires materially faster than SERIAL at the same triad count,
 *     the carry-dependency depth is the lever -> the field-op win is the
 *     SETCC-domain carry handling, NOT the MUL.
 *   - If they fire at the same cyc, firing latency is overhead/triad bound and
 *     the internal carry structure is invisible at this granularity (see the
 *     CAVEAT below) -> the 6.9% is unlikely to be carry-chain depth.
 *
 * CAVEAT: probe_sq_latency.c tests whether firing latency is overhead-bound vs
 * triad-bound. RUN THAT FIRST. If firing latency does not respond to internal
 * structure at all, this probe cannot resolve the question and you must measure
 * the carry effect end-to-end (swap only the carry style inside a full fe_mul).
 *
 * Build: make PROG=probe_carry_isolate CFLAGS="-O3 -march=native -masm=intel -fwrapv -fPIE -I include/"
 * Run:   sudo taskset -c 0 ./probe_carry_isolate_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* ---- workload parameters ---------------------------------------------- */
#define T      40          /* total carry steps (== triad count of the body) */
#define LANES   4          /* parallel accumulator lanes (T must divide this) */

/* Per-lane TMP allocation (disjoint across lanes so PARALLEL lanes never share
 * a register). sq/mul never run here — the whole TMP file is ours. */
static const int ACC[LANES] = { TMP0, TMP2, TMP3, TMP4 };
static const int HI [LANES] = { TMP5, TMP7, TMP10, TMP11 };
static const int CY [LANES] = { TMP12, TMP13, TMP14, TMP15 };

/* The trigger leaves R8 = 0xFFFF...FF (the per-step addend, "ONES") and
 * RAX = 0 (zero source for TMP init). The patch writes its 5-limb-shaped
 * output back into RDI..(arch) which the trigger stores to [rbp+0..]. We only
 * use two outputs: acc-sum -> RDI, carry-count -> R9. */

/* Emit a triple-pack carry step for lane L into patch[] at index n; returns
 * new n. RAW slot 0->1 (ADD sets acc flags, SETCC reads them) and slot 1->2
 * (SETCC writes CY, ADD reads CY) are both confirmed (CLAUDE.md). */
static int step(ucode_t *p, int n, int lane) {
    int acc = ACC[lane], hi = HI[lane], cy = CY[lane];
    p[n++] = (ucode_t){ ADD_DSZ64_DRR(acc, acc, R8),
                        SETCC_CONDB_DR(cy, acc),
                        ADD_DSZ64_DRR(hi, hi, cy), NOP_SEQWORD };
    return n;
}

/* zero a TMP from RAX (which the trigger sets to 0) */
static int zero_tmp(ucode_t *p, int n, int t) {
    p[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(t, RAX), NOP, NOP, NOP_SEQWORD };
    return n;
}

static ucode_t patch[256];

/* Build the SERIAL patch: single lane 0, T steps -> dependency depth T.
 * acc0 = 0xFFFF..FF * T (mod 2^64) = 2^64 - T ;  hi0 = T - 1.
 * We park acc0 -> RDI and hi0 -> R9 at the end. */
static int build_serial(void) {
    int n = 0;
    n = zero_tmp(patch, n, ACC[0]);
    n = zero_tmp(patch, n, HI[0]);
    for (int i = 0; i < T; i++) n = step(patch, n, 0);
    /* export: RDI = acc0, R9 = hi0 */
    patch[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(RDI, ACC[0]),
                            ZEROEXT_DSZ64_DR(R9, HI[0]), NOP, END_SEQWORD };
    return n;
}

/* Build the PARALLEL patch: LANES lanes, T/LANES steps each, interleaved so
 * adjacent triads belong to different lanes (no adjacent RAW). Dependency depth
 * = T/LANES. Same total step count T -> same triad count for the body.
 * Per lane j: acc_j = 2^64 - T/LANES, hi_j = T/LANES - 1.
 * Export sum of acc_j -> RDI and sum of hi_j -> R9 so the verify formula is
 * fixed and independent of lane interleave. */
static int build_parallel(void) {
    int n = 0, per = T / LANES;
    for (int j = 0; j < LANES; j++) { n = zero_tmp(patch, n, ACC[j]);
                                      n = zero_tmp(patch, n, HI[j]); }
    /* interleave: round-robin the lanes so no two adjacent triads share a reg */
    for (int s = 0; s < per; s++)
        for (int j = 0; j < LANES; j++) n = step(patch, n, j);
    /* reduce lanes: RDI = sum acc_j, R9 = sum hi_j  (serial tail, LANES long —
     * negligible vs the body, same for both variants if you also tail SERIAL,
     * but SERIAL has nothing to reduce). Use RDI/R9 as the running sums. */
    patch[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(RDI, ACC[0]),
                            ZEROEXT_DSZ64_DR(R9, HI[0]), NOP, NOP_SEQWORD };
    for (int j = 1; j < LANES; j++)
        patch[n++] = (ucode_t){ ADD_DSZ64_DRR(RDI, RDI, ACC[j]),
                                ADD_DSZ64_DRR(R9, R9, HI[j]), NOP,
                                (j == LANES - 1) ? END_SEQWORD : NOP_SEQWORD };
    return n;
}

/* ---- trigger + timing (mirrors probe_sq_latency.c) -------------------- */

/* One firing in place on [rbp+0..]. Sets R8=ONES, RAX=0, fires the patch via
 * the vmread byte (hook installed at 0x0618), stores RDI->[rbp+0], R9->[rbp+8]. */
#define FIRE \
    "mov r8, -1\n\t"               /* 0xFFFFFFFFFFFFFFFF */ \
    "xor eax, eax\n\t"            \
    ".byte 0x0f, 0x78, 0xca\n\t"  /* vmread rdx, rcx -> patched */ \
    "mov [rbp + 0], rdi\n\t"      \
    "mov [rbp + 8], r9\n\t"

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

static void fire_once(uint64_t *v) {
    register uint64_t *p asm("rbp") = v;
    asm volatile(FIRE : : "r"(p) : CLOBBERS);
}

static uint64_t time_fire(uint64_t *v) {
    uint64_t best = ~0ULL;
    for (int t = 0; t < TRIALS; t++) {
        register uint64_t *p asm("rbp") = v;
        uint64_t a = rdtsc_start();
        for (int r = 0; r < REPS; r++)
            asm volatile(REP16(FIRE) : : "r"(p) : CLOBBERS);
        uint64_t c = rdtsc_end() - a;
        if (c < best) best = c;
    }
    return best;
}

static void run(const char *name, int (*build)(void),
                uint64_t exp_acc, uint64_t exp_hi) {
    int n = build();
    patch_ucode(0x7c00, patch, n);
    hook_match_and_patch(1, 0x0618, 0x7c00);

    uint64_t buf[2] = {0, 0};
    fire_once(buf);
    const char *verdict =
        (buf[0] == exp_acc && buf[1] == exp_hi) ? "OK" : "MISMATCH";

    uint64_t best = time_fire(buf);
    double per = (double)best / ((double)REPS * UNROLL);

    printf("  %-9s %6d   %10.2f   acc=%016llx hi=%llu  [%s]\n",
           name, n, per,
           (unsigned long long)buf[0], (unsigned long long)buf[1], verdict);
    if (strcmp(verdict, "MISMATCH") == 0)
        printf("            expected acc=%016llx hi=%llu\n",
               (unsigned long long)exp_acc, (unsigned long long)exp_hi);
}

int main(void) {
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    printf("=== carry-chain isolation: SERIAL (adc-shape) vs PARALLEL (SETCC) ===\n");
    printf("(T=%d carry steps; PARALLEL uses %d lanes -> depth %d vs %d)\n\n",
           T, LANES, T / LANES, T);
    printf("  %-9s %6s   %10s   %s\n", "variant", "triads", "cyc/fire", "result");
    printf("  %-9s %6s   %10s   %s\n", "-------", "------", "--------", "------");

    /* SERIAL: T steps of +ONES into one acc => acc = 2^64 - T, hi = T - 1 */
    run("SERIAL",   build_serial,   (uint64_t)(-(int64_t)T),  (uint64_t)(T - 1));

    /* PARALLEL: LANES lanes x per steps; sum acc = LANES*(2^64 - per) mod 2^64
     * = -(LANES*per) = -T ; sum hi = LANES*(per - 1) = T - LANES */
    run("PARALLEL", build_parallel, (uint64_t)(-(int64_t)T),  (uint64_t)(T - LANES));

    printf("\nPARALLEL << SERIAL  => carry-dependency depth is the lever\n");
    printf("                       (microcode win is SETCC carry handling, not MUL).\n");
    printf("PARALLEL ~= SERIAL  => firing latency is overhead/triad-bound here;\n");
    printf("                       run probe_sq_latency.c first, then measure the\n");
    printf("                       carry effect inside a full fe_mul end-to-end.\n");

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
