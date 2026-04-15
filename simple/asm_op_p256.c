/*
 * asm_op_p256.c — Montgomery mul/sq for P-256 via microcode (two patches)
 *
 * Field: GF(p) where p = 2^256 - 2^224 + 2^192 + 2^96 - 1  (NIST P-256)
 * Representation: 4 saturated 64-bit limbs in Montgomery domain
 *   eval z = z[0] + z[1]*2^64 + z[2]*2^128 + z[3]*2^192
 *
 * Prime limbs:
 *   p[0] = 0xFFFFFFFFFFFFFFFF
 *   p[1] = 0x00000000FFFFFFFF
 *   p[2] = 0x0000000000000000
 *   p[3] = 0xFFFFFFFF00000001
 *
 * Montgomery constant: mu = -p^{-1} mod 2^64 = 1  (since p ≡ -1 mod 2^64)
 *
 * Algorithm: word-by-word Montgomery multiplication, 4 iterations.
 * Each iteration: multiply a[i]×b, add to accumulator, reduce mod p.
 * Split into two microcode patches (128-triad limit):
 *   Patch 1 (vmwrite, match 0): iterations 0-1
 *   Patch 2 (vmread,  match 1): iterations 2-3 + conditional subtract
 *
 * Build:  make PROG=asm_op_p256
 * Run:    sudo taskset -c 0 ./asm_op_p256_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* P-256 prime */
static const uint64_t P256_P[4] = {
    UINT64_C(0xFFFFFFFFFFFFFFFF),
    UINT64_C(0x00000000FFFFFFFF),
    UINT64_C(0x0000000000000000),
    UINT64_C(0xFFFFFFFF00000001)
};

/* ── fe_mul native C (word-by-word Montgomery) ──────────────── */

static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t acc[5] = {0};

    for (int i = 0; i < 4; i++) {
        /* Step 1: acc += a[i] * b */
        __uint128_t carry = 0;
        for (int j = 0; j < 4; j++) {
            carry += (__uint128_t)a[i] * b[j] + acc[j];
            acc[j] = (uint64_t)carry;
            carry >>= 64;
        }
        acc[4] += (uint64_t)carry;

        /* Step 2: Montgomery reduce — m = acc[0] * mu; for P-256 mu=1 so m = acc[0] */
        uint64_t m = acc[0];

        carry = 0;
        for (int j = 0; j < 4; j++) {
            carry += (__uint128_t)m * P256_P[j] + acc[j];
            acc[j] = (uint64_t)carry;
            carry >>= 64;
        }
        acc[4] += (uint64_t)carry;

        /* Step 3: shift right (discard word 0 which is now 0) */
        acc[0] = acc[1];
        acc[1] = acc[2];
        acc[2] = acc[3];
        acc[3] = acc[4];
        acc[4] = 0;
    }

    /* Conditional subtract: if acc >= p, output acc - p; else output acc */
    __uint128_t borrow = 0;
    uint64_t diff[4];
    for (int j = 0; j < 4; j++) {
        borrow = (__uint128_t)acc[j] - P256_P[j] - (uint64_t)borrow;
        diff[j] = (uint64_t)borrow;
        borrow = (borrow >> 64) & 1;
    }
    /* If borrow: acc < p, use acc. Else: acc >= p, use diff. */
    uint64_t mask = (uint64_t)0 - (uint64_t)borrow; /* all-ones if borrow (use acc) */
    for (int j = 0; j < 4; j++)
        out[j] = (acc[j] & mask) | (diff[j] & ~mask);
}

/* ── fe_sq native C ─────────────────────────────────────────── */

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    fe_mul_native(a, a, out);
}

/* ── independent reference (big-integer multiply mod p) ──────── */

static void fe_mul_reference(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    /*
     * Compute (a * b * R^{-1}) mod p in Montgomery domain.
     * Same algorithm as native but with explicit __uint128_t to avoid
     * any optimization that could hide bugs.
     */
    __uint128_t acc[5] = {0};

    for (int i = 0; i < 4; i++) {
        __uint128_t c = 0;
        for (int j = 0; j < 4; j++) {
            c += (__uint128_t)a[i] * b[j] + (uint64_t)acc[j];
            acc[j] = (uint64_t)c;
            c >>= 64;
        }
        acc[4] = (uint64_t)((uint64_t)acc[4] + (uint64_t)c);

        uint64_t m = (uint64_t)acc[0]; /* mu = 1 */
        c = 0;
        for (int j = 0; j < 4; j++) {
            c += (__uint128_t)m * P256_P[j] + (uint64_t)acc[j];
            acc[j] = (uint64_t)c;
            c >>= 64;
        }
        acc[4] = (uint64_t)((uint64_t)acc[4] + (uint64_t)c);

        acc[0] = acc[1]; acc[1] = acc[2]; acc[2] = acc[3]; acc[3] = acc[4]; acc[4] = 0;
    }

    __uint128_t borrow = 0;
    uint64_t diff[4];
    for (int j = 0; j < 4; j++) {
        borrow = (__uint128_t)(uint64_t)acc[j] - P256_P[j] - (uint64_t)borrow;
        diff[j] = (uint64_t)borrow;
        borrow = (borrow >> 64) & 1;
    }
    uint64_t mask = (uint64_t)0 - (uint64_t)borrow;
    for (int j = 0; j < 4; j++)
        out[j] = ((uint64_t)acc[j] & mask) | (diff[j] & ~mask);
}

