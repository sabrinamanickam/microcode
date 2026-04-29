/*
 * asm_op_poly1305.c — fe_sq for poly1305 (2^130-5) via microcode
 *
 * Field: GF(2^130 - 5),  3 limbs,  eval = z[0] + z[1]*2^44 + z[2]*2^87
 * Limb widths: 44, 43, 43 bits
 * Reduction constant: R = 5
 *
 * Products per limb:
 *   c0 = a0*a0     + (2*a1)*(10*a2)    2 MACs, carry@44
 *   c1 = (2*a0)*a1 + a2*(5*a2)         2 MACs, carry@43
 *   c2 = (2*a0)*a2 + a1*(2*a1)         2 MACs, carry@43
 *   Reduce: out[0] += carry * 5, re-propagate [0→1→2]
 *
 * Register convention (caller → microcode):
 *   RDI = a0      RSI = a1      R12 = a2
 *   R15 = 2*a0    R13 = 2*a1    R14 = 5*a2    R11 = 10*a2
 *   RAX = 0       R8 = 0
 *
 * Output: RDI = h0   R9 = h1   R10 = h2
 *
 * Hand-packed: 18 triads (was 33 with templates, 94% slot utilization)
 *
 * Build:  make PROG=asm_op_poly1305
 * Run:    sudo taskset -c 0 ./asm_op_poly1305_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"
#include "ucode_fe_templates.h"

#define MASK44 0xFFFFFFFFFFFULL   /* (1 << 44) - 1 */
#define MASK43 0x7FFFFFFFFFFULL   /* (1 << 43) - 1 */

/* ── fiat-crypto reference (the REAL GCC baseline CryptOpt compares against) ── */
#include "../curvesC/poly1305_square.c"

static void fe_sq_fiat(const uint64_t *a, uint64_t *out) {
    fiat_poly1305_carry_square(out, a);
}

/* ── microcode patch ──────────────────────────────────────────── */

