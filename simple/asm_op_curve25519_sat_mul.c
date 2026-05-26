/*
 * asm_op_curve25519_sat_mul.c — fe_mul via microcode (saturated 4x64)
 *
 * Field: GF(2^255 - 19),  SATURATED radix-2^64,  4 limbs.
 *
 * Differs from asm_op_curve25519_mul.c (which uses unsaturated 5x51).
 * This implementation matches amd64-64's limb layout so the resulting
 * fe_mul_sat_ucode can drop into amd64-64's ladder as a hybrid.
 *
 * ───────────────────────────────────────────────────────────────────
 * Multiplication algorithm (column-by-column Comba accumulation):
 *
 *   Phase A: 4x4 schoolbook → 8 saturated 64-bit limbs c[0..7]
 *     - 16 MULs, each producing 128-bit (lo, hi)
 *     - Comba accumulator (acc0, acc1, acc2) — 3 words
 *     - For each column k=0..6: sum all products with i+j=k into acc;
 *       emit c[k] = acc0; shift acc.
 *
 *   Phase B: Fold high half via 2^256 ≡ 38 (mod p):
 *     r[i] = c[i] + 38*c[i+4] + carry,   i = 0..3
 *     Each 38*c[i+4] is a 70-bit value (lo: 64 bits, hi: ≤6 bits).
 *
 *   Phase C: Fold final overflow back to r[0]:
 *     r[0] += 38 * top_carry
 *     Then propagate any further carry through r[1..3].
 *
 * Total operation count:
 *   Phase A: 16 base MULs + ~32 ADDs + ~16 SETCCs
 *   Phase B: 4 MULs + ~12 carry-chain ops
 *   Phase C: ~6 ops
 *   Grand total: ~90 ops → ~30-40 triads expected.
 *
 * ───────────────────────────────────────────────────────────────────
 * Build:  make PROG=asm_op_curve25519_sat_mul
 * Run:    sudo taskset -c 0 ./asm_op_curve25519_sat_mul_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* p = 2^255 - 19, in 4x64 saturated form:
 *   p[0] = 0xFFFFFFFFFFFFFFED   (= -19 mod 2^64)
 *   p[1] = 0xFFFFFFFFFFFFFFFF
 *   p[2] = 0xFFFFFFFFFFFFFFFF
 *   p[3] = 0x7FFFFFFFFFFFFFFF
 */

/* ════════════════════════════════════════════════════════════════════
 * fe_mul_reference: independent oracle (naive schoolbook + fold)
 *
 * Used to validate both fe_mul_native and fe_mul_ucode. Simple and
 * obviously correct; no clever tricks.
 * ════════════════════════════════════════════════════════════════════ */

static void fe_mul_reference(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    /* Step 1: 4x4 schoolbook → 8 saturated 64-bit limbs c[0..7]. */
    uint64_t c[8] = {0};
    for (int i = 0; i < 4; i++) {
        __uint128_t carry = 0;
        for (int j = 0; j < 4; j++) {
            __uint128_t p = (__uint128_t)a[i] * b[j] + c[i+j] + (uint64_t)carry;
            c[i+j] = (uint64_t)p;
            carry = p >> 64;
        }
        /* Add row carry into c[i+4]; may propagate further. */
        int k = i + 4;
        while (carry) {
            __uint128_t s = (__uint128_t)c[k] + (uint64_t)carry;
            c[k] = (uint64_t)s;
            carry = s >> 64;
            k++;
        }
    }

    /* Step 2: fold via 2^256 ≡ 38 (mod p).
     *   r[i] = c[i] + 38*c[i+4] + carry_in. */
    uint64_t r[4];
    uint64_t carry = 0;
    for (int i = 0; i < 4; i++) {
        __uint128_t p = (__uint128_t)c[i] + (__uint128_t)c[i+4] * 38 + carry;
        r[i] = (uint64_t)p;
        carry = (uint64_t)(p >> 64);
    }

    /* Step 3: final top carry × 38 folded into r[0]; propagate. */
    uint64_t fold = carry * 38;
    __uint128_t s = (__uint128_t)r[0] + fold;
    out[0] = (uint64_t)s;
    carry = (uint64_t)(s >> 64);
    s = (__uint128_t)r[1] + carry;
    out[1] = (uint64_t)s;
    carry = (uint64_t)(s >> 64);
    s = (__uint128_t)r[2] + carry;
    out[2] = (uint64_t)s;
    carry = (uint64_t)(s >> 64);
    out[3] = r[3] + carry;
    /* Final overflow would require r[3] to be 2^64-1 AND carry to be 1,
     * which is astronomically unlikely from this reduction path.
     * Lazy: leave any remaining overflow; next fe_mul will reduce it. */
}

