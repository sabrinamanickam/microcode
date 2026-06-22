/*
 * stgbuf_bench.c — Cycles per LDSTGBUF / STADSTGBUF micro-op.
 *
 * We don't have a clean isolated "stgbuf op alone" since each micro-op
 * sits in a triad slot. So we measure incremental cost: vary the number
 * of stgbuf ops in a patch and subtract.
 *
 * Method: install patch -> fire BATCH times in a tight loop -> repeat
 * REPS batches -> report min-per-fire (least-noise estimator) in cycles.
 *
 * Scenarios:
 *   S0  : empty patch (END_SEQWORD only)       -> hook-overhead baseline
 *   S1  : 1 NOP triad   + END                   -> triad-overhead baseline
 *   S2  : 3 NOP triads  + END
 *   S3  : 1 STADSTGBUF (one slot in one triad)  -> single-op cost
 *   S4  : 5 STADSTGBUF, one per triad           -> stores serialized
 *   S5  : 5 LDSTGBUF,  one per triad            -> loads serialized
 *   S6  : 5 STADSTGBUF, packed (3+2 per triad)  -> tests intra-triad ST
 *   S7  : 5 LDSTGBUF,  packed (3+2 per triad)   -> tests intra-triad LD
 *   S8  : 5 ST then 5 LD all packed             -> realistic fe25519 spill
 *
 * Derived numbers:
 *   triad cost           = S1 - S0
 *   STADSTGBUF cost      ≈ (S4 - S0 - 5*triad)/5  if not packed
 *                        ≈ (S6 - S0 - 2*triad)/5  if packed (3 ops/triad)
 *   LDSTGBUF cost        analogous
 *
 * Comparison target: C-side `mov reg, [mem]` is ~4 cyc but pipelined;
 * 5 movs ≈ 5-7 cyc back-to-back. STADSTGBUF beats that if its
 * per-op cost ≤ ~1 cyc and it can be packed 3-per-triad.
 *
 * Build:  make PROG=stgbuf_bench
 * Run:    sudo taskset -c 0 ./stgbuf_bench_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define REGION  0x7c00

/* fire_patch — same convention as the test harness */
static inline uint64_t fire_patch(void) {
    uint64_t res;
    asm volatile(
        "xor eax, eax\n\t"
        "vmwrite rcx, rdx\n\t"
        : "=a"(res)
        :
        : "rcx", "rdx", "memory", "cc"
    );
    return res;
}

static void reinstall(ucode_t *p, int n) {
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(REGION, p, n);
    hook_match_and_patch(0, 0x0cd8, REGION);
}

/* RDTSC pair (cpuid-serialized at start, rdtscp+cpuid at end). */
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

/* time_scenario: install patch, fire BATCH × REPS times, return min/op. */
static uint64_t time_scenario(const char *name, ucode_t *p, int n) {
    reinstall(p, n);

    /* Warm-up: a few fires to settle caches/branch predictor */
    for (int i = 0; i < 100; i++) (void)fire_patch();

    uint64_t min = UINT64_MAX;
    uint64_t sum = 0;

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
    printf("  %-44s  min %4" PRIu64 "  avg %4" PRIu64 "  cyc/fire\n",
           name, min_per, avg_per);
    return min_per;
}

/* Addresses within the known-safe 0xb000-0xc000 block. */
#define A0 0xb000
#define A1 0xb040
#define A2 0xb080
#define A3 0xb0c0
#define A4 0xb100

