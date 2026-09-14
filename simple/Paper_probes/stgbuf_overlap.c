/*
 * stgbuf_overlap.c — Does LDSTGBUF's ~30 cyc latency overlap with
 *                    non-stgbuf compute, or does it serialize?
 *
 * The question that decides whether a microcoded ladderstep with
 * stgbuf-resident state could ever be viable: if 30-cyc loads hide
 * behind arithmetic, careful scheduling could mask the cost. If they
 * fully serialize, stgbuf is dead for hot-path use.
 *
 * Method: same as stgbuf_bench (install patch → fire BATCH × REPS in a
 * tight loop → report min-per-fire).
 *
 * Scenarios (all use INDEPENDENT data — XOR doesn't depend on LD
 * result, so any serialization observed is structural, not RAW):
 *   B0 : empty patch                                  (baseline)
 *   B1 : 5 LDs alone, 5 triads                        (replicate S5)
 *   B2 : 5 XORs alone, 5 triads                       (compute-only baseline)
 *   B3 : 10 triads alternating LD / XOR (independent) (LD-then-other)
 *   B4 : 5 triads, each with [LD slot 0, XOR slot 1]  (intra-triad overlap)
 *   B5 : 5 LDs and 5 XORs packed (LD+XOR+XOR or 3 LDs / 3 XORs)
 *
 * Decision rule:
 *   - if B3 ≈ B1 (LDs hide behind XORs across triads): big win possible
 *   - if B3 ≈ B1 + B2:                                  full serialization
 *   - if B4 ≈ B1:                                      intra-triad overlap
 *   - if B4 ≈ B1 + ~per-triad-XOR-cost:                no intra-triad overlap
 *
 * Build:  make PROG=stgbuf_overlap
 * Run:    sudo taskset -c 0 ./stgbuf_overlap_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define REGION  0x7c00

static inline uint64_t fire_patch(void) {
    uint64_t res;
    asm volatile(
        "xor eax, eax\n\t"
        "vmwrite rcx, rdx\n\t"
        : "=a"(res) :: "rcx", "rdx", "memory", "cc"
    );
    return res;
}

static void reinstall(ucode_t *p, int n) {
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(REGION, p, n);
    hook_match_and_patch(0, 0x0cd8, REGION);
}

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
#define REPS   100

static uint64_t time_scenario(const char *name, ucode_t *p, int n) {
    reinstall(p, n);
    for (int i = 0; i < 100; i++) (void)fire_patch();   /* warm up */

    uint64_t min = UINT64_MAX, sum = 0;
    for (int r = 0; r < REPS; r++) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) (void)fire_patch();
        uint64_t t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt;
        if (dt < min) min = dt;
    }
    uint64_t min_per = min / BATCH;
    uint64_t avg_per = sum / REPS / BATCH;
    printf("  %-52s  min %4" PRIu64 "  avg %4" PRIu64 "  cyc/fire\n",
           name, min_per, avg_per);
    return min_per;
}

#define A0 0xb000
#define A1 0xb040
#define A2 0xb080
#define A3 0xb0c0
#define A4 0xb100