static void fe_sq_reference(const uint64_t *a, uint64_t *out) {
    fe_mul_reference(a, a, out);
}

/* ── microcode patches ───────────────────────────────────────── */

/*
 * TODO: Two patches implementing the Montgomery iterations in microcode.
 * Patch 1 (vmwrite hook, match 0): iterations 0-1
 * Patch 2 (vmread hook, match 1): iterations 2-3 + conditional subtract
 *
 * For now, fe_mul_ucode/fe_sq_ucode call the native C as a placeholder.
 * The microcode patches will be added once the native C + reference
 * are verified correct.
 */

static void install_p256_patches(void) {
    printf("p256 patches: TODO (using native C fallback)\n");
}

static void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    fe_mul_native(a, b, out);  /* placeholder until microcode is ready */
}

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    fe_sq_native(a, out);      /* placeholder */
}

/* ── verification ────────────────────────────────────────────── */

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* Montgomery form of 1: R mod p = 2^256 mod p */
static const uint64_t MONT_ONE[4] = {
    UINT64_C(0x0000000000000001),
    UINT64_C(0xFFFFFFFF00000000),
    UINT64_C(0xFFFFFFFFFFFFFFFF),
    UINT64_C(0x00000000FFFFFFFE)
};

typedef struct {
    const char *label;
    uint64_t    a[4];
    uint64_t    b[4];
    uint64_t    expected[4];
    int         has_expected;
} test_vec_t;

