/*
 * bench_kernel_4x64.c — LEVEL 1 for the SATURATED 4x64 representation:
 * isolated fe_mul / fe_sq kernel cost, every 4x64 backend, one process.
 *
 * The 4x64 counterpart of bench_kernel.c (5x51). Separate binary because the
 * 75-triad 4x64 mul patch and the 108-triad 5x51 pair are both based at U7c00
 * and cannot coexist in the 128-triad patch RAM.
 *
 * BACKENDS (all 4x64 saturated, radix 2^64):
 *   microcode   fe_mul_ucode4 / fe_sq_ucode4 from full_curve25519_amd64_64_ucode.c
 *               -- the exact wrapper the amd64-64/ucode X25519 calls. The 4x64
 *               patch has no dedicated squarer (a 75-triad multiplier leaves no
 *               room for one), so fe_sq = fe_mul(a, a).
 *   amd64-64    Bernstein-Schwabe qhasm fe25519_mul.S / fe25519_square.S
 *               (the objects linked into stock amd64-64 X25519)
 *   s2n-bignum  bignum_{mul,sqr}_p25519_alt, formally verified assembly (the
 *               _alt variants; the base ones need MULX/ADX, absent here)
 *
 * WHAT IS MEASURED — same method as bench_kernel.c:
 *   latency     dependent memory-to-memory chain; operands ping-pong between
 *               two distinct field slots (no same-address STLF stall)
 *   throughput  four independent chains interleaved
 *   floors      microcode: the same wrapper with a 1-triad no-op patch at the
 *               hook (wrapper + redirection, no arithmetic); native: a
 *               noinline call with the same 8-load / 4-store traffic
 * Sampling: one sample = 128 ops. K_REPS samples of every arm per phase, arms
 * round-robin, K_PHASES phases, all samples pooled; median/min/p10/p90 of the
 * pool are reported.
 *
 * Build: make PROG=bench/bench_kernel_4x64
 * Run:   sudo env BENCH_CORE=1 taskset -c 1 ./bench/bench_kernel_4x64_static [out.txt]
 */
#define _GNU_SOURCE
#define A64U_LIB                        /* library mode: no standalone main() */
#include "full_curve25519_amd64_64_ucode.c"
#include "include/freq_guard.h"
#include <unistd.h>

/* amd64-64 qhasm field ops (fe25519 is struct { unsigned long long v[4]; }). */
extern void supercop_amd64_64_fe25519_mul(uint64_t *r, const uint64_t *x, const uint64_t *y);
extern void supercop_amd64_64_fe25519_square(uint64_t *r, const uint64_t *x);
/* s2n-bignum verified asm, _alt variants. */
extern void bignum_mul_p25519_alt(uint64_t z[4], const uint64_t x[4], const uint64_t y[4]);
extern void bignum_sqr_p25519_alt(uint64_t z[4], const uint64_t x[4]);

/* ── harness geometry (identical to bench_kernel.c) ────────────────── */
#define K_OPS    128
#define K_REPS   50
#define K_PHASES 20
#define K_POOL   (K_REPS * K_PHASES)

/* ── uniform call shapes: F(a, b, out) and S(a, out) ───────────────── */
static inline void mul_uc (const uint64_t *a, const uint64_t *b, uint64_t *o) { fe_mul_ucode4(o, a, b); }
static inline void sq_uc  (const uint64_t *a, uint64_t *o)                    { fe_sq_ucode4(o, a); }
static inline void mul_a64(const uint64_t *a, const uint64_t *b, uint64_t *o) { supercop_amd64_64_fe25519_mul(o, a, b); }
static inline void sq_a64 (const uint64_t *a, uint64_t *o)                    { supercop_amd64_64_fe25519_square(o, a); }
static inline void mul_s2n(const uint64_t *a, const uint64_t *b, uint64_t *o) { bignum_mul_p25519_alt(o, a, b); }
static inline void sq_s2n (const uint64_t *a, uint64_t *o)                    { bignum_sqr_p25519_alt(o, a); }

/* Native invocation floor: same call shape and memory traffic as a 4x64
 * field op (8 loads, 4 stores), no arithmetic. */
