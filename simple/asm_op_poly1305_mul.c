/*
 * asm_op_poly1305_mul.c — fe_mul for poly1305 (2^130-5) via microcode
 *
 * Field: GF(2^130 - 5),  3 limbs,  eval = z[0] + z[1]*2^44 + z[2]*2^87
 * Limb widths: 44, 43, 43 bits
 * Reduction constant: R = 5
 *
 * Products per limb:
 *   c0 = a0*b0     + a1*(10*b2) + a2*(10*b1)   3 MACs, carry@44
 *   c1 = a0*b1     + a1*b0      + a2*(5*b2)    3 MACs, carry@43
 *   c2 = a0*b2     + a1*(2*b1)  + a2*b0        3 MACs, carry@43
 *   Reduce: out[0] += carry * 5, re-propagate [0→1→2]
 *
 * Register convention (caller → microcode):
 *   RDI = a0      RSI = a1      R12 = a2
 *   R15 = b0      R13 = b1      R11 = b2
 *   R10 = 10*b2   R9 = 10*b1    R14 = 5*b2   RBX = 2*b1
 *   RAX = 0       R8 = 0        RDX = b0 (copy for first MUL)
 *
 * Output: R10 = h0   R9 = h1   RDI = h2
 *
 * Estimated: ~39 triads
 *
 * Build:  make PROG=asm_op_poly1305_mul
 * Run:    sudo taskset -c 0 ./asm_op_poly1305_mul_static
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

/* ── microcode patch ──────────────────────────────────────────── */

static void install_fe_mul_patch(void) {
    ucode_t patch[] = {

    /* ═══ LIMB c0 = a0*b0 + a1*(10*b2) + a2*(10*b1)  [W=44, S=20] ═══ */

    /* RDX already = b0 (set by caller).  MAC_HEAD: a0 * b0 */
    MAC_HEAD(RDI, RDX),
    /* a1 * 10*b2  (R10 = 10*b2, last use → consumed) */
    MAC_NEXT(TMP4, RSI, R10, TMP2, TMP0),
    /* a2 * 10*b1  (R9 = 10*b1, last use → consumed) */
    MAC_NEXT(TMP5, R12, R9, TMP0, TMP2),
    /* extract carry@44, output → R10  (reuse freed register) */
    MAC_TAIL_3(TMP2, R10, 44, 20),

    /* ═══ c0 → c1 transition [S=20] ═══
     * c1 first product: a0*b1.  R13=b1, last use → direct srcB.
     * Prep: copy b0 → RDX for c1 product 2 (a1*b0). */
    LIMB_LINK(20, RDI, R13, ZEROEXT_DSZ64_DR(RDX, R15), NOP),

    /* ═══ LIMB c1 = a0*b1 + a1*b0 + a2*(5*b2)  [W=43, S=21] ═══ */

    /* R13 = lo(a0 * b1) from the LINK's MUL */
    MAC_RESUME(R13),
    /* a1 * b0  (RDX = b0, copied in LINK prep) */
    MAC_NEXT(TMP4, RSI, RDX, TMP2, TMP0),
    /* a2 * 5*b2  (R14 = 5*b2, last use → consumed) */
    MAC_NEXT(TMP5, R12, R14, TMP0, TMP2),
    /* extract carry@43, output → R9  (reuse freed register) */
    MAC_TAIL_3(TMP2, R9, 43, 21),

    /* ═══ c1 → c2 transition [S=21] ═══
     * c2 first product: a0*b2.  R11=b2, last use → direct srcB. */
    LIMB_LINK(21, RDI, R11, NOP, NOP),

    /* ═══ LIMB c2 = a0*b2 + a1*(2*b1) + a2*b0  [W=43, S=21] ═══ */

    /* R11 = lo(a0 * b2) from the LINK's MUL */
    MAC_RESUME(R11),
    /* a1 * 2*b1  (RBX = 2*b1, last use → consumed) */
    MAC_NEXT(TMP4, RSI, RBX, TMP2, TMP0),
    /* a2 * b0  (R15 = b0, last use → consumed) */
    MAC_NEXT(TMP5, R12, R15, TMP0, TMP2),
    /* extract carry@43, output → RDI  (a0 no longer needed) */
    MAC_TAIL_3(TMP2, RDI, 43, 21),

    /* ═══ FINAL REDUCTION: carry * 5 → limb0, re-propagate ═══ */

    REDUCE_COMBINE(21),
    REDUCE_MUL(5),
    REDUCE_ADD(R10),
    REPROP(R10, 44, 20, R9),
    REPROP_MASK(R10, 20, NOP_SEQWORD),
    REPROP(R9, 43, 21, RDI),
    REPROP_MASK(R9, 21, END_SEQWORD)

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("poly1305_mul patch installed: %d triads at U7c00\n",
           (int)ARRAY_SZ(patch));
}

/* ── fe_mul via microcode ─────────────────────────────────────── */

static void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_b   asm("rbx") = (uint64_t *)b;
    register uint64_t *_out asm("r15") = out;

    asm volatile(
        "push r15\n\t"

        /* load a[0..2] from rcx */
        "mov rdi, [rcx]\n\t"
        "mov rsi, [rcx + 8]\n\t"
        "mov r12, [rcx + 16]\n\t"

        /* load b[0..2] from rbx */
        "mov r15, [rbx]\n\t"       /* b0 */
        "mov r13, [rbx + 8]\n\t"   /* b1 */
        "mov r11, [rbx + 16]\n\t"  /* b2 */

        /* precompute */
        "imul r14, r11, 5\n\t"     /* 5*b2 */
        "lea r10, [r14 + r14]\n\t" /* 10*b2 */
        "imul r9, r13, 10\n\t"     /* 10*b1 */
        "lea rbx, [r13 + r13]\n\t" /* 2*b1  (clobbers b pointer, must be last) */

        /* clear accumulators */
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* copy b0 → RDX for first MUL (a0*b0) */
        "mov rdx, r15\n\t"

        /* fire microcode */
        "vmwrite rcx, rdx\n\t"

        /* store results: R10=h0, R9=h1, RDI=h2 */
        "pop rcx\n\t"
        "mov [rcx],      r10\n\t"
        "mov [rcx + 8],  r9\n\t"
        "mov [rcx + 16], rdi\n\t"

        : "+r"(_a), "+r"(_b), "+r"(_out)
        :
        : "rax", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14",
          "memory", "cc"
    );
}