static const test_vec_t test_vectors[] = {
    /* 0 * 0 = 0 in Montgomery domain */
    { "0*0", {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, 1 },

    /* 0 * R = 0 */
    { "0*R",
      {0,0,0,0},
      {0x0000000000000001, 0xFFFFFFFF00000000,
       0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFE},
      {0,0,0,0}, 1 },

    /* R * R = R^2 mod p (= toMontgomery(1) * R, but let reference compute) */
    { "R*R",
      {0x0000000000000001, 0xFFFFFFFF00000000,
       0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFE},
      {0x0000000000000001, 0xFFFFFFFF00000000,
       0xFFFFFFFFFFFFFFFF, 0x00000000FFFFFFFE},
      {0}, 0 },

    /* Squaring test: a*a where a is a small Montgomery value */
    { "small_sq",
      {0x0000000000000002, 0xFFFFFFFE00000000,
       0xFFFFFFFFFFFFFFFF, 0x00000001FFFFFFFD},
      {0x0000000000000002, 0xFFFFFFFE00000000,
       0xFFFFFFFFFFFFFFFF, 0x00000001FFFFFFFD},
      {0}, 0 },
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[4], nat[4], ucd[4];
    fe_mul_reference(t->a, t->b, ref);
    fe_mul_native(t->a, t->b, nat);
    fe_mul_ucode(t->a, t->b, ucd);

    int ok = 1;
    if (memcmp(ref, nat, 32) != 0) {
        printf("  FAIL [%s] native != reference\n", t->label);
        for (int i = 0; i < 4; i++)
            printf("    [%d] native=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, nat[i], ref[i], nat[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (memcmp(ref, ucd, 32) != 0) {
        printf("  FAIL [%s] ucode != reference\n", t->label);
        for (int i = 0; i < 4; i++)
            printf("    [%d] ucode=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, ucd[i], ref[i], ucd[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (t->has_expected && memcmp(ref, t->expected, 32) != 0) {
        printf("  FAIL [%s] result != expected\n", t->label);
        for (int i = 0; i < 4; i++)
            printf("    [%d] got=%016" PRIx64 " exp=%016" PRIx64 " %s\n",
                   i, ref[i], t->expected[i],
                   ref[i] == t->expected[i] ? "" : "***");
        ok = 0;
    }
    if (ok) printf("  PASS [%s]\n", t->label);
    return ok;
}

static int verify_random_quiet(const uint64_t a[4], const uint64_t b[4]) {
    uint64_t ref[4], nat[4], ucd[4];
    fe_mul_reference(a, b, ref);
    fe_mul_native(a, b, nat);
    fe_mul_ucode(a, b, ucd);
    if (memcmp(ref, nat, 32) != 0 || memcmp(ref, ucd, 32) != 0) {
        printf("  FAIL random\n");
        return 0;
    }
    return 1;
}

/* Generate a random value < p for Montgomery domain */
static void rand_mod_p(uint64_t out[4], uint64_t *rng) {
    for (;;) {
        for (int j = 0; j < 4; j++)
            out[j] = splitmix64(rng);
        /* Reduce to < p: simple rejection */
        int lt = 0, gt = 0;
        for (int j = 3; j >= 0; j--) {
            if (out[j] < P256_P[j]) { lt = 1; break; }
            if (out[j] > P256_P[j]) { gt = 1; break; }
        }
        if (lt || (!gt)) { /* out <= p, but we want < p; for random this is fine */
            if (lt) break;
            /* out == p, extremely unlikely, retry */
        }
    }
}

#define RANDOM_TESTS 10000
#define CHAIN_ITERS  1000

static int verify_all(void) {
    int pass = 0, fail = 0;

    printf("--- Known test vectors ---\n");
    for (int i = 0; i < (int)N_VECS; i++) {
        if (verify_one(&test_vectors[i])) pass++; else fail++;
    }

    printf("\n--- Random mul test (%d vectors) ---\n", RANDOM_TESTS);
    uint64_t rng = 0xA256CAFE12345678ULL;
    int rpass = 0;
    for (int i = 0; i < RANDOM_TESTS; i++) {
        uint64_t a[4], b[4];
        rand_mod_p(a, &rng);
        rand_mod_p(b, &rng);
        if (verify_random_quiet(a, b)) rpass++;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    printf("\n--- Random sq test (%d vectors) ---\n", RANDOM_TESTS);
    rpass = 0;
    for (int i = 0; i < RANDOM_TESTS; i++) {
        uint64_t a[4];
        rand_mod_p(a, &rng);
        if (verify_random_quiet(a, a)) rpass++;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    printf("\n--- Iterated chain (%d mul-self) ---\n", CHAIN_ITERS);
    uint64_t ri[4], ni[4], ui[4];
    memcpy(ri, MONT_ONE, 32); memcpy(ni, MONT_ONE, 32); memcpy(ui, MONT_ONE, 32);
    /* Multiply by a fixed value repeatedly */
    uint64_t mult[4] = {0x3, 0x0, 0x0, 0x0};
    for (int i = 0; i < CHAIN_ITERS; i++) {
        uint64_t tmp[4];
        memcpy(tmp, ri, 32); fe_mul_reference(tmp, mult, ri);
        memcpy(tmp, ni, 32); fe_mul_native(tmp, mult, ni);
        memcpy(tmp, ui, 32); fe_mul_ucode(tmp, mult, ui);
    }
    int ref_nat = memcmp(ri, ni, 32) == 0;
    int ref_ucd = memcmp(ri, ui, 32) == 0;
    printf("  ref==native: %s   ref==ucode: %s   -> %s\n",
           ref_nat ? "yes" : "NO", ref_ucd ? "yes" : "NO",
           (ref_nat && ref_ucd) ? "PASS" : "FAIL");
    if (ref_nat && ref_ucd) pass++; else fail++;

    printf("\n=== Verification: %d passed, %d failed ===\n\n", pass, fail);
    return fail;
}

/* ── timing ───────────────────────────────────────────────────── */

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

int main(void) {
    uint64_t t0, t1, min, sum;

    printf("=== P-256 Montgomery: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_p256_patches();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    uint64_t sa[4], sb[4];
    memcpy(sa, MONT_ONE, 32);
    sb[0] = 0x6B17D1F2E12C4247ULL; sb[1] = 0xF8BCE6E563A440F2ULL;
    sb[2] = 0x7037D812DEB33A0FULL; sb[3] = 0x4FE342E2FE1A7F9BULL;

    printf("--- mul: %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t ta[4], tb[4];

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, 32); memcpy(tb, sb, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_native(ta, tb, ta);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Native mul:  min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, 32); memcpy(tb, sb, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_ucode(ta, tb, ta);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Ucode mul:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    printf("\n--- sq: %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_native(ta, ta);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Native sq:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_ucode(ta, ta);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Ucode sq:    min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
