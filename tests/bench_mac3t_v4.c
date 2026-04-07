/*
 * bench_mac4t_genflags.c — 4-triad MAC with GENARITHFLAGS + SETCC
 *
 * ═══════════════════════════════════════════════════════════════════
 *  Root cause found: END_SEQWORD restores pre-hook EFLAGS.
 *  On 2nd+ vmwrite, SETCC reads stale CF=0.
 *
 *  Fix: GENARITHFLAGS recomputes CF from register values (result vs
 *  operand), so it's immune to the EFLAGS restore.
 *
 *  4-triad MAC:
 *    T0: ZEROEXT TMP3, RAX  | MUL R64SRC, R64SRC, R64DST | NOP
 *    T1: ADD RAX, TMP3, RCX | ADD R8, R8, RDX            | NOP
 *    T2: GENARITHFLAGS_RR(RAX, TMP3) | SETCC_CONDB TMP5  | NOP
 *    T3: ADD R8, R8, TMP5   | NOP | END
 *
 *  15 vmwrite × 4 triads = 60 triads (was 90 with 6-triad).
 *
 *  Build:  make PROG=bench_mac4t_genflags
 *  Run:    sudo taskset -c 0 ./bench_mac4t_genflags_static
 * ═══════════════════════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL

#define OPS_PER_BATCH   1000
#define NUM_BATCHES     200
#define WARMUP_BATCHES  20


/* ══════════════════════════════════════════════════════════════════
 *  4-TRIAD MAC: GENARITHFLAGS + SETCC (EFLAGS-restore immune)
 * ══════════════════════════════════════════════════════════════════ */
