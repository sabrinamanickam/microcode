/*
 * bench_sq5acc.c — X25519 end-to-end: shipped serial fe_sq vs the
 * five-accumulator fe_sq, with OpenSSL as the in-process control.
 *
 * Built twice from this one source:
 *   serial : make PROG=tests/bench_sq5acc EXTRA_CPPFLAGS="-DSQ_SERIAL"
 *            -> old serial fe_sq, register-chained inversion
 *   5acc   : make PROG=tests/bench_sq5acc
 *            -> five-accumulator fe_sq, inversion WITHOUT chained firings
 *               (the default since 2026-09-29)
 * Rename each binary after building (see bench_sq5acc_*_static).
 *
 * The two builds cannot share a process: the serial patch needs R8 = 0 at
 * the trigger and the 5acc patch needs R8 = 2^51-1, and FE_SQ_R8 is a
 * compile-time choice. So each run times OpenSSL too, and the microcode
 * numbers are comparable across the two runs only if OpenSSL's agree
 * (PLAN_kernel_optimization.md 9b methodology note).
 *
 * SAFETY for the 5acc build: the five-accumulator patch resets the machine
 * when fe_sq firings are register-chained (project memory, 2026-09-29). This
 * harness fires ONLY the inline x25519 / ladder_step / fe_invert, which under
 * -DSQ_UNCHAINED contain no chained firing. It must never call x25519_ucode,
 * fe_invert_ucode or fe_sq_ucode_n. main() refuses a 5acc build without
 * SQ_UNCHAINED at compile time.
 *
 * Run: sudo taskset -c 0 ./tests/bench_sq5acc_<variant>_static
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"

#if defined(ENABLE_SQ_5ACC) && !defined(SQ_UNCHAINED)
#error "5acc fe_sq must be built with -DSQ_UNCHAINED (chained firings reset the machine)"
#endif
#if defined(ENABLE_SQ_5ACC) && !defined(SQ_MASK_R8)
#error "5acc fe_sq needs -DSQ_MASK_R8"
#endif

#ifdef ENABLE_SQ_5ACC
#define VARIANT "5acc fe_sq, unchained inversion"
#else
#define VARIANT "serial fe_sq (-DSQ_SERIAL), chained inversion"
#endif

static const char *SC = "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4";
static const char *PT = "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c";
static const char *EX = "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552";

static int cmp_t64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

#define RUNS   300
#define ROUNDS 5
typedef void (*x25519_fn)(uint8_t *, const uint8_t *, const uint8_t *);

/* median and min of RUNS single scalar multiplications */
static void time_x(x25519_fn f, const uint8_t *sc, const uint8_t *pt,
                   double *med, double *mn) {
    static uint64_t t[RUNS];
    uint8_t out[32];
    for (int i = 0; i < 5; i++) f(out, sc, pt);        /* warm up */
    for (int i = 0; i < RUNS; i++) {
        uint64_t a = rdtsc_start();
        f(out, sc, pt);
        t[i] = rdtsc_end() - a;
    }
    qsort(t, RUNS, sizeof t[0], cmp_t64);
    *med = (double)t[RUNS / 2];
    *mn  = (double)t[0];
}

static void x_ours(uint8_t *o, const uint8_t *s, const uint8_t *p) { x25519(o, s, p); }
static void x_ossl(uint8_t *o, const uint8_t *s, const uint8_t *p) { x25519_openssl(o, s, p); }

static ladder_state_t g_ls;
static double time_step(void) {
    uint64_t *w = (uint64_t *)&g_ls, best = ~0ULL;
    for (size_t k = 0; k < sizeof g_ls / 8; k++) w[k] = (0x123456789ABCDULL * (k + 1)) & MASK51;
    for (int t = 0; t < 30; t++) {
        uint64_t a = rdtsc_start();
        for (int r = 0; r < 2000; r++) ladder_step(&g_ls);
        uint64_t c = rdtsc_end() - a;
        if (c < best) best = c;
    }
    return (double)best / 2000;
}
static double time_inv(void) {
    uint64_t z[5], o[5], best = ~0ULL;
    for (int i = 0; i < 5; i++) z[i] = (0x2468ACE13579BULL * (i + 3)) & MASK51;
    for (int t = 0; t < 30; t++) {
        uint64_t a = rdtsc_start();
        for (int r = 0; r < 20; r++) fe_invert(o, z);
        uint64_t c = rdtsc_end() - a;
        if (c < best) best = c;
    }
    return (double)best / 20;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== bench_sq5acc: %s ===\n\n", VARIANT);
    if (freq_guard()) return 2;
    bench_pin();
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();

    uint8_t sc[32], pt[32], r[32];
    hex_to_bytes(SC, sc, 32);
    hex_to_bytes(PT, pt, 32);
    x25519(r, sc, pt);
    int ok = !memcmp_hex(r, EX, 32);
    x25519_openssl(r, sc, pt);
    int ok_o = !memcmp_hex(r, EX, 32);
    printf("RFC 7748 vector 1: ours %s, openssl %s\n", ok ? "OK" : "FAIL", ok_o ? "OK" : "FAIL");
    if (!ok || !ok_o) goto out;
    {   /* iterated RFC test, 1000 rounds, through the inline x25519 only */
        uint8_t k[32] = {9}, u[32] = {9}, t[32];
        for (int i = 0; i < 1000; i++) { x25519(t, k, u); memcpy(u, k, 32); memcpy(k, t, 32); }
        ok = !memcmp_hex(k, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", 32);
        printf("RFC 7748 iterated x1000: %s\n", ok ? "OK" : "FAIL");
        if (!ok) goto out;
    }

    double om[ROUNDS], on[ROUNDS], sm[ROUNDS], sn[ROUNDS];
    for (int i = 0; i < ROUNDS; i++) {
        time_x(x_ours, sc, pt, &om[i], &on[i]);
        time_x(x_ossl, sc, pt, &sm[i], &sn[i]);
    }
    double step = time_step(), inv = time_inv();

    printf("\n-- X25519 (TSC cyc, %d runs/round, %d interleaved rounds) --\n", RUNS, ROUNDS);
    printf("  %-8s", "round");
    for (int i = 0; i < ROUNDS; i++) printf(" %9d", i);
    printf("\n  %-8s", "ours med");
    for (int i = 0; i < ROUNDS; i++) printf(" %9.0f", om[i]);
    printf("\n  %-8s", "ossl med");
    for (int i = 0; i < ROUNDS; i++) printf(" %9.0f", sm[i]);
    double bo = om[0], bs = sm[0], mo = on[0], ms = sn[0];
    for (int i = 1; i < ROUNDS; i++) {
        if (om[i] < bo) bo = om[i];
        if (sm[i] < bs) bs = sm[i];
        if (on[i] < mo) mo = on[i];
        if (sn[i] < ms) ms = sn[i];
    }
    printf("\n\n  best median: ours %.0f   openssl %.0f   ratio openssl/ours = %.4f (>1 = we win)\n",
           bo, bs, bs / bo);
    printf("  min        : ours %.0f   openssl %.0f\n", mo, ms);
    printf("  ladder_step %.1f cyc   fe_invert %.1f cyc\n", step, inv);
    printf("  (compare the two builds only if their openssl medians agree)\n");

    x25519(r, sc, pt);
    printf("\nRFC 7748 vector 1 after timing: %s\n", memcmp_hex(r, EX, 32) ? "FAIL" : "OK");
out:
    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