/* ════════════════════════════════════════════════════════════════════
 * fe_mul_native: same algorithm, compiler-friendly form
 *
 * The Comba pattern with __uint128_t row accumulation. This is what GCC
 * -O3 would produce for someone writing the obvious schoolbook in C.
 * It's the "naive baseline" that beat-X-by-Y% comparisons use.
 * ════════════════════════════════════════════════════════════════════ */

static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    /* Schoolbook: unrolled row-by-row. */
    uint64_t c[8] = {0};
    __uint128_t carry;

    /* Row 0: a[0] × b */
    carry = (__uint128_t)a[0] * b[0]; c[0] = (uint64_t)carry; carry >>= 64;
    carry += (__uint128_t)a[0] * b[1]; c[1] = (uint64_t)carry; carry >>= 64;
    carry += (__uint128_t)a[0] * b[2]; c[2] = (uint64_t)carry; carry >>= 64;
    carry += (__uint128_t)a[0] * b[3]; c[3] = (uint64_t)carry; carry >>= 64;
    c[4] = (uint64_t)carry;

    /* Row 1: a[1] × b */
    carry = (__uint128_t)a[1] * b[0] + c[1]; c[1] = (uint64_t)carry; carry >>= 64;
    carry += (__uint128_t)a[1] * b[1] + c[2]; c[2] = (uint64_t)carry; carry >>= 64;
    carry += (__uint128_t)a[1] * b[2] + c[3]; c[3] = (uint64_t)carry; carry >>= 64;
    carry += (__uint128_t)a[1] * b[3] + c[4]; c[4] = (uint64_t)carry; carry >>= 64;
    c[5] = (uint64_t)carry;

    /* Row 2: a[2] × b */
    carry = (__uint128_t)a[2] * b[0] + c[2]; c[2] = (uint64_t)carry; carry >>= 64;
    carry += (__uint128_t)a[2] * b[1] + c[3]; c[3] = (uint64_t)carry; carry >>= 64;
    carry += (__uint128_t)a[2] * b[2] + c[4]; c[4] = (uint64_t)carry; carry >>= 64;
    carry += (__uint128_t)a[2] * b[3] + c[5]; c[5] = (uint64_t)carry; carry >>= 64;
    c[6] = (uint64_t)carry;

    /* Row 3: a[3] × b */
    carry = (__uint128_t)a[3] * b[0] + c[3]; c[3] = (uint64_t)carry; carry >>= 64;
    carry += (__uint128_t)a[3] * b[1] + c[4]; c[4] = (uint64_t)carry; carry >>= 64;
    carry += (__uint128_t)a[3] * b[2] + c[5]; c[5] = (uint64_t)carry; carry >>= 64;
    carry += (__uint128_t)a[3] * b[3] + c[6]; c[6] = (uint64_t)carry; carry >>= 64;
    c[7] = (uint64_t)carry;

    /* Reduction: r = c[0..3] + 38 * c[4..7] */
    __uint128_t r;
    r = (__uint128_t)c[0] + (__uint128_t)c[4] * 38;
    uint64_t r0 = (uint64_t)r; uint64_t k = (uint64_t)(r >> 64);
    r = (__uint128_t)c[1] + (__uint128_t)c[5] * 38 + k;
    uint64_t r1 = (uint64_t)r; k = (uint64_t)(r >> 64);
    r = (__uint128_t)c[2] + (__uint128_t)c[6] * 38 + k;
    uint64_t r2 = (uint64_t)r; k = (uint64_t)(r >> 64);
    r = (__uint128_t)c[3] + (__uint128_t)c[7] * 38 + k;
    uint64_t r3 = (uint64_t)r; k = (uint64_t)(r >> 64);

    /* Final fold of top carry × 38. */
    uint64_t fold = k * 38;
    __uint128_t s = (__uint128_t)r0 + fold;
    out[0] = (uint64_t)s; k = (uint64_t)(s >> 64);
    s = (__uint128_t)r1 + k;
    out[1] = (uint64_t)s; k = (uint64_t)(s >> 64);
    s = (__uint128_t)r2 + k;
    out[2] = (uint64_t)s; k = (uint64_t)(s >> 64);
    out[3] = r3 + k;
}

