/*
 * probe_mula24.c — staged hardware validation + timing of the fused
 * z2 = E*(AA + 121665*E) firing (-DMUL_A24, see install_field_patches).
 *
 * What is new on hardware, and nothing else:
 *   - a one-triad check at the vmwrite entry (XOR R8^MARK + forward UJMPCC
 *     CONDZ), the idiom the loop probes proved, now in front of fe_mul;
 *   - fe_mul relocated one triad later;
 *   - the 16-triad mul121665 prologue + SEQ_GOTO0 back to the check.
 * ucode_sim (mula24) passes 3000/3000; this checks the real thing.
 *
 * Stages escalate and each is fsync-logged to probe_mula24.log BEFORE it
 * runs, so a hang names its stage after a reboot:
 *   10 install            20 one normal fe_mul     21 1000 normal fe_mul
 *   30 one fused firing   31 edge inputs           32 10000 fused + normal mixed
 *   40 RFC 7748 via the fused ladder (vector 1, iterated x1000)
 *   50 timing: ladder_step fused vs native tail, x25519 fused vs native tail
 *      vs OpenSSL, same process, interleaved rounds
 *
 * Builds (rename after each):
 *   serial fe_sq: make PROG=tests/probe_mula24 EXTRA_CPPFLAGS="-DMUL_A24 -DSQ_SERIAL"
 *   5acc fe_sq  : make PROG=tests/probe_mula24 EXTRA_CPPFLAGS="-DMUL_A24"   (5acc is the default)
 * The 5acc build fires only the inline x25519/ladder/invert, never a chained
 * fe_sq (x25519_ucode, fe_sq_ucode_n), which resets the machine.
 *
 * Run: sudo taskset -c 0 ./tests/probe_mula24_<variant>_static
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>

#ifndef MUL_A24
#error "build with -DMUL_A24"
#endif
#if defined(ENABLE_SQ_5ACC) && !(defined(SQ_UNCHAINED) && defined(SQ_MASK_R8))
#error "5acc fe_sq needs -DSQ_UNCHAINED -DSQ_MASK_R8"
#endif
#ifdef ENABLE_SQ_5ACC
#define VARIANT "MUL_A24 + 5acc fe_sq (unchained inversion)"
#else
#define VARIANT "MUL_A24 + serial fe_sq"
#endif

static int g_log = -1;
static void stage(const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    if (g_log >= 0) { (void)!write(g_log, buf, n); fsync(g_log); }
    (void)!write(1, buf, n);
}

/* ── reference: 5x51 multiply, canonical compare ─────────────────── */
typedef unsigned __int128 u128;
static void ref_mul(uint64_t h[5], const uint64_t a[5], const uint64_t b[5]) {
    u128 t[5] = {0};
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++) {
            u128 p = (u128)a[i] * b[j];
            if (i + j >= 5) t[i + j - 5] += 19 * p; else t[i + j] += p;
        }
    u128 c = 0;
    for (int k = 0; k < 5; k++) { t[k] += c; h[k] = (uint64_t)t[k] & MASK51; c = t[k] >> 51; }
    u128 h0 = (u128)h[0] + 19 * c;
    h[0] = (uint64_t)h0 & MASK51; h[1] += (uint64_t)(h0 >> 51);
}
static void ref_a24(uint64_t h[5], const uint64_t e[5], const uint64_t aa[5]) {
    /* b = aa + 121665*e, reduced so the reference multiply stays exact */
    u128 t[5]; uint64_t b[5];
    for (int j = 0; j < 5; j++) t[j] = (u128)aa[j] + (u128)121665 * e[j];
    u128 c = 0;
    for (int k = 0; k < 5; k++) { t[k] += c; b[k] = (uint64_t)t[k] & MASK51; c = t[k] >> 51; }
    b[0] += (uint64_t)(19 * c);
    ref_mul(h, e, b);
}
static int same(const uint64_t x[5], const uint64_t y[5]) {
    uint8_t p[32], q[32];
    fe_tobytes(p, x); fe_tobytes(q, y);
    return !memcmp(p, q, 32);
}