static void install_poly1305_sq_patch(void) {
    ucode_t patch[] = {

    /*
     * Hand-packed: 18 triads (was 33 with templates).
     * Key techniques: MUL srcB/hi RAW saves, double-ADD chains,
     * SHL+SHR mask via 1→2 RAW, cascading carry combine+MUL+ADD.
     *
     * RBX = a1 (precomputed copy in inline asm, preserves RSI for c2)
     */

    /* ═══ LIMB c0 = a0*a0 + (2*a1)*(10*a2)  [W=44] ═══ */

    /* T1: MUL a0² + save lo/hi via srcB/hi RAW */
    { MUL_DSZ64_DRR(RCX, RDI, RDI), ZEROEXT_DSZ64_DR(TMP0, RDI),
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD },

    /* T2: MUL 20*a1*a2 + accumulate lo via srcB RAW + carry */
    { MUL_DSZ64_DRR(RCX, R13, R11), ADD_DSZ64_DRR(TMP0, TMP0, R11),
      SETCC_CONDB_DR(TMP3, TMP0), NOP_SEQWORD },

    /* T3: double-ADD hi (0→1 RAW) + lo-carry extraction */
    { ADD_DSZ64_DRR(TMP1, TMP1, RCX), ADD_DSZ64_DRR(TMP1, TMP1, TMP3),
      SHR_DSZ64_DRI(TMP8, TMP0, 44), NOP_SEQWORD },

    /* T4: hi shift + mask prep + output mask via 1→2 RAW */
    { SHL_DSZ64_DRI(TMP1, TMP1, 20), SHL_DSZ64_DRI(TMP9, TMP0, 20),
      SHR_DSZ64_DRI(RDI, TMP9, 20), NOP_SEQWORD },

    /* ═══ c0→c1: carry combine + MUL (2*a0)*a1 + accumulate ═══ */

    /* T5: OR carry + MUL c1 prod0 + acc via cascading 0→1→2 RAW */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, RBX),
      ADD_DSZ64_DRR(TMP2, TMP0, RBX), NOP_SEQWORD },

    /* T6: capture lo carry + save hi(prod0) with carry */
    { SETCC_CONDB_DR(TMP3, TMP2), ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      NOP, NOP_SEQWORD },

    /* T7: MUL a2*(5*a2) + accumulate via srcB RAW + carry */
    { MUL_DSZ64_DRR(RCX, R12, R14), ADD_DSZ64_DRR(TMP0, TMP2, R14),
      SETCC_CONDB_DR(TMP3, TMP0), NOP_SEQWORD },

    /* T8: double-ADD hi (0→1 RAW) + lo-carry extraction */
    { ADD_DSZ64_DRR(TMP4, TMP4, RCX), ADD_DSZ64_DRR(TMP4, TMP4, TMP3),
      SHR_DSZ64_DRI(TMP8, TMP0, 43), NOP_SEQWORD },

    /* T9: hi shift + mask prep + output mask via 1→2 RAW */
    { SHL_DSZ64_DRI(TMP4, TMP4, 21), SHL_DSZ64_DRI(TMP9, TMP0, 21),
      SHR_DSZ64_DRI(R9, TMP9, 21), NOP_SEQWORD },

    /* ═══ c1→c2: carry combine + MUL (2*a0)*a2 + accumulate ═══ */

    /* T10: OR carry + MUL c2 prod0 + acc via cascading 0→1→2 RAW */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP4), MUL_DSZ64_DRR(RCX, R15, R12),
      ADD_DSZ64_DRR(TMP2, TMP0, R12), NOP_SEQWORD },

    /* T11: capture lo carry + save hi(prod0) with carry */
    { SETCC_CONDB_DR(TMP3, TMP2), ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      NOP, NOP_SEQWORD },

    /* T12: MUL a1*(2*a1) + accumulate via srcB RAW + carry */
    { MUL_DSZ64_DRR(RCX, RSI, R13), ADD_DSZ64_DRR(TMP0, TMP2, R13),
      SETCC_CONDB_DR(TMP3, TMP0), NOP_SEQWORD },

    /* T13: double-ADD hi (0→1 RAW) + lo-carry extraction */
    { ADD_DSZ64_DRR(TMP4, TMP4, RCX), ADD_DSZ64_DRR(TMP4, TMP4, TMP3),
      SHR_DSZ64_DRI(TMP8, TMP0, 43), NOP_SEQWORD },

    /* T14: hi shift + mask prep + output mask via 1→2 RAW */
    { SHL_DSZ64_DRI(TMP4, TMP4, 21), SHL_DSZ64_DRI(TMP9, TMP0, 21),
      SHR_DSZ64_DRI(R10, TMP9, 21), NOP_SEQWORD },

    /* ═══ REDUCTION: carry*5 → out[0], re-propagate ═══ */

    /* T15: carry combine + MUL×5 + add to out[0] — cascade 0→1→2 RAW */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP4), MUL_DSZ64_DIR(TMP1, 5, TMP0),
      ADD_DSZ64_DRR(RDI, RDI, TMP0), NOP_SEQWORD },

    /* T16: carry@44 + mask prep + propagate to out[1] via 0→2 RAW */
    { SHR_DSZ64_DRI(TMP0, RDI, 44), SHL_DSZ64_DRI(TMP9, RDI, 20),
      ADD_DSZ64_DRR(R9, R9, TMP0), NOP_SEQWORD },

    /* T17: mask out[0] + carry@43 + mask prep out[1] */
    { SHR_DSZ64_DRI(RDI, TMP9, 20), SHR_DSZ64_DRI(TMP0, R9, 43),
      SHL_DSZ64_DRI(TMP9, R9, 21), NOP_SEQWORD },

    /* T18: mask out[1] + propagate to out[2] */
    { SHR_DSZ64_DRI(R9, TMP9, 21), ADD_DSZ64_DRR(R10, R10, TMP0),
      NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("poly1305_sq patch installed: %d triads at U7c00\n",
           (int)ARRAY_SZ(patch));
}