__attribute__((noinline, noclone))
static void mul_nfloor(const uint64_t *a, const uint64_t *b, uint64_t *o) {
    uint64_t t0 = a[0]^b[0], t1 = a[1]^b[1], t2 = a[2]^b[2], t3 = a[3]^b[3];
    o[0] = t0; o[1] = t1; o[2] = t2; o[3] = t3;
    asm volatile("" ::: "memory");
}
__attribute__((noinline, noclone))
static void sq_nfloor(const uint64_t *a, uint64_t *o) {
    uint64_t t0 = a[0], t1 = a[1], t2 = a[2], t3 = a[3];
    o[0] = t0; o[1] = t1; o[2] = t2; o[3] = t3;
    asm volatile("" ::: "memory");
}

/* ── operands: ping-pong pairs + four independent chains ───────────── */
static uint64_t xa[4], xb[4], xk[4];
static uint64_t ya[4], yb[4], za[4], zb[4], wa[4], wb[4];

static void init_operands(void) {
    for (int i = 0; i < 4; i++) {
        xa[i] = 0x13579BDF02468ACEULL * (i + 1);
        xb[i] = 0x2468ACE013579BDFULL * (i + 1);
        xk[i] = 0x0FEDCBA987654321ULL * (i + 1);
    }
    xa[3] &= 0x7FFFFFFFFFFFFFFFULL; xb[3] &= 0x7FFFFFFFFFFFFFFFULL;
    xk[3] &= 0x7FFFFFFFFFFFFFFFULL;
    for (int i = 0; i < 4; i++) {
        ya[i] = xa[i] ^ 0x11; yb[i] = xb[i];
        za[i] = xa[i] ^ 0x22; zb[i] = xb[i];
        wa[i] = xa[i] ^ 0x33; wb[i] = xb[i];
    }
}

#define LAT_M(F)  F(xa, xk, xb); F(xb, xk, xa); F(xa, xk, xb); F(xb, xk, xa)
#define LAT_S(F)  F(xa, xb);     F(xb, xa);     F(xa, xb);     F(xb, xa)
#define TPT_M(F)  F(xa, xk, xb); F(ya, xk, yb); F(za, xk, zb); F(wa, xk, wb)
#define TPT_S(F)  F(xa, xb);     F(ya, yb);     F(za, zb);     F(wa, wb)

/* ── canonical reduction mod p = 2^255 - 19 of a 256-bit value ─────── */
static void reduce4(uint64_t r[4], const uint64_t v[4]) {
    uint64_t t[4]; memcpy(t, v, 32);
    for (int pass = 0; pass < 2; pass++) {          /* fold bit 255: 2^255 = 19 */
        uint64_t hi = t[3] >> 63; t[3] &= 0x7FFFFFFFFFFFFFFFULL;
        unsigned __int128 c = (unsigned __int128)t[0] + 19 * hi;
        t[0] = (uint64_t)c; c >>= 64;
        for (int i = 1; i < 4; i++) { c += t[i]; t[i] = (uint64_t)c; c >>= 64; }
    }
    /* now t < 2^255; subtract p once if t >= p */
    int ge = (t[3] == 0x7FFFFFFFFFFFFFFFULL && t[2] == ~0ULL && t[1] == ~0ULL &&
              t[0] >= 0xFFFFFFFFFFFFFFEDULL);
    if (ge) { t[0] -= 0xFFFFFFFFFFFFFFEDULL; t[1] = t[2] = 0; t[3] = 0; }
    memcpy(r, t, 32);
}

/* Every backend against s2n-bignum (formally verified, fully reduced), on
 * random inputs AND on unreduced inputs up to 2^256-1, since the microcode
 * and amd64-64 outputs are < 2^256 but not canonical and get chained. */
static int verify_backends(void) {
    uint64_t a[4], b[4], want[4], got[4], ra[4], rb[4];
    int bad = 0;
    srandom(20260927);
    for (int n = 0; n < 2000; n++) {
        for (int i = 0; i < 4; i++) {
            a[i] = (uint64_t)random() << 33 ^ (uint64_t)random() << 11 ^ (uint64_t)random();
            b[i] = (uint64_t)random() << 33 ^ (uint64_t)random() << 11 ^ (uint64_t)random();
        }
        if (n < 8) for (int i = 0; i < 4; i++) { a[i] = ~0ULL >> (n & 1); b[i] = ~0ULL; }
        mul_s2n(a, b, want); reduce4(rb, want);
        mul_uc (a, b, got);  reduce4(ra, got); if (memcmp(ra, rb, 32)) bad++;
        mul_a64(a, b, got);  reduce4(ra, got); if (memcmp(ra, rb, 32)) bad++;
        sq_s2n(a, want);     reduce4(rb, want);
        sq_uc (a, got);      reduce4(ra, got); if (memcmp(ra, rb, 32)) bad++;
        sq_a64(a, got);      reduce4(ra, got); if (memcmp(ra, rb, 32)) bad++;
    }
    return bad;
}