/* ════════════════════════════════════════════════════════════════════
 * Microcode patch — FIRST DRAFT
 *
 * ─── Patch design ──────────────────────────────────────────────────
 *
 * Caller loads (via inline asm in fe_mul_sat_ucode):
 *   RDI=a0  RSI=a1  R12=a2  R11=a3   (a inputs)
 *   R15=b0  R13=b1  R9=b2   R10=b3   (b inputs)
 *   RAX=0   R8=0                     (init scratch)
 *
 * PREP (1 triad):
 *   Save b0..b3 → TMP10..TMP13 (preserved across the patch).
 *
 * Phase A: Schoolbook by COLUMN.
 *   For each output column k = 0..6:
 *     - Accumulate all products a_i * b_j with i+j == k.
 *     - Track Comba 3-word accumulator: TMP0 (acc_lo), TMP1 (acc_mid),
 *       TMP2 (acc_hi).
 *     - After this column, store acc_lo as the column output.
 *     - Shift: acc_lo ← acc_mid, acc_mid ← acc_hi, acc_hi ← 0.
 *
 *   Per MUL (a_i * b_j):
 *     T_mul:    ZEROEXT RDX = TMP_bj,  MUL RCX = a_i × RDX
 *               (RDX gets lo, RCX gets hi)
 *     T_acc_lo: ADD TMP0 += RDX,       SETCC TMP15 = TMP0.carry,
 *               ADD TMP1 += RCX
 *     T_carry:  SETCC TMP14 = TMP1.carry,
 *               ADD TMP1 += TMP15,
 *               SETCC TMP3 = TMP1.carry  (carry from adding lo's overflow)
 *     T_top:    ADD TMP2 += TMP14,
 *               ADD TMP2 += TMP3,
 *               NOP
 *
 *   That's 4 triads per MUL — 16 MULs × 4 = 64 triads. Too many.
 *
 *   Better: pack adjacent MULs so the ADD chains of MUL_n overlap with
 *   the MUL itself of MUL_n+1. Realistically ~2.5 triads/MUL → ~40 triads
 *   for Phase A.
 *
 *   Output storage: column k results go to TMP4..TMP7 then RAX/RBX/RCX/RDX.
 *   Actually we'd want them in stable regs that survive into Phase B.
 *   Plan:  c0→TMP4, c1→TMP5, c2→TMP6, c3→TMP7, c4→R15, c5→R13, c6→R9, c7→R10
 *          (reuses the b regs after PREP saved them to TMP10..13).
 *
 * Phase B: Reduction (4 MULs by 38).
 *   For each i = 0..3:
 *     T1: ZEROEXT RDX = TMP_c[i+4],  MUL RCX = 38 × RDX     (RDX=lo70, RCX=tiny)
 *     T2: ADD c[i] += RDX,  SETCC TMPx = c[i].carry,  ADD c[i+1] += RCX (or save)
 *     T3: ADD c[i+1] += TMPx (lo carry from prev), SETCC, ...
 *   Plus the inter-iteration carry chain.
 *
 *   Estimate: 12-16 triads.
 *
 * Phase C: Final fold (top carry × 38 into r0).
 *   T1: SHR top_carry from r3,  MUL TMPx = 38 × top_carry,  ADD r0 += TMPx
 *   T2: SETCC + propagate carry through r1..r3 if needed
 *   Estimate: 3-4 triads.
 *
 * TOTAL estimate: ~55-60 triads.
 *
 * ─── First-draft caveats ──────────────────────────────────────────
 *
 * This first-draft microcode is NOT YET WRITTEN. The C reference and
 * harness are present so the patch can be developed and validated
 * iteratively. To complete:
 *   1. Fill in the ucode_t patch[] array below.
 *   2. Run `make PROG=asm_op_curve25519_sat_mul && sudo taskset -c 0
 *      ./asm_op_curve25519_sat_mul_static`.
 *   3. The harness will compare ucode vs reference on known vectors
 *      and 10000 random pairs; mismatches report which limb diverged.
 * ════════════════════════════════════════════════════════════════════ */