/* ── fe_sq via microcode ─────────────────────────────────────── */

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    register uint64_t *_in  asm("rcx") = (uint64_t *)a;
    register uint64_t *_out asm("r15") = out;

    asm volatile(
        "push r15\n\t"

        /* load 3 limbs */
        "mov rdi, [rcx]\n\t"
        "mov rsi, [rcx + 8]\n\t"
        "mov r12, [rcx + 16]\n\t"

        /* precompute */
        "lea r15, [rdi + rdi]\n\t"       /* 2*a0 */
        "lea r13, [rsi + rsi]\n\t"       /* 2*a1 */
        "mov rbx, rsi\n\t"              /* copy a1 (RSI preserved for c2) */
        "imul r14, r12, 5\n\t"           /* 5*a2 */
        "lea r11, [r14 + r14]\n\t"       /* 10*a2 */

        /* fire microcode */
        "vmwrite rcx, rdx\n\t"

        /* store results */
        "pop rcx\n\t"
        "mov [rcx],      rdi\n\t"
        "mov [rcx + 8],  r9\n\t"
        "mov [rcx + 16], r10\n\t"

        : "+r"(_in), "+r"(_out)
        :
        : "rax", "rbx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14",
          "memory", "cc"
    );
}

/* ── fe_sq native C ──────────────────────────────────────────── */

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    uint64_t a0 = a[0], a1 = a[1], a2 = a[2];
    uint64_t r2 = a2 * 5;

    __uint128_t c0 = (__uint128_t)a0*a0 + (__uint128_t)a1*(r2*4);
    __uint128_t c1 = (__uint128_t)(2*a0)*a1 + (__uint128_t)a2*r2;
    __uint128_t c2 = (__uint128_t)(2*a0)*a2 + (__uint128_t)a1*(2*a1);

    uint64_t carry;
    carry = (uint64_t)(c0 >> 44); out[0] = (uint64_t)c0 & MASK44;
    c1 += carry;
    carry = (uint64_t)(c1 >> 43); out[1] = (uint64_t)c1 & MASK43;
    c2 += carry;
    carry = (uint64_t)(c2 >> 43); out[2] = (uint64_t)c2 & MASK43;
    out[0] += carry * 5;
    carry = out[0] >> 44; out[0] &= MASK44;
    out[1] += carry;
    carry = out[1] >> 43; out[1] &= MASK43;
    out[2] += carry;
}

/* ── independent reference (big-integer square mod p) ────────── */