/* ── fe_mul native C ──────────────────────────────────────────── */

static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t a0=a[0], a1=a[1], a2=a[2];
    uint64_t b0=b[0], b1=b[1], b2=b[2];
    uint64_t r2 = b2 * 5;

    __uint128_t c0 = (__uint128_t)a0*b0 + (__uint128_t)a1*(r2*2) + (__uint128_t)a2*(b1*10);
    __uint128_t c1 = (__uint128_t)a0*b1 + (__uint128_t)a1*b0     + (__uint128_t)a2*r2;
    __uint128_t c2 = (__uint128_t)a0*b2 + (__uint128_t)a1*(2*b1) + (__uint128_t)a2*b0;

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

/* ── independent reference (big-integer multiply mod p) ────────── */

static void fe_mul_reference(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    /* 1. Limbs → flat 3×64: val = v0 + v1*2^44 + v2*2^87 */
    uint64_t va[3], vb[3];
    __uint128_t acc;

    acc = (__uint128_t)a[0] + ((__uint128_t)a[1] << 44);
    va[0] = (uint64_t)acc;
    acc = (acc >> 64) + ((__uint128_t)a[2] << 23);
    va[1] = (uint64_t)acc;
    va[2] = (uint64_t)(acc >> 64);

    acc = (__uint128_t)b[0] + ((__uint128_t)b[1] << 44);
    vb[0] = (uint64_t)acc;
    acc = (acc >> 64) + ((__uint128_t)b[2] << 23);
    vb[1] = (uint64_t)acc;
    vb[2] = (uint64_t)(acc >> 64);

    /* 2. Schoolbook multiply: va × vb → r[0..5] */
    __uint128_t rr[6] = {0};
    for (int i = 0; i < 3; i++) {
        __uint128_t carry = 0;
        for (int j = 0; j < 3; j++) {
            __uint128_t prod = (__uint128_t)va[i] * vb[j]
                             + (uint64_t)rr[i+j] + carry;
            rr[i+j] = (uint64_t)prod;
            carry = prod >> 64;
        }
        rr[i+3] += carry;
    }
    uint64_t r[6];
    for (int k = 0; k < 6; k++) r[k] = (uint64_t)rr[k];

    /* 3. Reduce mod p = 2^130-5 */
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

    /* 4. Flat → limbs (44/43/43) */
    acc = (__uint128_t)r[0] | ((__uint128_t)r[1] << 64);
    out[0] = (uint64_t)acc & MASK44;  acc >>= 44;
    out[1] = (uint64_t)acc & MASK43;  acc >>= 43;
    acc += (__uint128_t)r[2] << (128 - 87);
    out[2] = (uint64_t)acc & MASK43;

    uint64_t carry = (uint64_t)(acc >> 43);
    out[0] += carry * 5;
    carry = out[0] >> 44; out[0] &= MASK44;
    out[1] += carry;
    carry = out[1] >> 43; out[1] &= MASK43;
    out[2] += carry;
}