int main(void) {
    printf("=== stgbuf micro-op latency probe ===\n");
    printf("BATCH = %d  REPS = %d  (min-per-fire reported)\n\n", BATCH, REPS);

    assign_to_core(0);

    /* ── S0: empty patch ──────────────────────────────────────── */
    ucode_t s0[] = {
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c0 = time_scenario("S0  empty patch (END only)", s0, ARRAY_SZ(s0));

    /* ── S1: 1 NOP triad + END ────────────────────────────────── */
    ucode_t s1[] = {
        { NOP, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c1 = time_scenario("S1  1 NOP triad", s1, ARRAY_SZ(s1));

    /* ── S2: 3 NOP triads ─────────────────────────────────────── */
    ucode_t s2[] = {
        { NOP, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c2 = time_scenario("S2  3 NOP triads", s2, ARRAY_SZ(s2));

    /* ── S3: 1 STADSTGBUF + END ───────────────────────────────── */
    ucode_t s3[] = {
        { ZEROEXT_DSZ32_DI(TMP0, 0xCAFE), NOP, NOP, NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A0), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c3 = time_scenario("S3  1 STADSTGBUF (in own triad)", s3, ARRAY_SZ(s3));

    /* ── S4: 5 STADSTGBUF, one per triad (serialized) ─────────── */
    ucode_t s4[] = {
        { ZEROEXT_DSZ32_DI(TMP0, 0xCAFE), NOP, NOP, NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A0), NOP, NOP, NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A1), NOP, NOP, NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A2), NOP, NOP, NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A3), NOP, NOP, NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A4), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c4 = time_scenario("S4  5 STADSTGBUF, one/triad (5 triads)",
                                s4, ARRAY_SZ(s4));

    /* ── S5: 5 LDSTGBUF, one per triad ────────────────────────── */
    /* Pre-seed all 5 slots first (in the same patch) so LDs read defined
     * data. Then 5 LDSTGBUFs into separate TMPs. */
    ucode_t s5[] = {
        { ZEROEXT_DSZ32_DI(TMP0, 0x1111), NOP, NOP, NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A0),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A1),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A2), NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A3),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A4), NOP, NOP_SEQWORD },
        /* The actual measured part: 5 separate LDSTGBUFs */
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP0, A0), NOP, NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP1, A1), NOP, NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP2, A2), NOP, NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP3, A3), NOP, NOP, NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP4, A4), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c5 = time_scenario("S5  5 LDSTGBUF, one/triad (5 LD triads)",
                                s5, ARRAY_SZ(s5));

    /* ── S6: 5 STADSTGBUF packed (3 + 2 in two triads) ────────── */
    ucode_t s6[] = {
        { ZEROEXT_DSZ32_DI(TMP0, 0xCAFE), NOP, NOP, NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A0),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A1),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A2), NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A3),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A4), NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c6 = time_scenario("S6  5 STADSTGBUF packed (2 triads)",
                                s6, ARRAY_SZ(s6));

    /* ── S7: 5 LDSTGBUF packed (3 + 2) ────────────────────────── */
    /* Seed first, then read packed. */
    ucode_t s7[] = {
        { ZEROEXT_DSZ32_DI(TMP0, 0x2222), NOP, NOP, NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A0),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A1),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A2), NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A3),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A4), NOP, NOP_SEQWORD },
        /* Measured part: 5 LDs in 2 triads */
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP0, A0),
          LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP1, A1),
          LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP2, A2), NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP3, A3),
          LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP4, A4), NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c7 = time_scenario("S7  5 LDSTGBUF packed (2 triads)",
                                s7, ARRAY_SZ(s7));

    /* ── S8: 5 ST then 5 LD, all packed (4 triads of stgbuf work) ─ */
    ucode_t s8[] = {
        { ZEROEXT_DSZ32_DI(TMP0, 0x3333), ZEROEXT_DSZ32_DI(TMP1, 0x4444),
          ZEROEXT_DSZ32_DI(TMP2, 0x5555), NOP_SEQWORD },
        /* Store all 5 (packed: 3 + 2) */
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A0),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP1, A1),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP2, A2), NOP_SEQWORD },
        { STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP0, A3),
          STADSTGBUF_DSZ64_ASZ16_SC1_RI(TMP1, A4), NOP, NOP_SEQWORD },
        /* Load all 5 back (packed: 3 + 2) */
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP3, A0),
          LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP4, A1),
          LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP5, A2), NOP_SEQWORD },
        { LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP6, A3),
          LDSTGBUF_DSZ64_ASZ16_SC1_DI(TMP7, A4), NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    uint64_t c8 = time_scenario("S8  5 ST + 5 LD packed (4 stgbuf triads)",
                                s8, ARRAY_SZ(s8));

    /* ── Derived costs ────────────────────────────────────────── */
    init_match_and_patch();
    do_fix_IN_patch();

    printf("\n--- Derived per-op costs (subtract baselines) ---\n");
    int64_t triad_cost = (int64_t)c1 - (int64_t)c0;
    int64_t triad_cost_3 = ((int64_t)c2 - (int64_t)c0) / 3;
    printf("  triad cost (S1-S0)            : %+" PRId64 " cyc\n", triad_cost);
    printf("  triad cost ((S2-S0)/3)        : %+" PRId64 " cyc\n", triad_cost_3);
    printf("  effective triad cost (avg)    : %+" PRId64 " cyc\n",
           (triad_cost + triad_cost_3) / 2);

    int64_t t = (triad_cost + triad_cost_3) / 2;
    if (t < 1) t = 1;

    printf("\n  --- per-op cost (cycles) ---\n");
    /* S3: 1 STADSTGBUF in its own triad → ~1 extra triad over S1 */
    printf("  STADSTGBUF×1 (own triad): S3-S1 = %+" PRId64 " cyc\n",
           (int64_t)c3 - (int64_t)c1);
    /* S4 = c0 + 1 NOP-triad + 5 ST-triads + END-triad (vs S0 = c0) */
    printf("  STADSTGBUF avg one/triad : (S4-S0-%" PRId64 ")/5 = %+" PRId64 " cyc\n",
           (int64_t)(t * (long)6),   /* 1 setup NOP + 5 ST + 1 END = 7 triads; vs S0=1 */
           ((int64_t)c4 - (int64_t)c0 - 6*t) / 5);
    printf("  LDSTGBUF avg one/triad   : (S5-seed-overhead)/5 ≈ %+" PRId64 " cyc\n",
           ((int64_t)c5 - (int64_t)c0 - 3*t   /* MOV + 2 packed seed triads */
            - 5*t                              /* 5 LD triads themselves */
            ) / 5 + t                          /* add the per-triad cost back per LD */ );
    printf("  STADSTGBUF packed (3/triad) : (S6-S0-2*t)/5 = %+" PRId64 " cyc\n",
           ((int64_t)c6 - (int64_t)c0 - 2*t) / 5);
    printf("  LDSTGBUF  packed (3/triad)  : (S7-seed)/5  ≈ %+" PRId64 " cyc\n",
           ((int64_t)c7 - (int64_t)c0 - 5*t) / 5);
    printf("  fe25519 spill (S8 over S0)  : %+" PRId64 " cyc total\n",
           (int64_t)c8 - (int64_t)c0);

    printf("\nNote: 'per-op' numbers are best-effort decomposition;\n");
    printf("      raw min-per-fire is the most reliable comparison number.\n");
    printf("      Compare S8 (≈stgbuf round-trip for 5 limbs) against the\n");
    printf("      current C-wrapper memory cost (~5-7 cyc for 5 movs).\n");

    return 0;
}