static void fe_sq_reference(const uint64_t *a, uint64_t *out) {
    /*
     * Truly independent: convert limbs → flat 192-bit integer,
     * schoolbook square → 384-bit result, reduce mod 2^130-5,
     * convert back to limbs.
     *
     * NOTE: the naive polynomial schoolbook (t[i+j] += a[i]*a[j])
     * does NOT work here because poly1305 has non-uniform limb widths
     * (44/43/43).  a1*a1 lands at bit 88 but limb 2 starts at bit 87.
     */

    /* 1. Limbs → flat 3×64 integer: val = a0 + a1*2^44 + a2*2^87 */
    uint64_t v[3];
    __uint128_t acc = (__uint128_t)a[0] + ((__uint128_t)a[1] << 44);
    v[0] = (uint64_t)acc;
    acc = (acc >> 64) + ((__uint128_t)a[2] << 23);   /* 87 - 64 = 23 */
    v[1] = (uint64_t)acc;
    v[2] = (uint64_t)(acc >> 64);

    /* 2. Schoolbook square: v[0..2] → r[0..5] (384 bits) */
    __uint128_t rr[6] = {0};
    for (int i = 0; i < 3; i++) {
        __uint128_t carry = 0;
        for (int j = 0; j < 3; j++) {
            __uint128_t prod = (__uint128_t)v[i] * v[j]
                             + (uint64_t)rr[i+j] + carry;
            rr[i+j] = (uint64_t)prod;
            carry = prod >> 64;
        }
        rr[i+3] += carry;
    }
    uint64_t r[6];
    for (int k = 0; k < 6; k++) r[k] = (uint64_t)rr[k];

    /* 3. Reduce mod p = 2^130-5 (two passes) */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t lo2 = r[2] & 0x3ULL;
        uint64_t h[4];
        h[0] = (r[2] >> 2) | (r[3] << 62);
        h[1] = (r[3] >> 2) | (r[4] << 62);
        h[2] = (r[4] >> 2) | (r[5] << 62);
        h[3] = r[5] >> 2;
        __uint128_t s = (__uint128_t)r[0] + (__uint128_t)h[0] * 5;
        r[0] = (uint64_t)s; s >>= 64;
        s += (__uint128_t)r[1] + (__uint128_t)h[1] * 5;
        r[1] = (uint64_t)s; s >>= 64;
        s += (__uint128_t)lo2 + (__uint128_t)h[2] * 5;
        r[2] = (uint64_t)s; s >>= 64;
        s += (__uint128_t)h[3] * 5;
        r[3] = (uint64_t)s;
        r[4] = r[5] = 0;
    }

    /* 4. Flat → limbs (44 / 43 / 43) */
    acc = (__uint128_t)r[0] | ((__uint128_t)r[1] << 64);
    out[0] = (uint64_t)acc & MASK44;  acc >>= 44;
    out[1] = (uint64_t)acc & MASK43;  acc >>= 43;
    acc += (__uint128_t)r[2] << (128 - 87);   /* r[2] at bit 128, limb 2 at bit 87 */
    out[2] = (uint64_t)acc & MASK43;

    /* Final carry wrap */
    uint64_t carry = (uint64_t)(acc >> 43);
    out[0] += carry * 5;
    carry = out[0] >> 44; out[0] &= MASK44;
    out[1] += carry;
    carry = out[1] >> 43; out[1] &= MASK43;
    out[2] += carry;
}

/* ── verification ────────────────────────────────────────────── */

/* Normalize limbs so outputs from different carry depths are comparable.
 * After this, equivalent field elements have identical limb values. */
static void fe_carry(uint64_t h[3]) {
    uint64_t c;
    c = h[0] >> 44; h[0] &= MASK44;
    h[1] += c;
    c = h[1] >> 43; h[1] &= MASK43;
    h[2] += c;
    c = h[2] >> 43; h[2] &= MASK43;
    h[0] += c * 5;
    c = h[0] >> 44; h[0] &= MASK44;
    h[1] += c;
}

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

typedef struct {
    const char *label;
    uint64_t    input[3];
    uint64_t    expected[3];
    int         has_expected;
} test_vec_t;

/*
 * Hand-verified expected outputs:
 *   0² = 0
 *   1² = 1
 *   9² = 81
 *   (2^44)² = 2^88 = 2^87 * 2 → {0, 0, 2}
 *   (2^87)² = 2^174 ≡ 2^174 mod (2^130-5)
 *     2^174 = 2^130 * 2^44 ≡ 5 * 2^44
 *     In limbs: {0, 5, 0}  (since limb1 = 5 * 2^44 / 2^44 = 5)
 */
