/*
 * bench_keccak.c — Phase 0 baseline for the Keccak microcode plan.
 *
 * Measures per-permutation cycles for two SUPERCOP `crypto_hash/keccakc1024`
 * implementations on the actual Goldmont hardware:
 *
 *   1) x86_64_asm  — hand-tuned scalar asm (SOTA on Goldmont per SUPERCOP)
 *   2) opt64lcu6   — C with Bebigokimisa + 6x unrolling (sanity baseline)
 *
 * Then measures a no-op microcode patch hooked on vmwrite, so we know the
 * pure dispatch cost. If that overhead is > 15 % of the asm baseline, the
 * hook strategy needs to change before any real microcode work.
 *
 * Build: make PROG=bench_keccak
 * Run:   sudo taskset -c 0 ./bench_keccak_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* The two permutation entry points provided by the SUPERCOP sources. Symbol
 * renames are done at link time (see Makefile) so the two implementations can
 * coexist in one binary. Both take a uint64_t[25] state, in place. */
extern void keccak_x86_64_asm_perm(uint64_t state[25]);
extern void keccak_opt64lcu6_perm(uint64_t state[25]);

/* ── timing ───────────────────────────────────────────────────────── */

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

#define BATCH 1000
#define REPS  100

/* ── no-op microcode patch (1 triad, returns immediately) ─────────── */

static void install_noop_patch(void) {
    ucode_t patch[] = {
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Fire vmwrite once. RDX gets clobbered by the dispatch sequence; we
 * pre-load it with a dummy value. RCX is a dummy operand. */
static inline void fire_noop_hook(void) {
    uint64_t a = 0, b = 0;
    asm volatile(
        "vmwrite %0, %1\n\t"
        :
        : "r"(a), "r"(b)
        : "memory", "cc");
}

/* ── cross-check: both impls must produce identical output ───────── */

static int verify_cross_consistency(void) {
    /* Start from a non-trivial state so we don't accidentally match on the
     * "all-zero special case". Mix a known constant pattern. */
    uint64_t base[25];
    for (int i = 0; i < 25; i++)
        base[i] = 0x0123456789ABCDEFULL * (i + 1) ^ 0xDEADBEEFCAFEBABEULL;

    uint64_t a[25], b[25];
    memcpy(a, base, sizeof(base));
    memcpy(b, base, sizeof(base));

    keccak_x86_64_asm_perm(a);
    keccak_opt64lcu6_perm(b);

    if (memcmp(a, b, sizeof(a)) != 0) {
        printf("FAIL: x86_64_asm vs opt64lcu6 produce different output.\n");
        for (int i = 0; i < 25; i++)
            printf("  [%2d]  asm=%016" PRIx64 "  opt64=%016" PRIx64 "  %s\n",
                   i, a[i], b[i], a[i] == b[i] ? "" : "***");
        return 1;
    }

    /* Also: 24 rounds applied to all-zero state has a well-known fingerprint
     * (RC[0]=0x01 propagates). We don't have the published KAT inline here,
     * so just sanity-check that the result isn't trivially zero. */
    uint64_t z[25] = {0};
    keccak_x86_64_asm_perm(z);
    int any_nonzero = 0;
    for (int i = 0; i < 25; i++) if (z[i]) { any_nonzero = 1; break; }
    if (!any_nonzero) {
        printf("FAIL: zero state mapped to zero (broken impl)\n");
        return 1;
    }

    printf("Cross-check OK: x86_64_asm == opt64lcu6 on test input.\n");
    return 0;
}

/* ── bench one impl ──────────────────────────────────────────────── */

typedef void (*perm_fn)(uint64_t *);

static void bench_perm(const char *label, perm_fn fn) {
    uint64_t state[25];
    /* Re-seed state every batch so we don't fall into a fixed-point cycle.
     * (Keccak has none, but rdtsc serializing is enough — keep it simple.) */
    uint64_t min = UINT64_MAX, sum = 0;
    for (int r = 0; r < REPS; r++) {
        for (int i = 0; i < 25; i++)
            state[i] = 0x0123456789ABCDEFULL * (i + 1);
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fn(state);
        uint64_t t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt;
        if (dt < min) min = dt;
    }
    printf("%-18s  min/perm %6" PRIu64 "  avg/perm %6" PRIu64 " cycles\n",
           label, min / BATCH, sum / REPS / BATCH);
}

static void bench_noop_hook(void) {
    /* Fire 1000 vmwrites per batch; the patch immediately ends. */
    uint64_t min = UINT64_MAX, sum = 0;
    for (int r = 0; r < REPS; r++) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fire_noop_hook();
        uint64_t t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt;
        if (dt < min) min = dt;
    }
    printf("%-18s  min/fire %6" PRIu64 "  avg/fire %6" PRIu64 " cycles\n",
           "no-op hook", min / BATCH, sum / REPS / BATCH);
}

/* ── main ────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== Phase 0 baseline: Keccak-f[1600] on Goldmont ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    if (verify_cross_consistency() != 0) {
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    printf("\n--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    bench_perm("x86_64_asm",   keccak_x86_64_asm_perm);
    bench_perm("opt64lcu6",    keccak_opt64lcu6_perm);

    /* Now install the no-op patch and measure hook overhead. */
    install_noop_patch();
    bench_noop_hook();

    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