/* ── pooled per-arm samples ────────────────────────────────────────── */
#define MAX_ARMS 32
typedef struct { const char *label; int n; } arm_t;
static arm_t    g_arm[MAX_ARMS];
static int      g_narms;
static uint64_t g_pool[MAX_ARMS][K_POOL];

static int arm_slot(const char *label) {
    for (int i = 0; i < g_narms; i++) if (g_arm[i].label == label) return i;
    if (g_narms >= MAX_ARMS) { fprintf(stderr, "raise MAX_ARMS\n"); exit(1); }
    g_arm[g_narms].label = label; g_arm[g_narms].n = 0;
    return g_narms++;
}

static int k_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static uint64_t k_pctl(const uint64_t *s, int n, double p) {
    int i = (int)(p / 100.0 * (n - 1) + 0.5);
    return s[i < 0 ? 0 : i >= n ? n - 1 : i];
}
/* med/min/p10/p90 in cycles per op; returns 0 if the arm has no samples. */
static int arm_stats(const char *label, double st[4]) {
    for (int i = 0; i < g_narms; i++) {
        if (g_arm[i].label != label || !g_arm[i].n) continue;
        int n = g_arm[i].n;
        qsort(g_pool[i], n, sizeof(uint64_t), k_cmp);
        st[0] = (double)g_pool[i][n / 2] / K_OPS;
        st[1] = (double)g_pool[i][0] / K_OPS;
        st[2] = (double)k_pctl(g_pool[i], n, 10.0) / K_OPS;
        st[3] = (double)k_pctl(g_pool[i], n, 90.0) / K_OPS;
        return 1;
    }
    return 0;
}
static double arm_med(const char *label) {
    double st[4];
    return arm_stats(label, st) ? st[0] : -1.0;
}

#define BAR asm volatile("" ::: "memory")

/* GROUP is 4 ops; K_OPS/4 groups per sample; BAR forces results to memory. */
#define TIME(LABEL, GROUP) do {                                              \
    int _s = arm_slot(LABEL);                                                \
    for (int q = 0; q < K_REPS; q++) {                                       \
        uint64_t _a = rdtsc_start();                                         \
        for (int _i = 0; _i < K_OPS / 4; _i++) { GROUP; BAR; }               \
        uint64_t _b = rdtsc_end();                                           \
        if (g_arm[_s].n < K_POOL) g_pool[_s][g_arm[_s].n++] = _b - _a;       \
    }                                                                        \
} while (0)

