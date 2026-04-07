/*
 * bench_sq_compare.c — Correctness + performance: CryptOpt vs MAC128 vs reference
 *
 * ═══════════════════════════════════════════════════════════════════
 *  Compares three implementations of curve25519 field square:
 *
 *    1. fe_sq_ref()     — C reference (__uint128_t), always available
 *    2. fe_sq_cryptopt()— CryptOpt native asm (fiat_curve25519_carry_square)
 *                         Requires BMI2 — will SIGILL on Goldmont.
 *    3. fe_sq_mac128()  — Microcode MAC128 via vmwrite hook
 *                         Requires Red-Unlocked NUC + root.
 *
 *  Build (NUC):
 *    nasm -f elf64 -o cryptopt_sq.o seed0001771325517180_ratio11203.asm
 *    gcc -static -g -O0 -masm=intel -march=x86-64-v2 \
 *        -I../../include bench_sq_compare.c cryptopt_sq.o \
 *        -o bench_sq_compare_static
 *
 *  Run:
 *    sudo taskset -c 0 ./bench_sq_compare_static
 *
 *  Set RUN_CRYPTOPT=0 at compile time to skip CryptOpt on Goldmont:
 *    gcc -DRUN_CRYPTOPT=0 ...
 * ═══════════════════════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>
#include <setjmp.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL

/* Default: try CryptOpt unless explicitly disabled */
#ifndef RUN_CRYPTOPT
#define RUN_CRYPTOPT 1
#endif

#ifndef RUN_MAC128
#define RUN_MAC128 1
#endif

/* ── Benchmark parameters ─────────────────────────────────────── */
#define OPS_PER_BATCH   1000
#define NUM_BATCHES     200
#define WARMUP_BATCHES  20


/* ══════════════════════════════════════════════════════════════════
 *  EXTERNAL: CryptOpt-generated asm
 *  Convention: fiat_curve25519_carry_square(uint64_t *out, const uint64_t *in)
 * ══════════════════════════════════════════════════════════════════ */
extern void fiat_curve25519_carry_square(uint64_t *out, const uint64_t *in);

/* Wrapper to normalize calling convention: (in, out) */
static inline void fe_sq_cryptopt(const uint64_t *a, uint64_t *out) {
        fiat_curve25519_carry_square(out, a);
}


/* ══════════════════════════════════════════════════════════════════
 *  REFERENCE — C, __uint128_t
 * ══════════════════════════════════════════════════════════════════ */
static void fe_sq_ref(const uint64_t *a, uint64_t *out) {
        uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
        uint64_t d0 = 2*a0, d1 = 2*a1, d2 = 2*a2, d3 = 2*a3;
        uint64_t r3 = 19*a3, r4 = 19*a4;

        __uint128_t c0 = (__uint128_t)a0*a0 + (__uint128_t)d1*r4 + (__uint128_t)d2*r3;
        __uint128_t c1 = (__uint128_t)d0*a1 + (__uint128_t)r3*a3 + (__uint128_t)d2*r4;
        __uint128_t c2 = (__uint128_t)d0*a2 + (__uint128_t)a1*a1 + (__uint128_t)d3*r4;
        __uint128_t c3 = (__uint128_t)d0*a3 + (__uint128_t)d1*a2 + (__uint128_t)r4*a4;
        __uint128_t c4 = (__uint128_t)d0*a4 + (__uint128_t)d1*a3 + (__uint128_t)a2*a2;

        uint64_t carry;
        carry = (uint64_t)(c0 >> 51); out[0] = (uint64_t)c0 & MASK51;
        c1 += carry;
        carry = (uint64_t)(c1 >> 51); out[1] = (uint64_t)c1 & MASK51;
        c2 += carry;
        carry = (uint64_t)(c2 >> 51); out[2] = (uint64_t)c2 & MASK51;
        c3 += carry;
        carry = (uint64_t)(c3 >> 51); out[3] = (uint64_t)c3 & MASK51;
        c4 += carry;
        carry = (uint64_t)(c4 >> 51); out[4] = (uint64_t)c4 & MASK51;

        out[0] += carry * 19;
        carry = out[0] >> 51; out[0] &= MASK51;
        out[1] += carry;
}


/* ══════════════════════════════════════════════════════════════════
 *  MAC128 — microcode hook (6-triad MAC128 at 0x7c00)
 * ══════════════════════════════════════════════════════════════════ */