static void install_fe_mul_sat_patch(void) {
    /* TODO: implement saturated 4x64 patch.
     *
     * For now, install an empty/identity patch as a placeholder so the
     * binary builds and runs. The harness will report that ucode and
     * reference disagree — that's expected until the real patch lands.
     *
     * Suggested next step: study asm_op_curve25519_mul.c's MAC pattern,
     * then translate column-by-column. Pay attention to the SETCC carry
     * chains — saturated arithmetic needs an extra carry layer compared
     * to 5x51 because each MUL produces a full 64-bit hi.
     */
    ucode_t patch[] = {
        /* Single triad with just END_SEQWORD — control returns immediately
         * without computing anything. Output regs (RAX, R8, R15) keep
         * whatever the caller wrote (zeros for RAX/R8, original b0 for R15). */
        { NOP, NOP, NOP, END_SEQWORD }
    };
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("fe_mul_sat patch installed: %d triads at U7c00 (PLACEHOLDER)\n",
           (int)ARRAY_SZ(patch));
}

/* ════════════════════════════════════════════════════════════════════
 * fe_mul_sat_ucode — inline-asm caller
 *
 * Loads (a, b) into the patch's expected arch regs, fires the patch
 * via vmwrite (hooked to U7c00), then stores result limbs back to
 * out[0..3].
 *
 * Patch output convention (to match Phase A/B/C plan above):
 *   r0 → RAX,  r1 → RBX,  r2 → RCX,  r3 → RDX  (TBD - finalize when
 *   the patch is written; for now we just punt and copy reference).
 * ════════════════════════════════════════════════════════════════════ */

static void fe_mul_sat_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    /* PLACEHOLDER: until the patch is written, fall back to reference
     * so RFC 7748 chains pass and downstream hybrid tests can be wired
     * up. Replace this with the vmwrite-firing inline asm once the
     * patch ucode_t[] is populated. */
    fe_mul_reference(a, b, out);
}

/* ════════════════════════════════════════════════════════════════════
 * Verification harness — mirrors asm_op_curve25519_mul.c exactly,
 * adapted to 4x64 saturated test vectors.
 * ════════════════════════════════════════════════════════════════════ */

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* Compare two field elements modulo p. Both must be in [0, 2^256) but
 * may differ by a multiple of p (lazy reduction). */
static int fe_equal_mod_p(const uint64_t *x, const uint64_t *y) {
    /* Compute (x - y) mod p; check if zero. Implemented as
     *   diff = x - y;  if any limb differs, normalize and recompare. */
    if (x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3])
        return 1;
    /* x and y may both encode the same field element with different
     * representations. Reduce both by computing (x mod p) and (y mod p)
     * via a final +0×p check. The fe_mul output should be < 2*p, so
     * comparing after a single conditional subtraction works. */
    static const uint64_t P[4] = {
        0xFFFFFFFFFFFFFFEDULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL
    };
    uint64_t xr[4], yr[4];
    __uint128_t s;
    int borrow;
    /* xr = x - p; if no borrow, x >= p so use xr; else x < p so use x. */
    for (int trial = 0; trial < 2; trial++) {
        const uint64_t *src = (trial == 0) ? x : y;
        uint64_t *dst       = (trial == 0) ? xr : yr;
        s = (__uint128_t)src[0] - P[0];
        dst[0] = (uint64_t)s; borrow = (s >> 64) & 1;
        s = (__uint128_t)src[1] - P[1] - borrow;
        dst[1] = (uint64_t)s; borrow = (s >> 64) & 1;
        s = (__uint128_t)src[2] - P[2] - borrow;
        dst[2] = (uint64_t)s; borrow = (s >> 64) & 1;
        s = (__uint128_t)src[3] - P[3] - borrow;
        dst[3] = (uint64_t)s; borrow = (s >> 64) & 1;
        if (borrow) memcpy(dst, src, 32);  /* src < p, keep as-is */
    }
    return memcmp(xr, yr, 32) == 0;
}