static void install_mac4t(void) {
        ucode_t patch[] = {
                /* T0: save acc_lo, multiply */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1: accumulate both halves (no flag dependency here) */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T2: regenerate CF from ADD result, capture it */
                {
                        GENARITHFLAGS_RR(RAX, TMP3),
                        SETCC_CONDB_DR(TMP5, RAX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T3: fold carry, done */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 4);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  6-TRIAD MAC (original, for A/B comparison)
 * ══════════════════════════════════════════════════════════════════ */
static void install_mac6t(void) {
        ucode_t patch[] = {
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 6);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  REFERENCE
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
 *  CURVE25519 SQUARE — 15 vmwrite (shared by both hooks)
 * ══════════════════════════════════════════════════════════════════ */
__attribute__((noinline))
static void fe_sq_mac(const uint64_t *a, uint64_t *out) {
        uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
        uint64_t d0 = 2*a0, d1 = 2*a1, d2 = 2*a2, d3 = 2*a3;
        uint64_t r3 = 19*a3, r4 = 19*a4;

        uint64_t c_lo, c_hi, carry;

#define MAC(xA, yA) \
        "mov rcx, %[" xA "]\n\t" \
        "mov rdx, %[" yA "]\n\t" \
        "vmwrite rcx, rdx\n\t"

        /* c[0] = a0*a0 + d1*r4 + d2*r3 */
        asm volatile(
                "xor rax, rax\n\t" "xor r8, r8\n\t"
                MAC("x1","y1") MAC("x2","y2") MAC("x3","y3")
                "mov %[lo], rax\n\t" "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [x1] "r"(a0), [y1] "r"(a0),
                  [x2] "r"(d1), [y2] "r"(r4),
                  [x3] "r"(d2), [y3] "r"(r3)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51); out[0] = c_lo & MASK51;

        /* c[1] = carry + d0*a1 + r3*a3 + d2*r4 */
        asm volatile(
                "mov rax, %[cin]\n\t" "xor r8, r8\n\t"
                MAC("x1","y1") MAC("x2","y2") MAC("x3","y3")
                "mov %[lo], rax\n\t" "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a1),
                  [x2] "r"(r3), [y2] "r"(a3),
                  [x3] "r"(d2), [y3] "r"(r4)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51); out[1] = c_lo & MASK51;

        /* c[2] = carry + d0*a2 + a1*a1 + d3*r4 */
        asm volatile(
                "mov rax, %[cin]\n\t" "xor r8, r8\n\t"
                MAC("x1","y1") MAC("x2","y2") MAC("x3","y3")
                "mov %[lo], rax\n\t" "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a2),
                  [x2] "r"(a1), [y2] "r"(a1),
                  [x3] "r"(d3), [y3] "r"(r4)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51); out[2] = c_lo & MASK51;

        /* c[3] = carry + d0*a3 + d1*a2 + r4*a4 */
        asm volatile(
                "mov rax, %[cin]\n\t" "xor r8, r8\n\t"
                MAC("x1","y1") MAC("x2","y2") MAC("x3","y3")
                "mov %[lo], rax\n\t" "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a3),
                  [x2] "r"(d1), [y2] "r"(a2),
                  [x3] "r"(r4), [y3] "r"(a4)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51); out[3] = c_lo & MASK51;

        /* c[4] = carry + d0*a4 + d1*a3 + a2*a2 */
        asm volatile(
                "mov rax, %[cin]\n\t" "xor r8, r8\n\t"
                MAC("x1","y1") MAC("x2","y2") MAC("x3","y3")
                "mov %[lo], rax\n\t" "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a4),
                  [x2] "r"(d1), [y2] "r"(a3),
                  [x3] "r"(a2), [y3] "r"(a2)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51); out[4] = c_lo & MASK51;

#undef MAC

        out[0] += carry * 19;
        carry = out[0] >> 51; out[0] &= MASK51;
        out[1] += carry;
}


/* ══════════════════════════════════════════════════════════════════
 *  MEASUREMENT
 * ══════════════════════════════════════════════════════════════════ */
static inline uint64_t rdtscp_start(void) {
        uint32_t lo, hi;
        asm volatile("cpuid\n\t" "rdtsc"
                     : "=a"(lo), "=d"(hi) :: "rbx", "rcx");
        return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtscp_end(void) {
        uint32_t lo, hi;
        asm volatile("rdtscp\n\t"
                     "mov %0, eax\n\t" "mov %1, edx\n\t"
                     "cpuid"
                     : "=r"(lo), "=r"(hi)
                     :: "rax", "rbx", "rcx", "rdx");
        return ((uint64_t)hi << 32) | lo;
}

static void fe_normalize(const uint64_t *in, uint64_t *out) {
        uint64_t t[5], c;
        memcpy(t, in, 40);
        for (int p = 0; p < 2; p++) {
                c = t[0] >> 51; t[0] &= MASK51;
                t[1] += c; c = t[1] >> 51; t[1] &= MASK51;
                t[2] += c; c = t[2] >> 51; t[2] &= MASK51;
                t[3] += c; c = t[3] >> 51; t[3] &= MASK51;
                t[4] += c; c = t[4] >> 51; t[4] &= MASK51;
                t[0] += c * 19;
        }
        memcpy(out, t, 40);
}

static int fe_equal(const uint64_t *a, const uint64_t *b) {
        uint64_t na[5], nb[5];
        fe_normalize(a, na); fe_normalize(b, nb);
        return memcmp(na, nb, 40) == 0;
}

static void print_limbs(const char *label, const uint64_t *v) {
        printf("  %-8s [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ",\n"
               "           %016" PRIx64 ", %016" PRIx64 "]\n",
               label, v[0], v[1], v[2], v[3], v[4]);
}

typedef void (*sq_fn)(const uint64_t *, uint64_t *);

static int cmp_u64(const void *a, const void *b) {
        uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
        return (x > y) - (x < y);
}

static void bench(const char *name, sq_fn fn, const uint64_t *input) {
        uint64_t timings[NUM_BATCHES], out[5];
        volatile uint64_t sink = 0; (void)sink;

        for (int i = 0; i < WARMUP_BATCHES; i++)
                for (int j = 0; j < OPS_PER_BATCH; j++)
                        fn(input, out);

        for (int b = 0; b < NUM_BATCHES; b++) {
                uint64_t t0 = rdtscp_start();
                for (int j = 0; j < OPS_PER_BATCH; j++)
                        fn(input, out);
                uint64_t t1 = rdtscp_end();
                timings[b] = t1 - t0;
                sink = out[0];
        }

        qsort(timings, NUM_BATCHES, sizeof(uint64_t), cmp_u64);
        uint64_t med = timings[NUM_BATCHES/2] / OPS_PER_BATCH;
        uint64_t p10 = timings[NUM_BATCHES/10] / OPS_PER_BATCH;
        uint64_t p90 = timings[NUM_BATCHES*9/10] / OPS_PER_BATCH;

        printf("  %-12s  median: %4" PRIu64 " cyc/op   "
               "[p10: %4" PRIu64 ",  p90: %4" PRIu64 "]\n",
               name, med, p10, p90);
}


/* ══════════════════════════════════════════════════════════════════
 *  REPEATED-INVOCATION SANITY CHECK
 * ══════════════════════════════════════════════════════════════════ */
static int test_repeated_carry(const char *label) {
        uint64_t lo, hi;

        /* 2 × (0xFFFFFFFF × 0xFFFFFFFF) = 2 × 0xFFFFFFFE00000001
         * = 0x1FFFFFFFC00000002
         * Expected: lo=0xFFFFFFFC00000002, hi=1
         */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[a]\n\t" "mov rdx, %[b]\n\t" "vmwrite rcx, rdx\n\t"
                "mov rcx, %[a]\n\t" "mov rdx, %[b]\n\t" "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(lo), [hi] "=r"(hi)
                : [a] "r"((uint64_t)0xFFFFFFFFULL), [b] "r"((uint64_t)0xFFFFFFFFULL)
                : "rax", "rcx", "rdx", "r8"
        );

        int ok = (hi == 1 && lo == 0xFFFFFFFC00000002ULL);
        printf("  %-12s  repeated carry: lo=0x%016" PRIx64 " hi=%" PRIu64 " %s\n",
               label, lo, hi, ok ? "PASS" : "** FAIL **");
        return ok;
}


/* ══════════════════════════════════════════════════════════════════ */
int main(void) {
        printf("==========================================================\n");
        printf("  4-triad MAC (GENARITHFLAGS+SETCC) vs 6-triad MAC\n");
        printf("==========================================================\n\n");

        uint64_t input[5] = {
                0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                0x000216936D3CD6E5ULL
        };
        uint64_t ref[5];
        fe_sq_ref(input, ref);

        /* ── 4-triad MAC (GENARITHFLAGS+SETCC) ───────────────── */

        printf("Installing 4-triad MAC (GENARITHFLAGS + SETCC_CONDB)...\n\n");
        install_mac4t();

        /* Repeated carry check first */
        if (!test_repeated_carry("mac4t")) {
                printf("\n  *** Repeated carry still broken! ***\n");
                printf("  GENARITHFLAGS does not fix the EFLAGS restore issue.\n");
                return 1;
        }

        /* Full correctness */
        uint64_t out[5];
        fe_sq_mac(input, out);
        printf("  Correctness: ");
        if (fe_equal(ref, out)) {
                printf("PASS\n");
        } else {
                printf("FAIL\n");
                print_limbs("ref:", ref);
                print_limbs("mac4t:", out);
                return 1;
        }

        /* Iterated */
        {
                uint64_t r[5]={1,0,0,0,0}, m[5]={1,0,0,0,0}, tmp[5];
                for (int i = 0; i < 1000; i++) {
                        fe_sq_ref(r,tmp); memcpy(r,tmp,40);
                        fe_sq_mac(m,tmp); memcpy(m,tmp,40);
                }
                printf("  Iterated (1000x): %s\n", fe_equal(r,m) ? "PASS" : "FAIL");
                if (!fe_equal(r,m)) return 1;
        }

        printf("\n");
        bench("ref (C)", fe_sq_ref, input);
        bench("mac4t", fe_sq_mac, input);

        /* ── 6-triad MAC for comparison ───────────────────────── */

        printf("\nRe-installing 6-triad MAC (bit-manipulation carry)...\n");
        install_mac6t();

        test_repeated_carry("mac6t");
        fe_sq_mac(input, out);
        printf("  Correctness: %s\n\n", fe_equal(ref,out) ? "PASS" : "FAIL");

        bench("ref (C)", fe_sq_ref, input);
        bench("mac6t", fe_sq_mac, input);

        printf("\n==========================================================\n");
        printf("  Summary\n");
        printf("==========================================================\n");
        printf("  mac6t: 15 vmwrite x 6 triads = 90 triads/sq\n");
        printf("  mac4t: 15 vmwrite x 4 triads = 60 triads/sq (33%% fewer)\n");
        printf("  Key: GENARITHFLAGS recomputes CF from registers,\n");
        printf("       immune to END_SEQWORD EFLAGS restore.\n\n");

        return 0;
}