#if RUN_MAC128
static void install_mac128(void) {
        ucode_t mac128_patch[] = {
                /* T0: save acc_lo, multiply */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1: sum + carry operands */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T2: propagated overflow + acc_hi */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T3: merge carry chain */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T4: extract carry bit */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T5: fold carry, done */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac128_patch, 6);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

__attribute__((noinline))
static void fe_sq_mac128(const uint64_t *a, uint64_t *out) {
        uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
        uint64_t d0 = 2*a0, d1 = 2*a1, d2 = 2*a2, d3 = 2*a3;
        uint64_t r3 = 19*a3, r4 = 19*a4;

        uint64_t c_lo, c_hi, carry;

        /* c[0] = a0·a0 + d1·r4 + d2·r3 */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x2]\n\t"  "mov rdx, %[y2]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x3]\n\t"  "mov rdx, %[y3]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [x1] "r"(a0), [y1] "r"(a0),
                  [x2] "r"(d1), [y2] "r"(r4),
                  [x3] "r"(d2), [y3] "r"(r3)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[0] = c_lo & MASK51;

        /* c[1] = carry + d0·a1 + r3·a3 + d2·r4 */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x2]\n\t"  "mov rdx, %[y2]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x3]\n\t"  "mov rdx, %[y3]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a1),
                  [x2] "r"(r3), [y2] "r"(a3),
                  [x3] "r"(d2), [y3] "r"(r4)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[1] = c_lo & MASK51;

        /* c[2] = carry + d0·a2 + a1·a1 + d3·r4 */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x2]\n\t"  "mov rdx, %[y2]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x3]\n\t"  "mov rdx, %[y3]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a2),
                  [x2] "r"(a1), [y2] "r"(a1),
                  [x3] "r"(d3), [y3] "r"(r4)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[2] = c_lo & MASK51;

        /* c[3] = carry + d0·a3 + d1·a2 + r4·a4 */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x2]\n\t"  "mov rdx, %[y2]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x3]\n\t"  "mov rdx, %[y3]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a3),
                  [x2] "r"(d1), [y2] "r"(a2),
                  [x3] "r"(r4), [y3] "r"(a4)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[3] = c_lo & MASK51;

        /* c[4] = carry + d0·a4 + d1·a3 + a2·a2 */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x2]\n\t"  "mov rdx, %[y2]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x3]\n\t"  "mov rdx, %[y3]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a4),
                  [x2] "r"(d1), [y2] "r"(a3),
                  [x3] "r"(a2), [y3] "r"(a2)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[4] = c_lo & MASK51;

        /* Final reduction */
        out[0] += carry * 19;
        carry = out[0] >> 51;
        out[0] &= MASK51;
        out[1] += carry;
}
#endif /* RUN_MAC128 */


/* ══════════════════════════════════════════════════════════════════
 *  TSC MEASUREMENT
 * ══════════════════════════════════════════════════════════════════ */
