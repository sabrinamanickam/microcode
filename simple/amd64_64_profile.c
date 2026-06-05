/* amd64_64_profile.c — per-op cost profiler for the SUPERCOP amd64-64
 * (4×64 saturated) X25519, mirroring inline2_profile.c so the two are
 * directly comparable.
 *
 * Links the SAME objects the benchmark uses (AMD64_OBJS), via the
 * amd64_64 namespace header (-include). amd64-64 is pure native asm — no
 * microcode, so NO sudo needed; pin with taskset -c 0 for stable RDTSC.
 *
 * Note: amd64-64's fe25519_add / fe25519_sub are inlined inside ladderstep.S,
 * so there are no standalone add/sub to time. We measure mul / square / cswap
 * per-op, then ladderstep / invert / full top-down.
 *
 * Build: make PROG=amd64_64_profile CFLAGS="-O3 -fwrapv -fPIC -fPIE -gdwarf-4 -Wall -march=native -mtune=native -masm=intel -I include/"
 * Run:   taskset -c 0 ./amd64_64_profile_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include "fe25519.h"   /* from AMD64_DIR via -I; macros expand through CRYPTO_NAMESPACE */

/* ladderstep / work_cswap are namespaced locally in mont25519.c; replicate. */
#define ladderstep CRYPTO_NAMESPACE(ladderstep)
#define work_cswap CRYPTO_NAMESPACE(work_cswap)
extern void ladderstep(fe25519 *work);
extern void work_cswap(fe25519 *, unsigned long long);
/* crypto_scalarmult is #defined to x25519_amd64_64 by the namespace header. */
extern int crypto_scalarmult(unsigned char *, const unsigned char *, const unsigned char *);

static inline uint64_t rdtsc_start(void) {
    unsigned lo, hi;
    asm volatile("cpuid\n\trdtsc" : "=a"(lo), "=d"(hi) :: "rbx", "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t rdtsc_end(void) {
    unsigned lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx", "memory");
    uint64_t t = ((uint64_t)hi << 32) | lo;
    asm volatile("cpuid" ::: "rax", "rbx", "rcx", "rdx", "memory");
    return t;
}

static void pin_core0(void) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(0, &s);
    sched_setaffinity(0, sizeof(s), &s);
}

/* deterministic non-trivial fill (SplitMix64) */
static void fill(void *p, size_t n, uint64_t seed) {
    uint64_t *q = p;
    for (size_t i = 0; i < n / 8; i++) {
        seed += 0x9E3779B97F4A7C15ULL;
        uint64_t z = seed;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        q[i] = z ^ (z >> 31);
    }
}

static void hex2bytes(const char *h, uint8_t *o, int n) {
    for (int i = 0; i < n; i++) { unsigned b; sscanf(h + 2 * i, "%2x", &b); o[i] = (uint8_t)b; }
}

#define TIME_FN(LABEL, CALL, ITERS, FTRIALS) do {                           \
    uint64_t _best = ~0ULL;                                                 \
    for (int _t = 0; _t < (FTRIALS); _t++) {                                \
        uint64_t _a = rdtsc_start();                                        \
        for (int _r = 0; _r < (ITERS); _r++) { CALL; }                      \
        uint64_t _c = rdtsc_end() - _a;                                     \
        if (_c < _best) _best = _c;                                         \
    }                                                                       \
    printf("  %-30s %9.2f cyc\n", LABEL, (double)_best / (ITERS));          \
} while (0)

int main(void) {
    pin_core0();
    printf("=== amd64-64 (SUPERCOP, 4x64 saturated) PER-OP PROFILER ===\n\n");

    /* correctness: RFC 7748 first test vector */
    uint8_t scalar[32], point[32], out[32];
    hex2bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex2bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);
    crypto_scalarmult(out, scalar, point);
    static const char *expect = "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552";
    char got[65]; for (int i = 0; i < 32; i++) sprintf(got + 2 * i, "%02x", out[i]);
    printf("RFC 7748: %s\n", strcmp(got, expect) == 0 ? "OK" : "MISMATCH");
    printf("  got %s\n  exp %s\n\n", got, expect);

    fe25519 r, x, work[5];
    fill(&r, sizeof r, 1); fill(&x, sizeof x, 2); fill(work, sizeof work, 3);

    printf("-- Per-op cost (dependent, min) --\n");
    TIME_FN("fe25519_mul (dep)",    fe25519_mul(&r, &r, &x), 50000, 30);
    fill(&r, sizeof r, 1);
    TIME_FN("fe25519_square (dep)", fe25519_square(&r, &r), 50000, 30);
    TIME_FN("work_cswap",           work_cswap(work + 1, (unsigned long long)(_r & 1)), 200000, 30);

    printf("\n-- Top-down: whole-op cost (dependent, min) --\n");
    fill(work, sizeof work, 3);
    TIME_FN("ladderstep (1 full step)", ladderstep(work), 10000, 30);
    fill(&r, sizeof r, 5);
    TIME_FN("fe25519_invert",           fe25519_invert(&r, &r), 1000, 30);
    {
        for (int i = 0; i < 5; i++) crypto_scalarmult(out, scalar, point);  /* warmup */
        uint64_t best = ~0ULL;
        for (int t = 0; t < 300; t++) {
            uint64_t a = rdtsc_start(); crypto_scalarmult(out, scalar, point);
            uint64_t c = rdtsc_end() - a; if (c < best) best = c;
        }
        printf("  %-30s %9llu cyc\n", "x25519 (full)", (unsigned long long)best);
    }

    volatile uint64_t sink = 0;
    sink += r.v[0] ^ work[1].v[0] ^ out[0];
    printf("\n(sink: %llu)\n", (unsigned long long)sink);
    return 0;
}