/* ── verification ────────────────────────────────────────────── */

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
    uint64_t    a[3];
    uint64_t    b[3];
    uint64_t    expected[3];
    int         has_expected;
} test_vec_t;

static const test_vec_t test_vectors[] = {
    { "0*0",     {0,0,0}, {0,0,0}, {0,0,0}, 1 },
    { "1*1",     {1,0,0}, {1,0,0}, {1,0,0}, 1 },
    { "0*1",     {0,0,0}, {1,0,0}, {0,0,0}, 1 },
    { "2*3",     {2,0,0}, {3,0,0}, {6,0,0}, 1 },
    { "9*9",     {9,0,0}, {9,0,0}, {81,0,0}, 1 },
    { "2^44*2^44",  {0,1,0}, {0,1,0}, {0,0,2}, 1 },
    { "2^87*2^44",  {0,0,1}, {0,1,0}, {10,0,0}, 1 },
    { "max*max", {MASK44,MASK43,MASK43}, {MASK44,MASK43,MASK43}, {0}, 0 },
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[3], nat[3], ucd[3];
    fe_mul_reference(t->a, t->b, ref);
    fe_mul_native(t->a, t->b, nat);
    fe_mul_ucode(t->a, t->b, ucd);
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

static int verify_random_quiet(const uint64_t a[3], const uint64_t b[3]) {
    uint64_t ref[3], nat[3], ucd[3];
    fe_mul_reference(a, b, ref);
    fe_mul_native(a, b, nat);
    fe_mul_ucode(a, b, ucd);
    fe_carry(ref); fe_carry(nat); fe_carry(ucd);
    if (memcmp(ref, nat, 24) != 0 || memcmp(ref, ucd, 24) != 0) {
        printf("  FAIL random: a={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64 "}\n",
               a[0], a[1], a[2]);
        printf("           b={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64 "}\n",
               b[0], b[1], b[2]);
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
        uint64_t a[3], b[3];
        a[0] = splitmix64(&rng) & MASK44;
        a[1] = splitmix64(&rng) & MASK43;
        a[2] = splitmix64(&rng) & MASK43;
        b[0] = splitmix64(&rng) & MASK44;
        b[1] = splitmix64(&rng) & MASK43;
        b[2] = splitmix64(&rng) & MASK43;
        if (verify_random_quiet(a, b)) rpass++;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    printf("\n--- Iterated chain (%d mul-self) ---\n", CHAIN_ITERS);
    uint64_t bp[3] = { 0x3FFULL, 0x1ULL, 0x7ULL };
    uint64_t ri[3], ni[3], ui[3];
    memcpy(ri, bp, 24); memcpy(ni, bp, 24); memcpy(ui, bp, 24);
    for (int i = 0; i < CHAIN_ITERS; i++) {
        uint64_t tmp[3];
        memcpy(tmp, ri, 24); fe_mul_reference(tmp, tmp, ri);
        memcpy(tmp, ni, 24); fe_mul_native(tmp, tmp, ni);
        memcpy(tmp, ui, 24); fe_mul_ucode(tmp, tmp, ui);
    }
    fe_carry(ri); fe_carry(ni); fe_carry(ui);
    int ref_nat = memcmp(ri, ni, 24) == 0;
    int ref_ucd = memcmp(ri, ui, 24) == 0;
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

    printf("=== fe_mul poly1305: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_fe_mul_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    uint64_t sa[3] = { 0xABCDEF012345ULL & MASK44,
                        0x123456789ABULL & MASK43,
                        0x7654321FEDCULL & MASK43 };
    uint64_t sb[3] = { 0x112233445566ULL & MASK44,
                        0x6655443322ULL   & MASK43,
                        0x1F2F3F4F5F6ULL  & MASK43 };

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t ta[3], tb[3];

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, sizeof(ta));
        memcpy(tb, sb, sizeof(tb));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_native(ta, tb, ta);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Native -O3:  min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(ta, sa, sizeof(ta));
        memcpy(tb, sb, sizeof(tb));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_ucode(ta, tb, ta);
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