int main(void) {
    printf("=== stgbuf overlap probe ===\n");
    printf("Does LDSTGBUF latency hide behind non-stgbuf compute?\n");
    printf("BATCH = %d  REPS = %d\n\n", BATCH, REPS);

    assign_to_core(0);

    /* ── B0: empty patch ─────────────────────────────────────── */
    ucode_t b0[] = {
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c0 = time_scenario("B0  empty", b0, ARRAY_SZ(b0));

    /* ── Seed stgbuf slots and TMPs we'll use as XOR operands ─── */
    /* Used as a shared prologue inserted into each measured patch. */
    /* TMP6 and TMP7 hold values for XOR; TMP10..14 hold stgbuf
     * destinations for LD. */

    /* ── B1: 5 LDs alone, one per triad (≈ S5) ────────────────── */
    ucode_t b1[] = {
        /* Seed: set TMP0 (the value to store), then write 5 slots */
        { ZEROEXT_DSZ32_DI(TMP0, 0xCAFE), NOP, NOP, NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A0),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A1),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A2), NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A3),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A4), NOP, NOP_SEQWORD },
        /* Measured: 5 LDs, 5 separate triads */
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP10, A0), NOP, NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP11, A1), NOP, NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP12, A2), NOP, NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP13, A3), NOP, NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP14, A4), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c1 = time_scenario("B1  5 LDs alone, 5 triads", b1, ARRAY_SZ(b1));

    /* ── B2: 5 XORs alone, 5 triads (compute-only baseline) ──── */
    ucode_t b2[] = {
        /* Seed XOR operands */
        { ZEROEXT_DSZ32_DI(TMP6, 0x1111), ZEROEXT_DSZ32_DI(TMP7, 0x2222), NOP, NOP_SEQWORD },
        /* Measured: 5 XORs, 5 separate triads. Use TMP6 and TMP7 as inputs;
         * write to a different TMP each time to avoid any RAW chains. */
        { XOR_DSZ64_DRR(TMP10, TMP6, TMP7), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(TMP11, TMP6, TMP7), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(TMP12, TMP6, TMP7), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(TMP13, TMP6, TMP7), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(TMP14, TMP6, TMP7), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c2 = time_scenario("B2  5 XORs alone, 5 triads", b2, ARRAY_SZ(b2));

    /* ── B3: 10 triads alternating LD / XOR (independent) ──────
     *
     * Each LD writes to TMP10-14; each XOR reads TMP6/TMP7 and writes
     * to a different TMP. No RAW dependency between LDs and XORs.
     *
     * If LDs and XORs overlap: cost ≈ max(B1-baseline, B2-baseline)
     * If they serialize: cost ≈ (B1-c0) + (B2-c0) + c0
     */
    ucode_t b3[] = {
        /* Seed */
        { ZEROEXT_DSZ32_DI(TMP0, 0xCAFE),
          ZEROEXT_DSZ32_DI(TMP6, 0x1111),
          ZEROEXT_DSZ32_DI(TMP7, 0x2222), NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A0),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A1),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A2), NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A3),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A4), NOP, NOP_SEQWORD },
        /* Measured: LD, XOR, LD, XOR, ... (10 triads) */
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP10, A0), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(TMP0, TMP6, TMP7),      NOP, NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP11, A1), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(TMP1, TMP6, TMP7),      NOP, NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP12, A2), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(TMP2, TMP6, TMP7),      NOP, NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP13, A3), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(TMP3, TMP6, TMP7),      NOP, NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP14, A4), NOP, NOP, NOP_SEQWORD },
        { XOR_DSZ64_DRR(TMP4, TMP6, TMP7),      NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c3 = time_scenario("B3  LD/XOR alternating 10 triads (indep)",
                                b3, ARRAY_SZ(b3));

    /* ── B4: 5 triads, each with [LD slot 0, XOR slot 1] ───────
     *
     * Intra-triad overlap test. If LD doesn't block other slots in the
     * same triad, this should be ≈ B1 (no extra cost for the XOR).
     */
    ucode_t b4[] = {
        { ZEROEXT_DSZ32_DI(TMP0, 0xCAFE),
          ZEROEXT_DSZ32_DI(TMP6, 0x1111),
          ZEROEXT_DSZ32_DI(TMP7, 0x2222), NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A0),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A1),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A2), NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A3),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A4), NOP, NOP_SEQWORD },
        /* Measured: 5 triads of [LD in slot 0, XOR in slot 1] */
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP10, A0),
          XOR_DSZ64_DRR(TMP0, TMP6, TMP7), NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP11, A1),
          XOR_DSZ64_DRR(TMP1, TMP6, TMP7), NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP12, A2),
          XOR_DSZ64_DRR(TMP2, TMP6, TMP7), NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP13, A3),
          XOR_DSZ64_DRR(TMP3, TMP6, TMP7), NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP14, A4),
          XOR_DSZ64_DRR(TMP4, TMP6, TMP7), NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c4 = time_scenario("B4  LD+XOR same triad x5", b4, ARRAY_SZ(b4));

    /* ── B5: dense — 1 LD + 2 XORs per triad x5 ────────────────
     *
     * If LDs serialize ONLY with each other (allowing full intra-triad
     * fill of XORs), this should be ≈ B1.
     * If LDs block all slots, this should be ≈ B4 (same as 1 XOR).
     */
    ucode_t b5[] = {
        { ZEROEXT_DSZ32_DI(TMP0, 0xCAFE),
          ZEROEXT_DSZ32_DI(TMP6, 0x1111),
          ZEROEXT_DSZ32_DI(TMP7, 0x2222), NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A0),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A1),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A2), NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A3),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A4), NOP, NOP_SEQWORD },
        /* Measured: 5 triads, each is LD + XOR + XOR */
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP10, A0),
          XOR_DSZ64_DRR(TMP0, TMP6, TMP7),
          XOR_DSZ64_DRR(TMP1, TMP6, TMP7), NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP11, A1),
          XOR_DSZ64_DRR(TMP2, TMP6, TMP7),
          XOR_DSZ64_DRR(TMP3, TMP6, TMP7), NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP12, A2),
          XOR_DSZ64_DRR(TMP4, TMP6, TMP7),
          XOR_DSZ64_DRR(TMP5, TMP6, TMP7), NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP13, A3),
          XOR_DSZ64_DRR(TMP0, TMP6, TMP7),
          XOR_DSZ64_DRR(TMP1, TMP6, TMP7), NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP14, A4),
          XOR_DSZ64_DRR(TMP2, TMP6, TMP7),
          XOR_DSZ64_DRR(TMP3, TMP6, TMP7), NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c5 = time_scenario("B5  LD + 2 XOR per triad x5 (dense)",
                                b5, ARRAY_SZ(b5));

    init_match_and_patch();
    do_fix_IN_patch();

    /* ── Verdict ──────────────────────────────────────────────── */
    int64_t b1_excess = (int64_t)c1 - (int64_t)c0;   /* cost of 5 LDs */
    int64_t b2_excess = (int64_t)c2 - (int64_t)c0;   /* cost of 5 XORs */
    int64_t b3_excess = (int64_t)c3 - (int64_t)c0;   /* alternating */
    int64_t b4_excess = (int64_t)c4 - (int64_t)c0;   /* same-triad */
    int64_t b5_excess = (int64_t)c5 - (int64_t)c0;   /* dense */

    int64_t serial_predict = b1_excess + b2_excess;   /* fully serial */
    int64_t overlap_predict = b1_excess;              /* fully overlapped */

    printf("\n--- Decode ---\n");
    printf("  B1-B0 (5 LD cost)     = %4" PRId64 " cyc\n", b1_excess);
    printf("  B2-B0 (5 XOR cost)    = %4" PRId64 " cyc\n", b2_excess);
    printf("  B3-B0 (LD/XOR alt)    = %4" PRId64 " cyc\n", b3_excess);
    printf("    full-serial predict = %4" PRId64 " cyc\n", serial_predict);
    printf("    full-overlap predict= %4" PRId64 " cyc\n", overlap_predict);
    printf("  B4-B0 (LD+XOR triad)  = %4" PRId64 " cyc\n", b4_excess);
    printf("  B5-B0 (LD+2XOR triad) = %4" PRId64 " cyc\n", b5_excess);

    printf("\n  Across-triad overlap: ");
    if (b3_excess <= overlap_predict + 5)
        printf("YES — LDs hide behind XORs (cost ≈ LD-alone)\n");
    else if (b3_excess >= serial_predict - 5)
        printf("NO  — full serialization (cost ≈ LD+XOR sum)\n");
    else
        printf("PARTIAL — somewhere between\n");

    printf("  Same-triad overlap : ");
    if (b4_excess <= b1_excess + 5)
        printf("YES — XOR in slot 1 is free against the LD in slot 0\n");
    else
        printf("NO  — same-triad XOR adds cost\n");

    printf("\nIf YES on either: stgbuf-resident state may be viable with\n");
    printf("careful scheduling. If NO on both: stgbuf is dead for hot path.\n");
    return 0;
}