static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static void rand_fe(uint64_t v[5], int bits) {
    for (int i = 0; i < 5; i++) v[i] = rnd() & ((1ULL << bits) - 1);
}

#define PCLOB "rax","rbx","rcx","rdx","rsi","rdi", \
              "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"

/* single firings on a scratch ladder_state: a/e at X1, b/aa at X2, out X3 */
static ladder_state_t g_io;
static void fire_mul(uint64_t o[5], const uint64_t a[5], const uint64_t b[5]) {
    memcpy(g_io.x1, a, 40); memcpy(g_io.x2, b, 40);
    register ladder_state_t *p asm("rbp") = &g_io;
    asm volatile(FE_MUL(X3_OFF, X1_OFF, X2_OFF) : : "r"(p) : PCLOB);
    memcpy(o, g_io.x3, 40);
}
static void fire_a24(uint64_t o[5], const uint64_t e[5], const uint64_t aa[5]) {
    memcpy(g_io.x1, e, 40); memcpy(g_io.x2, aa, 40);
    register ladder_state_t *p asm("rbp") = &g_io;
    asm volatile(FE_MUL_A24(X3_OFF, X1_OFF, X2_OFF) : : "r"(p) : PCLOB);
    memcpy(o, g_io.x3, 40);
}

/* ── the pre-fusion ladder step, under the same patch layout ──────────
 * Main block copied verbatim from ladder_step; tail is the old native
 * mul121665 + add + FE_MUL_FROM_REGS_A. Keep in step with ladder_step. */
static void ladder_step_ref(ladder_state_t *st) {
    register ladder_state_t *_st asm("rbp") = st;
    asm volatile(
        FE_ADD(A_OFF, X2_OFF, Z2_OFF)
        FE_SQ_FROM_REGS(AA_OFF)
        FE_SUB(B_OFF, X2_OFF, Z2_OFF)
        FE_SQ_FROM_REGS(BB_OFF)
        FE_SUB(E_OFF, AA_OFF, BB_OFF)
        FE_SUB_NOSTORE(X3_OFF, Z3_OFF)
        FE_MUL_FROM_REGS_A(DA_OFF, A_OFF)
        FE_ADD_NOSTORE(X3_OFF, Z3_OFF)
        FE_MUL_FROM_REGS_A(CB_OFF, B_OFF)
        FE_ADD_NOSTORE(DA_OFF, CB_OFF)
        FE_SQ_FROM_REGS(X3_OFF)
        FE_SUB_NOSTORE(DA_OFF, CB_OFF)
        FE_SQ_FROM_REGS(Z3_OFF)
        FE_MUL(Z3_OFF, X1_OFF, Z3_OFF)
        FE_MUL(X2_OFF, AA_OFF, BB_OFF)
        : : "r"(_st) : PCLOB);
    fe_mul121665_native(st->t0, st->E);
    register ladder_state_t *_st2 asm("rbp") = st;
    asm volatile(
        FE_ADD_NOSTORE(AA_OFF, T0_OFF)
        FE_MUL_FROM_REGS_A(Z2_OFF, E_OFF)
        : : "r"(_st2) : PCLOB);
}

/* x25519 driver copied from full_curve25519_inline2.c, step as a parameter,
 * so both tails go through identical code around the ladder. */
static inline __attribute__((always_inline))
void x25519_with(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32],
                 void (*step)(ladder_state_t *)) {
    uint8_t e[32];
    memcpy(e, scalar, 32);
    scalar_clamp(e);
    ladder_state_t st;
    fe_frombytes(st.x1, point);
    memcpy(st.x2, (const uint64_t[]){1, 0, 0, 0, 0}, 40);
    memset(st.z2, 0, 40);
    memcpy(st.x3, st.x1, 40);
    memcpy(st.z3, (const uint64_t[]){1, 0, 0, 0, 0}, 40);
    uint64_t swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (e[pos >> 3] >> (pos & 7)) & 1;
        swap ^= bit;
        fe_cswap(st.x2, st.x3, swap);
        fe_cswap(st.z2, st.z3, swap);
        swap = bit;
        step(&st);
    }
    fe_cswap(st.x2, st.x3, swap);
    fe_cswap(st.z2, st.z3, swap);
    fe_invert(st.z2, st.z2);
    {
        register ladder_state_t *_st asm("rbp") = &st;
        asm volatile(FE_MUL(X2_OFF, X2_OFF, Z2_OFF) : : "r"(_st) : PCLOB);
    }
    fe_tobytes(out, st.x2);
}
static void x_fused(uint8_t *o, const uint8_t *s, const uint8_t *p) { x25519_with(o, s, p, ladder_step); }
static void x_ref(uint8_t *o, const uint8_t *s, const uint8_t *p)   { x25519_with(o, s, p, ladder_step_ref); }
static void x_ossl(uint8_t *o, const uint8_t *s, const uint8_t *p)  { x25519_openssl(o, s, p); }