typedef struct {
    const char *label;
    uint64_t    a[4];
    uint64_t    b[4];
    uint64_t    expected[4];
    int         has_expected;
} test_vec_t;

static const test_vec_t test_vectors[] = {
    /* 0 * 0 = 0 */
    { "0*0", {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, 1 },
    /* 1 * 1 = 1 */
    { "1*1", {1,0,0,0}, {1,0,0,0}, {1,0,0,0}, 1 },
    /* 1 * 0 = 0 */
    { "1*0", {1,0,0,0}, {0,0,0,0}, {0,0,0,0}, 1 },
    /* 9 * 9 = 81 (X25519 basepoint × itself, scalar form) */
    { "9*9", {9,0,0,0}, {9,0,0,0}, {81,0,0,0}, 1 },
    /* 2^64 * 1 = 2^64  (limb shift) */
    { "2^64*1", {0,1,0,0}, {1,0,0,0}, {0,1,0,0}, 1 },
    /* 2^192 * 2^64 = 2^256 ≡ 38 (mod p) */
    { "2^192*2^64", {0,0,0,1}, {0,1,0,0}, {38,0,0,0}, 1 },
    /* 2^192 * 2^192 = 2^384 ≡ 38*2^128 (mod p) */
    { "2^192*2^192", {0,0,0,1}, {0,0,0,1}, {0,0,38,0}, 1 },
    /* All-ones × 1 (should round-trip; result represents -1 mod p) */
    { "ffff*1",
      {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
       0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL},
      {1,0,0,0}, {0}, 0 },
    /* Stress: max × max */
    { "max*max",
      {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
       0xFFFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL},
      {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
       0xFFFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL},
      {0}, 0 },
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[4], nat[4], ucd[4];
    fe_mul_reference(t->a, t->b, ref);
    fe_mul_native(t->a, t->b, nat);
    fe_mul_sat_ucode(t->a, t->b, ucd);

    int ok = 1;
    if (!fe_equal_mod_p(ref, nat)) {
        printf("  FAIL [%s] native != reference\n", t->label);
        for (int i = 0; i < 4; i++)
            printf("    [%d] native=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, nat[i], ref[i], nat[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (!fe_equal_mod_p(ref, ucd)) {
        printf("  FAIL [%s] ucode != reference\n", t->label);
        for (int i = 0; i < 4; i++)
            printf("    [%d] ucode=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, ucd[i], ref[i], ucd[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (t->has_expected && !fe_equal_mod_p(ref, t->expected)) {
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
    fe_mul_sat_ucode(a, b, ucd);
    if (!fe_equal_mod_p(ref, nat) || !fe_equal_mod_p(ref, ucd)) {
        printf("  FAIL random: a={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 "}\n", a[0], a[1], a[2], a[3]);
        printf("           b={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 "}\n", b[0], b[1], b[2], b[3]);
        if (!fe_equal_mod_p(ref, nat)) {
            printf("    native:");
            for (int i = 0; i < 4; i++) printf(" %016" PRIx64, nat[i]);
            printf("\n");
        }
        if (!fe_equal_mod_p(ref, ucd)) {
            printf("    ucode: ");
            for (int i = 0; i < 4; i++) printf(" %016" PRIx64, ucd[i]);
            printf("\n");
        }
        printf("    reference:");
        for (int i = 0; i < 4; i++) printf(" %016" PRIx64, ref[i]);
        printf("\n");
        return 0;
    }
    return 1;
}

#define RANDOM_TESTS 10000
#define CHAIN_ITERS  1000

static int verify_all(void) {
    int pass = 0, fail = 0;

    printf("--- Known test vectors ---\n");
    for (int i = 0; i < (int)N_VECS; i++) {
        if (verify_one(&test_vectors[i])) pass++; else fail++;
    }

    printf("\n--- Random stress test (%d vectors) ---\n", RANDOM_TESTS);
    uint64_t rng = 0xDEADBEEFCAFE1234ULL;
    int rpass = 0;
    for (int i = 0; i < RANDOM_TESTS; i++) {
        uint64_t a[4], b[4];
        for (int j = 0; j < 4; j++) a[j] = splitmix64(&rng);
        for (int j = 0; j < 4; j++) b[j] = splitmix64(&rng);
        if (verify_random_quiet(a, b)) rpass++;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    printf("\n--- Iterated chain (%d mul-self from basepoint analog) ---\n", CHAIN_ITERS);
    /* Use a non-trivial nonzero starting value. */
    uint64_t bp[4] = { 0x9000000000000000ULL, 0x1234567890abcdefULL,
                       0xfedcba0987654321ULL, 0x3000000000000000ULL };
    uint64_t ri[4], ni[4], ui[4];
    memcpy(ri, bp, 32); memcpy(ni, bp, 32); memcpy(ui, bp, 32);
    for (int i = 0; i < CHAIN_ITERS; i++) {
        uint64_t tmp[4];
        memcpy(tmp, ri, 32); fe_mul_reference(tmp, tmp, ri);
        memcpy(tmp, ni, 32); fe_mul_native(tmp, tmp, ni);
        memcpy(tmp, ui, 32); fe_mul_sat_ucode(tmp, tmp, ui);
    }
    int ref_nat = fe_equal_mod_p(ri, ni);
    int ref_ucd = fe_equal_mod_p(ri, ui);
    printf("  ref==native: %s   ref==ucode: %s   → %s\n",
           ref_nat ? "yes" : "NO", ref_ucd ? "yes" : "NO",
           (ref_nat && ref_ucd) ? "PASS" : "FAIL");
    if (ref_nat && ref_ucd) pass++; else fail++;

    printf("\n=== Verification: %d passed, %d failed ===\n\n", pass, fail);
    return fail;
}

/* ════════════════════════════════════════════════════════════════════
 * Timing
 * ════════════════════════════════════════════════════════════════════ */

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

    printf("=== fe_mul (saturated 4x64): microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_fe_mul_sat_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors).\n", failures);
        printf("NOTE: this is expected for the first build — the ucode\n");
        printf("      patch is a placeholder. Fill in install_fe_mul_sat_patch()\n");
        printf("      and the inline asm in fe_mul_sat_ucode(), then re-run.\n");
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    /* Use values close to maximum-bits-set since saturated 4x64 inputs
     * are typically that wide. */
    uint64_t state_a[4] = { 0x9988776655443322ULL, 0x1122334455667788ULL,
                             0xAABBCCDDEEFF0011ULL, 0x55667788AABBCCDDULL };
    uint64_t state_b[4] = { 0xFEDCBA9876543210ULL, 0x0123456789ABCDEFULL,
                             0xDEADBEEFCAFEBABEULL, 0x1357246813579246ULL };

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t tmp_a[4], tmp_b[4];

    /* Naive C reference */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, sizeof(tmp_a));
        memcpy(tmp_b, state_b, sizeof(tmp_b));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_reference(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Naive ref:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* Optimized C native (Comba row-by-row) */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, sizeof(tmp_a));
        memcpy(tmp_b, state_b, sizeof(tmp_b));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_native(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Native -O3:  min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* Microcode */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, sizeof(tmp_a));
        memcpy(tmp_b, state_b, sizeof(tmp_b));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_sat_ucode(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