/* 1-triad no-op at the vmwrite hook: wrapper + redirection, no arithmetic. */
static void install_probe_patch(void) {
    ucode_t probe[] = { { ZEROEXT_DSZ64_DR(RDI, RDI), NOP, NOP, END_SEQWORD } };
    patch_ucode(0x7c00, probe, 1);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void row(const char *name, const char *m, const char *s) {
    double a = arm_med(m), b = arm_med(s);
    printf("  %-40s", name);
    if (a >= 0) printf(" %8.1f", a); else printf("      n/a");
    if (b >= 0) printf(" %8.1f", b); else printf("      n/a");
    printf("\n");
}

int main(int argc, char **argv) {
    const char *out_path = argc > 1 ? argv[1] : "bench_kernel_4x64_out.txt";
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== LEVEL 1: isolated fe_mul / fe_sq kernel cost (4x64 saturated) ===\n");
    printf("harness: %d ops/sample, %d samples x %d round-robin phases = %d pooled "
           "samples/arm, ping-pong dependent chain\n\n", K_OPS, K_REPS, K_PHASES, K_POOL);

    bench_pin();
    if (frequency_guard()) return 2;
    if (geteuid() != 0) { printf("needs root (installs the 4x64 microcode patch)\n"); return 1; }

    init_match_and_patch();
    do_fix_IN_patch();
    install_mul_patch();

    {
        int bad = verify_backends();
        printf("microcode and amd64-64 asm vs s2n-bignum: %s (%d mismatches / 8000)\n",
               bad ? "FAIL" : "OK", bad);
        if (bad) { init_match_and_patch(); do_fix_IN_patch(); return 1; }
    }
    if (test_rfc7748()) {
        printf("RFC 7748 FAILED with the 4x64 patch - aborting.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }
    printf("RFC 7748 OK - the patch being timed is the verified one.\n\n");

    for (int ph = 0; ph < K_PHASES; ph++) {
        init_operands();
        TIME("uc4_mul_lat",  LAT_M(mul_uc));
        TIME("uc4_sq_lat",   LAT_S(sq_uc));
        TIME("a64_mul_lat",  LAT_M(mul_a64));
        TIME("a64_sq_lat",   LAT_S(sq_a64));
        TIME("s2n_mul_lat",  LAT_M(mul_s2n));
        TIME("s2n_sq_lat",   LAT_S(sq_s2n));
        TIME("uc4_mul_tput", TPT_M(mul_uc));
        TIME("uc4_sq_tput",  TPT_S(sq_uc));
        TIME("a64_mul_tput", TPT_M(mul_a64));
        TIME("a64_sq_tput",  TPT_S(sq_a64));
        TIME("s2n_mul_tput", TPT_M(mul_s2n));
        TIME("s2n_sq_tput",  TPT_S(sq_s2n));
        TIME("nfloor4_mul",  LAT_M(mul_nfloor));
        TIME("nfloor4_sq",   LAT_S(sq_nfloor));
    }

    /* Dispatch floor: same wrapper, 1-triad no-op patch. fe_sq_ucode4 is
     * fe_mul_ucode4(a, a), so one floor serves both columns. */
    install_probe_patch();
    printf("dispatch-floor probe installed: 1 triad (ZEROEXT rdi,rdi) at the vmwrite hook\n");
    for (int ph = 0; ph < K_PHASES; ph++) {
        init_operands();
        TIME("floor4_mul_lat",  LAT_M(mul_uc));
        TIME("floor4_mul_tput", TPT_M(mul_uc));
    }

    /* Restore the production patch, re-verify, and re-measure one arm as a
     * drift check between the main phases and the floor block. */
    install_mul_patch();
    init_operands();
    TIME("uc4_mul_recheck", LAT_M(mul_uc));
    if (test_rfc7748()) {
        printf("RFC 7748 FAILED after restoring the patch.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    printf("\n=== Table K1 (4x64) - kernel latency, dependent chain ===\n");
    printf("  %-40s %8s %8s\n", "backend", "fe_mul", "fe_sq");
    row("microcode 4x64 (sq = mul(a,a))",   "uc4_mul_lat", "uc4_sq_lat");
    row("amd64-64 qhasm asm",               "a64_mul_lat", "a64_sq_lat");
    row("s2n-bignum verified asm (_alt)",   "s2n_mul_lat", "s2n_sq_lat");
    printf("\n=== floors ===\n");
    row("microcode wrapper + redirection",  "floor4_mul_lat", "floor4_mul_lat");
    row("native call + memory round trip",  "nfloor4_mul",    "nfloor4_sq");
    printf("\n=== Table K3 (4x64) - four independent chains ===\n");
    row("microcode 4x64",                   "uc4_mul_tput", "uc4_sq_tput");
    row("amd64-64 qhasm asm",               "a64_mul_tput", "a64_sq_tput");
    row("s2n-bignum verified asm (_alt)",   "s2n_mul_tput", "s2n_sq_tput");
    {
        double a = arm_med("uc4_mul_lat"), b = arm_med("uc4_mul_recheck");
        if (a > 0 && b > 0)
            printf("\n  drift check: fe_mul re-measured after the floor block = %.1f "
                   "(main %.1f, %+.2f%%)\n", b, a, 100.0 * (b - a) / a);
    }

    FILE *f = fopen(out_path, "w");
    if (f) {
        fprintf(f, "# bench_kernel_4x64 raw arms: label median min p10 p90 (cycles/op, pooled)\n");
        fprintf(f, "# root=1 ops_per_sample=%d samples=%d phases=%d pooled=%d core=%d\n",
                K_OPS, K_REPS, K_PHASES, K_POOL, bench_core());
        for (int i = 0; i < g_narms; i++) {
            double st[4];
            if (arm_stats(g_arm[i].label, st))
                fprintf(f, "%-16s %8.2f %8.2f %8.2f %8.2f\n",
                        g_arm[i].label, st[0], st[1], st[2], st[3]);
        }
        fclose(f);
        printf("\nraw arms written to %s\n", out_path);
    }

    volatile uint64_t sink = 0;
    for (int i = 0; i < 4; i++) sink ^= xa[i]^xb[i]^ya[i]^yb[i]^za[i]^zb[i]^wa[i]^wb[i];
    printf("(checksum %llu)\n", (unsigned long long)sink);

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