static inline uint64_t rdtscp_start(void) {
        uint32_t lo, hi;
        asm volatile("cpuid\n\t"
                     "rdtsc"
                     : "=a"(lo), "=d"(hi)
                     :: "rbx", "rcx");
        return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtscp_end(void) {
        uint32_t lo, hi;
        asm volatile("rdtscp\n\t"
                     "mov %0, eax\n\t"
                     "mov %1, edx\n\t"
                     "cpuid"
                     : "=r"(lo), "=r"(hi)
                     :: "rax", "rbx", "rcx", "rdx");
        return ((uint64_t)hi << 32) | lo;
}


/* ══════════════════════════════════════════════════════════════════
 *  SIGILL GUARD — detect BMI2 absence gracefully
 * ══════════════════════════════════════════════════════════════════ */
#if RUN_CRYPTOPT
static sigjmp_buf sigill_jmp;
static volatile int sigill_caught = 0;

static void sigill_handler(int sig) {
        (void)sig;
        sigill_caught = 1;
        siglongjmp(sigill_jmp, 1);
}

/* Returns 1 if CryptOpt asm is runnable, 0 if SIGILL */
static int probe_cryptopt(void) {
        struct sigaction sa, old_sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigill_handler;
        sigaction(SIGILL, &sa, &old_sa);

        uint64_t in[5]  = { 1, 0, 0, 0, 0 };
        uint64_t out[5] = { 0 };

        if (sigsetjmp(sigill_jmp, 1) == 0) {
                fiat_curve25519_carry_square(out, in);
                sigaction(SIGILL, &old_sa, NULL);
                return 1;  /* ran fine */
        }

        /* got SIGILL */
        sigaction(SIGILL, &old_sa, NULL);
        return 0;
}
#endif


/* ══════════════════════════════════════════════════════════════════
 *  PRINT HELPERS
 * ══════════════════════════════════════════════════════════════════ */
static void print_limbs(const char *label, const uint64_t *v) {
        printf("  %-10s [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ",\n"
               "             %016" PRIx64 ", %016" PRIx64 "]\n",
               label, v[0], v[1], v[2], v[3], v[4]);
}


/* ══════════════════════════════════════════════════════════════════
 *  CORRECTNESS TESTS
 * ══════════════════════════════════════════════════════════════════ */
typedef struct {
        const char *name;
        uint64_t in[5];
} sq_test_t;

static int run_test(const sq_test_t *t, int have_cryptopt, int have_mac128) {
        uint64_t ref[5], co[5], mac[5];
        int pass = 1;

        fe_sq_ref(t->in, ref);

        printf("  %s\n", t->name);
        print_limbs("input:", t->in);
        print_limbs("ref:", ref);

#if RUN_CRYPTOPT
        if (have_cryptopt) {
                fe_sq_cryptopt(t->in, co);
                print_limbs("cryptopt:", co);
                if (memcmp(ref, co, sizeof(ref)) != 0) {
                        printf("  *** CRYPTOPT MISMATCH ***\n");
                        pass = 0;
                }
        }
#endif

#if RUN_MAC128
        if (have_mac128) {
                fe_sq_mac128(t->in, mac);
                print_limbs("mac128:", mac);
                if (memcmp(ref, mac, sizeof(ref)) != 0) {
                        printf("  *** MAC128 MISMATCH ***\n");
                        pass = 0;
                }
        }
#endif

        printf("  %s\n\n", pass ? "✓ PASS" : "✗ FAIL");
        return pass;
}

static int test_iterated(int have_cryptopt, int have_mac128) {
        printf("─── Iterated Square Test (1000 rounds) ───\n\n");

        uint64_t ref[5] = { 1, 0, 0, 0, 0 };
        uint64_t co[5]  = { 1, 0, 0, 0, 0 };
        uint64_t mac[5] = { 1, 0, 0, 0, 0 };
        int pass = 1;

        for (int i = 0; i < 1000; i++) {
                uint64_t tmp[5];
                fe_sq_ref(ref, tmp);  memcpy(ref, tmp, sizeof(ref));
#if RUN_CRYPTOPT
                if (have_cryptopt) {
                        fe_sq_cryptopt(co, tmp);  memcpy(co, tmp, sizeof(co));
                }
#endif
#if RUN_MAC128
                if (have_mac128) {
                        fe_sq_mac128(mac, tmp);  memcpy(mac, tmp, sizeof(mac));
                }
#endif
        }

        printf("  After 1000 iterations:\n");
        print_limbs("ref:", ref);
#if RUN_CRYPTOPT
        if (have_cryptopt) {
                print_limbs("cryptopt:", co);
                if (memcmp(ref, co, sizeof(ref)) != 0) {
                        printf("  *** CRYPTOPT MISMATCH ***\n");
                        pass = 0;
                }
        }
#endif
#if RUN_MAC128
        if (have_mac128) {
                print_limbs("mac128:", mac);
                if (memcmp(ref, mac, sizeof(ref)) != 0) {
                        printf("  *** MAC128 MISMATCH ***\n");
                        pass = 0;
                }
        }
#endif

        printf("  %s\n\n", pass ? "✓ PASS" : "✗ FAIL");
        return pass;
}


/* ══════════════════════════════════════════════════════════════════
 *  BENCHMARK
 *
 *  Pattern: OPS_PER_BATCH ops inside timed region, NUM_BATCHES reps.
 *  Reports median cycles/op to reject outliers.
 * ══════════════════════════════════════════════════════════════════ */
static int cmp_u64(const void *a, const void *b) {
        uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
        return (x > y) - (x < y);
}

typedef void (*sq_fn)(const uint64_t *, uint64_t *);

static void bench_one(const char *name, sq_fn fn, const uint64_t *input) {
        uint64_t timings[NUM_BATCHES];
        uint64_t out[5];

        /* warmup */
        for (int i = 0; i < WARMUP_BATCHES; i++)
                for (int j = 0; j < OPS_PER_BATCH; j++)
                        fn(input, out);

        /* timed */
        for (int b = 0; b < NUM_BATCHES; b++) {
                uint64_t t0 = rdtscp_start();
                for (int j = 0; j < OPS_PER_BATCH; j++)
                        fn(input, out);
                uint64_t t1 = rdtscp_end();
                timings[b] = t1 - t0;
        }

        qsort(timings, NUM_BATCHES, sizeof(uint64_t), cmp_u64);

        uint64_t median = timings[NUM_BATCHES / 2] / OPS_PER_BATCH;
        uint64_t p10    = timings[NUM_BATCHES / 10] / OPS_PER_BATCH;
        uint64_t p90    = timings[NUM_BATCHES * 9 / 10] / OPS_PER_BATCH;

        printf("  %-12s  median: %4" PRIu64 " cyc/op   "
               "[p10: %4" PRIu64 ",  p90: %4" PRIu64 "]\n",
               name, median, p10, p90);
}


/* ══════════════════════════════════════════════════════════════════
 *  MAIN
 * ══════════════════════════════════════════════════════════════════ */
int main(void) {
        printf("╔═══════════════════════════════════════════════════════════╗\n");
        printf("║  Curve25519 Field Square — CryptOpt vs MAC128 Benchmark  ║\n");
        printf("╚═══════════════════════════════════════════════════════════╝\n\n");

        /* ── Probe available implementations ────────────────────── */

        int have_cryptopt = 0;
        int have_mac128   = 0;

#if RUN_CRYPTOPT
        printf("Probing CryptOpt asm (BMI2 required)... ");
        have_cryptopt = probe_cryptopt();
        printf("%s\n", have_cryptopt ? "OK" : "SIGILL — skipping (no BMI2)");
#else
        printf("CryptOpt: disabled at compile time\n");
#endif

#if RUN_MAC128
        printf("Installing MAC128 hook (6 triads, 0x0cd8 → 0x7c00)... ");
        install_mac128();
        have_mac128 = 1;
        printf("OK\n");
#else
        printf("MAC128: disabled at compile time\n");
#endif

        printf("\n");

        /* ── Correctness ────────────────────────────────────────── */

        printf("═══════════════════════════════════════════\n");
        printf("  Correctness Tests\n");
        printf("═══════════════════════════════════════════\n\n");

        sq_test_t tests[] = {
                { "identity (1,0,0,0,0)²",
                  { 1, 0, 0, 0, 0 } },
                { "small (7,11,3,5,2)²",
                  { 7, 11, 3, 5, 2 } },
                { "mid-range limbs",
                  { 0x1234567890ULL, 0x0ABCDEF01234ULL,
                    0x0000F00DCAFEULL, 0x0007000000000ULL,
                    0x00055555555555ULL } },
                { "near-max 51-bit limbs",
                  { MASK51, MASK51, MASK51, MASK51, MASK51 } },
                { "libsodium basepoint x",
                  { 0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                    0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                    0x000216936D3CD6E5ULL } },
                { "one-hot limb[2]",
                  { 0, 0, 1, 0, 0 } },
                { "alternating (max,0,max,0,max)",
                  { MASK51, 0, MASK51, 0, MASK51 } },
                { "powers of two",
                  { 1ULL << 10, 1ULL << 20, 1ULL << 30,
                    1ULL << 40, 1ULL << 50 } },
        };

        int n = sizeof(tests) / sizeof(tests[0]);
        int pass = 0;
        for (int i = 0; i < n; i++)
                pass += run_test(&tests[i], have_cryptopt, have_mac128);

        int iter_pass = test_iterated(have_cryptopt, have_mac128);

        int total = pass + iter_pass;
        int total_n = n + 1;

        printf("═══════════════════════════════════════════\n");
        printf("  Correctness: %d / %d passed\n", total, total_n);
        printf("═══════════════════════════════════════════\n\n");

        if (total != total_n) {
                printf("CORRECTNESS FAILURE — skipping benchmark.\n");
                return 1;
        }

        /* ── Benchmark ──────────────────────────────────────────── */

        printf("═══════════════════════════════════════════\n");
        printf("  Performance (%d ops/batch × %d batches)\n", OPS_PER_BATCH, NUM_BATCHES);
        printf("═══════════════════════════════════════════\n\n");

        /* Use libsodium basepoint as representative input */
        uint64_t bench_input[5] = {
                0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                0x000216936D3CD6E5ULL
        };

        bench_one("ref (C)", fe_sq_ref, bench_input);

#if RUN_CRYPTOPT
        if (have_cryptopt)
                bench_one("cryptopt", fe_sq_cryptopt, bench_input);
#endif

#if RUN_MAC128
        if (have_mac128)
                bench_one("mac128", fe_sq_mac128, bench_input);
#endif

        printf("\n");

        /* ── Summary ────────────────────────────────────────────── */

        printf("═══════════════════════════════════════════\n");
        printf("  Notes\n");
        printf("═══════════════════════════════════════════\n");
        if (!have_cryptopt)
                printf("  • CryptOpt asm skipped (BMI2 not available on this CPU).\n"
                       "    Re-generate with CryptOpt targeting Goldmont, or run\n"
                       "    on BMI2-capable hardware.\n");
        if (have_mac128)
                printf("  • MAC128 overhead is ~5 cyc/vmwrite × 15 calls = ~75 cyc redirect.\n"
                       "    Batching (3-MAC or monolithic hook) will reduce this.\n");
        printf("\n");

        return 0;
}