static const char *SC = "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4";
static const char *PT = "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c";
static const char *EX = "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552";
static const char *IT = "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51";

static int rfc_ok(void (*x)(uint8_t *, const uint8_t *, const uint8_t *)) {
    uint8_t sc[32], pt[32], r[32];
    hex_to_bytes(SC, sc, 32); hex_to_bytes(PT, pt, 32);
    x(r, sc, pt);
    if (memcmp_hex(r, EX, 32)) return 0;
    uint8_t k[32] = {9}, u[32] = {9}, t[32];
    for (int i = 0; i < 1000; i++) { x(t, k, u); memcpy(u, k, 32); memcpy(k, t, 32); }
    return !memcmp_hex(k, IT, 32);
}

static int cmp_t64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
#define RUNS   300
#define ROUNDS 5
static double med_x(void (*f)(uint8_t *, const uint8_t *, const uint8_t *)) {
    static uint64_t t[RUNS];
    uint8_t sc[32], pt[32], o[32];
    hex_to_bytes(SC, sc, 32); hex_to_bytes(PT, pt, 32);
    for (int i = 0; i < 5; i++) f(o, sc, pt);
    for (int i = 0; i < RUNS; i++) {
        uint64_t a = rdtsc_start(); f(o, sc, pt); t[i] = rdtsc_end() - a;
    }
    qsort(t, RUNS, sizeof t[0], cmp_t64);
    return (double)t[RUNS / 2];
}
static ladder_state_t g_ls;
static double time_step(void (*f)(ladder_state_t *)) {
    uint64_t *w = (uint64_t *)&g_ls, best = ~0ULL;
    for (int tr = 0; tr < 30; tr++) {
        for (size_t k = 0; k < sizeof g_ls / 8; k++) w[k] = (0x123456789ABCDULL * (k + 1)) & MASK51;
        uint64_t a = rdtsc_start();
        for (int r = 0; r < 2000; r++) f(&g_ls);
        uint64_t c = rdtsc_end() - a;
        if (c < best) best = c;
    }
    return (double)best / 2000;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (geteuid() != 0) { printf("needs root\n"); return 1; }
    g_log = open("probe_mula24.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    stage("00 %s", VARIANT);
    if (freq_guard()) return 2;
    bench_pin();
    init_match_and_patch();
    do_fix_IN_patch();
    stage("10 about to install the MUL_A24 layout");
    install_field_patches();
    stage("11 installed");

    int bad = 0;
    uint64_t a[5], b[5], got[5], want[5];

    stage("20 about to fire ONE normal fe_mul (check triad must fall through)");
    rand_fe(a, 52); rand_fe(b, 52);
    fire_mul(got, a, b); ref_mul(want, a, b);
    stage("20 survived: %s", same(got, want) ? "correct" : "WRONG");
    if (!same(got, want)) goto out;

    stage("21 about to fire 1000 normal fe_mul, random limbs < 2^54");
    for (int i = 0; i < 1000; i++) {
        rand_fe(a, 54); rand_fe(b, 54);
        fire_mul(got, a, b); ref_mul(want, a, b);
        bad += !same(got, want);
    }
    stage("21 survived, %d wrong", bad);
    if (bad) goto out;

    stage("30 about to fire ONE fused firing (check jumps to prologue, goto back)");
    rand_fe(a, 53); rand_fe(b, 51);
    fire_a24(got, a, b); ref_a24(want, a, b);
    stage("30 survived: %s", same(got, want) ? "correct" : "WRONG");
    if (!same(got, want)) goto out;

    stage("31 about to fire fused edge cases");
    {
        static const uint64_t E[][5] = {
            {0, 0, 0, 0, 0}, {1, 0, 0, 0, 0},
            {MASK51, MASK51, MASK51, MASK51, MASK51},
            {(1ULL << 54) - 1, (1ULL << 54) - 1, (1ULL << 54) - 1, (1ULL << 54) - 1, (1ULL << 54) - 1},
        };
        static const uint64_t A[][5] = {
            {0, 0, 0, 0, 0}, {(1ULL << 52) - 1, (1ULL << 52) - 1, (1ULL << 52) - 1, (1ULL << 52) - 1, (1ULL << 52) - 1},
        };
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 2; j++) {
                fire_a24(got, E[i], A[j]); ref_a24(want, E[i], A[j]);
                if (!same(got, want)) { bad++; stage("31 WRONG at E%d A%d", i, j); }
            }
    }
    stage("31 survived, %d wrong", bad);
    if (bad) goto out;

    stage("32 about to fire 10000 fused + 10000 normal, interleaved");
    for (int i = 0; i < 10000; i++) {
        rand_fe(a, 54); rand_fe(b, 52);
        fire_a24(got, a, b); ref_a24(want, a, b);
        bad += !same(got, want);
        fire_mul(got, a, b); ref_mul(want, a, b);
        bad += !same(got, want);
    }
    stage("32 survived, %d wrong", bad);
    if (bad) goto out;

    stage("40 about to run RFC 7748 (vector 1 + iterated x1000) through the fused ladder");
    int rf = rfc_ok(x_fused), rr = rfc_ok(x_ref), rp = rfc_ok(x25519);
    stage("40 fused %s, native-tail %s, production x25519 %s",
          rf ? "OK" : "FAIL", rr ? "OK" : "FAIL", rp ? "OK" : "FAIL");
    if (!rf || !rr || !rp) { bad++; goto out; }

    stage("50 timing");
    double sf = 0, sr = 0, xf[ROUNDS], xr[ROUNDS], xo[ROUNDS];
    sf = time_step(ladder_step); sr = time_step(ladder_step_ref);
    for (int i = 0; i < ROUNDS; i++) { xf[i] = med_x(x_fused); xr[i] = med_x(x_ref); xo[i] = med_x(x_ossl); }
    double bf = xf[0], br = xr[0], bo = xo[0];
    for (int i = 1; i < ROUNDS; i++) {
        if (xf[i] < bf) bf = xf[i];
        if (xr[i] < br) br = xr[i];
        if (xo[i] < bo) bo = xo[i];
    }
    printf("\n-- %s --\n", VARIANT);
    printf("  ladder_step: fused %.1f   native tail %.1f   saved %.1f cyc/step (x255 = %.0f)\n",
           sf, sr, sr - sf, 255 * (sr - sf));
    printf("  X25519 medians per round (TSC):\n");
    printf("    fused      "); for (int i = 0; i < ROUNDS; i++) printf(" %8.0f", xf[i]); printf("\n");
    printf("    native tail"); for (int i = 0; i < ROUNDS; i++) printf(" %8.0f", xr[i]); printf("\n");
    printf("    openssl    "); for (int i = 0; i < ROUNDS; i++) printf(" %8.0f", xo[i]); printf("\n");
    printf("  best median: fused %.0f  native tail %.0f  openssl %.0f\n", bf, br, bo);
    printf("  fused vs native tail %+.0f;  openssl/fused = %.4f (>1 = we win)\n", bf - br, bo / bf);
    stage("51 timing done");

    {
        int ok = rfc_ok(x_fused);
        stage("52 RFC through fused ladder after timing: %s", ok ? "OK" : "FAIL");
        bad += !ok;
    }
out:
    init_match_and_patch();
    do_fix_IN_patch();
    stage("99 microcode restored; verdict %s", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