static const test_vec_t test_vectors[] = {
    { "zero",   {0, 0, 0},  {0, 0, 0}, 1 },
    { "one",    {1, 0, 0},  {1, 0, 0}, 1 },
    { "nine",   {9, 0, 0},  {81, 0, 0}, 1 },
    { "2^44",   {0, 1, 0},  {0, 0, 2}, 1 },
    { "2^87",   {0, 0, 1},  {0, 5, 0}, 1 },
    { "1+2^44", {1, 1, 0},  {1, 2, 2}, 1 },
    { "max44",  {MASK44, 0, 0}, {0}, 0 },
    { "all_max",{MASK44, MASK43, MASK43}, {0}, 0 },
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[3], nat[3], ucd[3];
    fe_sq_reference(t->input, ref);
    fe_sq_native(t->input, nat);
    fe_sq_ucode(t->input, ucd);
    /* normalize so different carry depths produce identical limbs */
    fe_carry(ref); fe_carry(nat); fe_carry(ucd);

    int ok = 1;
    if (memcmp(ref, nat, 24) != 0) {
        printf("  FAIL [%s] native != reference\n", t->label);
        for (int i = 0; i < 3; i++)
            printf("    [%d] native=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, nat[i], ref[i], nat[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (memcmp(ref, ucd, 24) != 0) {
        printf("  FAIL [%s] ucode != reference\n", t->label);
        for (int i = 0; i < 3; i++)
            printf("    [%d] ucode=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, ucd[i], ref[i], ucd[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (t->has_expected && memcmp(ref, t->expected, 24) != 0) {
        printf("  FAIL [%s] result != expected\n", t->label);
        for (int i = 0; i < 3; i++)
            printf("    [%d] got=%016" PRIx64 " exp=%016" PRIx64 " %s\n",
                   i, ref[i], t->expected[i],
                   ref[i] == t->expected[i] ? "" : "***");
        ok = 0;
    }
    if (ok) printf("  PASS [%s]\n", t->label);
    return ok;
}

static int verify_random_quiet(const uint64_t in[3]) {
    uint64_t ref[3], nat[3], ucd[3];
    fe_sq_reference(in, ref);
    fe_sq_native(in, nat);
    fe_sq_ucode(in, ucd);
    fe_carry(ref); fe_carry(nat); fe_carry(ucd);
    if (memcmp(ref, nat, 24) != 0 || memcmp(ref, ucd, 24) != 0) {
        printf("  FAIL random: in={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64 "}\n",
               in[0], in[1], in[2]);
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
    uint64_t rng = 0xCAFEBABE13371337ULL;
    int rpass = 0;
    for (int i = 0; i < RANDOM_TESTS; i++) {
        uint64_t in[3];
        in[0] = splitmix64(&rng) & MASK44;
        in[1] = splitmix64(&rng) & MASK43;
        in[2] = splitmix64(&rng) & MASK43;
        if (verify_random_quiet(in)) rpass++;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    printf("\n--- Iterated chain (%d sq) ---\n", CHAIN_ITERS);
    uint64_t bp[3] = { 0x3FFULL, 0x1ULL, 0x7ULL };
    uint64_t ri[3], ni[3], ui[3];
    memcpy(ri, bp, 24); memcpy(ni, bp, 24); memcpy(ui, bp, 24);
    for (int i = 0; i < CHAIN_ITERS; i++) {
        fe_sq_reference(ri, ri);
        fe_sq_native(ni, ni);
        fe_sq_ucode(ui, ui);
    }
    fe_carry(ri); fe_carry(ni); fe_carry(ui);
    int ref_nat = memcmp(ri, ni, 24) == 0;
    int ref_ucd = memcmp(ri, ui, 24) == 0;
    printf("  ref==native: %s   ref==ucode: %s   -> %s\n",
           ref_nat ? "yes" : "NO", ref_ucd ? "yes" : "NO",
           (ref_nat && ref_ucd) ? "PASS" : "FAIL");
    if (ref_nat && ref_ucd) {
        pass++;
    } else {
        fail++;
        printf("  reference:"); for (int i=0;i<3;i++) printf(" %016" PRIx64, ri[i]); printf("\n");
        printf("  native:   "); for (int i=0;i<3;i++) printf(" %016" PRIx64, ni[i]); printf("\n");
        printf("  ucode:    "); for (int i=0;i<3;i++) printf(" %016" PRIx64, ui[i]); printf("\n");
    }

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

    printf("=== fe_sq poly1305: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_poly1305_sq_patch();

    /* ── verification ──────────────────────────────────────────── */
    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    /* ── benchmark ────────────────────────────────────────────── */
    uint64_t state[3] = { 0xABCDEF012345ULL & MASK44,
                          0x123456789ABULL & MASK43,
                          0x7654321FEDCULL & MASK43 };

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t tmp[3];

    /* native C -O3 */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_native(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Naive -O3:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* fiat-crypto (the real GCC baseline CryptOpt compares against) */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_fiat(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Fiat-crypto: min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* microcode */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_ucode(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* clean up */
    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
