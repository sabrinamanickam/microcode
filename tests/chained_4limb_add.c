/*
 * chained_4limb_add.c — 4-limb 256-bit add via chained ADC + GFL_RR.
 *
 * Pattern (the new primitive in action):
 *   T_setup: stash a[0..3], b[0..3] into TMPs
 *   T0: ADD t0 = a0 + b0           (sets TMP-CF for t0)
 *   T1: GFL_RR(t0, t0)              (arch CF = t0's CF)
 *   T2: ADC t1 = a1 + b1 + arch_CF (sets TMP-CF for t1)
 *   T3: GFL_RR(t1, t1)
 *   T4: ADC t2 = a2 + b2 + arch_CF
 *   T5: GFL_RR(t2, t2)
 *   T6: ADC t3 = a3 + b3 + arch_CF
 *   T7: GFL_RR(t3, t3)              (publish final CF for caller)
 *   T_writeback: t0..t3 → arch regs for wrapper to store
 *   T_final: ADC RAX = 0 + 0 + arch_CF  (final carry as a value too)
 *
 * Wrapper passes a, b via memory; reads result back.
 *
 * Tests:
 *   1. 10k random additions verified against __uint128_t reference
 *   2. Cycle-count microbench
 *
 * Build: make PROG=chained_4limb_add
 * Run:   sudo taskset -c 0 ./chained_4limb_add_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

static void install_patch(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        /* ── stash a, b into TMPs ──
         * a[0..3] = R8, R9, R10, R11    → TMP0, TMP1, TMP2, TMP3
         * b[0..3] = R12, R13, R14, R15  → TMP4, TMP5, TMP6, TMP7
         * 8 ZEROEXTs in 3 triads */
        { ZEROEXT_DSZ64_DR(TMP0, R8),  ZEROEXT_DSZ64_DR(TMP1, R9),
          ZEROEXT_DSZ64_DR(TMP2, R10), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(TMP3, R11), ZEROEXT_DSZ64_DR(TMP4, R12),
          ZEROEXT_DSZ64_DR(TMP5, R13), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(TMP6, R14), ZEROEXT_DSZ64_DR(TMP7, R15),
          NOP, NOP_SEQWORD },

        /* ── 4-limb chained add ── */
        /* limb 0: ADD t0 = a0 + b0 ; GFL */
        { ADD_DSZ64_DRR(TMP8, TMP0, TMP4), GENARITHFLAGS_RR(TMP8, TMP8),
          NOP, NOP_SEQWORD },
        /* limb 1: ADC t1 = a1 + b1 + CF ; GFL */
        { ADC_DSZ64_DRR(TMP9, TMP1, TMP5), GENARITHFLAGS_RR(TMP9, TMP9),
          NOP, NOP_SEQWORD },
        /* limb 2: ADC t2 = a2 + b2 + CF ; GFL */
        { ADC_DSZ64_DRR(TMP10, TMP2, TMP6), GENARITHFLAGS_RR(TMP10, TMP10),
          NOP, NOP_SEQWORD },
        /* limb 3: ADC t3 = a3 + b3 + CF ; GFL */
        { ADC_DSZ64_DRR(TMP11, TMP3, TMP7), GENARITHFLAGS_RR(TMP11, TMP11),
          NOP, NOP_SEQWORD },

        /* ── writeback: results to arch regs the wrapper can read ── */
        { ZEROEXT_DSZ64_DR(R8,  TMP8),  ZEROEXT_DSZ64_DR(R9,  TMP9),
          ZEROEXT_DSZ64_DR(R10, TMP10), NOP_SEQWORD },
        /* R11 = result[3], RAX = final CF as a value (0 or 1) */
        { ZEROEXT_DSZ64_DR(R11, TMP11),
          ADC_DSZ64_DRR(RAX, RDX, RSI),     /* RDX=RSI=0 → RAX = arch CF */
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* fe_add via microcode: 4 limbs of a + 4 limbs of b → out, returns final carry.
 * Inputs/outputs staged through a single memory buffer to avoid register pressure.
 *   buf[0..3]  = a
 *   buf[4..7]  = b
 *   buf[8..11] = out
 *   buf[12]    = final carry
 */
static uint64_t fe_add_ucode(const uint64_t a[4], const uint64_t b[4], uint64_t out[4]) {
    uint64_t buf[13];
    buf[0]=a[0]; buf[1]=a[1]; buf[2]=a[2]; buf[3]=a[3];
    buf[4]=b[0]; buf[5]=b[1]; buf[6]=b[2]; buf[7]=b[3];
    asm volatile(
        "mov  r8,  qword ptr [%[bp] + 0]\n\t"
        "mov  r9,  qword ptr [%[bp] + 8]\n\t"
        "mov  r10, qword ptr [%[bp] + 16]\n\t"
        "mov  r11, qword ptr [%[bp] + 24]\n\t"
        "mov  r12, qword ptr [%[bp] + 32]\n\t"
        "mov  r13, qword ptr [%[bp] + 40]\n\t"
        "mov  r14, qword ptr [%[bp] + 48]\n\t"
        "mov  r15, qword ptr [%[bp] + 56]\n\t"
        "xor  rdx, rdx\n\t"
        "xor  rsi, rsi\n\t"
        "xor  rax, rax\n\t"
        "mov  rbx, 1\n\t"
        "mov  rcx, 1\n\t"
        "push 2\n\t"
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  qword ptr [%[bp] + 64], r8\n\t"
        "mov  qword ptr [%[bp] + 72], r9\n\t"
        "mov  qword ptr [%[bp] + 80], r10\n\t"
        "mov  qword ptr [%[bp] + 88], r11\n\t"
        "mov  qword ptr [%[bp] + 96], rax\n\t"
        :
        : [bp] "r"(buf)
        : "rax", "rbx", "rcx", "rdx", "rsi",
          "r8",  "r9",  "r10", "r11",
          "r12", "r13", "r14", "r15",
          "cc", "memory"
    );
    out[0] = buf[8]; out[1] = buf[9]; out[2] = buf[10]; out[3] = buf[11];
    return buf[12];
}

/* Reference: 4-limb add via __uint128_t. */
static uint64_t fe_add_ref(const uint64_t a[4], const uint64_t b[4], uint64_t out[4]) {
    __uint128_t s;
    s = (__uint128_t)a[0] + b[0];            out[0] = (uint64_t)s;
    s = (__uint128_t)a[1] + b[1] + (s >> 64); out[1] = (uint64_t)s;
    s = (__uint128_t)a[2] + b[2] + (s >> 64); out[2] = (uint64_t)s;
    s = (__uint128_t)a[3] + b[3] + (s >> 64); out[3] = (uint64_t)s;
    return (uint64_t)(s >> 64);
}

/* ─────── verification ─────── */

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static int verify(void) {
    int pass = 0, fail = 0;
    uint64_t rng = 42;

    /* Known vectors */
    struct { uint64_t a[4], b[4]; } vecs[] = {
        { {0,0,0,0}, {0,0,0,0} },
        { {1,0,0,0}, {1,0,0,0} },
        { {0xFFFFFFFFFFFFFFFFULL,0,0,0}, {1,0,0,0} },     /* limb0 overflows */
        { {0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL,
           0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL}, {1,0,0,0} },  /* propagates 3x */
        { {0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL,
           0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL},
          {0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL,
           0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL} },
    };
    printf("--- Known vectors ---\n");
    for (size_t i = 0; i < sizeof(vecs)/sizeof(vecs[0]); i++) {
        uint64_t out_u[4], out_r[4];
        uint64_t cu = fe_add_ucode(vecs[i].a, vecs[i].b, out_u);
        uint64_t cr = fe_add_ref(  vecs[i].a, vecs[i].b, out_r);
        int ok = (cu == cr) && !memcmp(out_u, out_r, 32);
        printf("  vec %zu: carry ucode=%" PRIu64 " ref=%" PRIu64 "  %s\n",
               i, cu, cr, ok ? "PASS" : "FAIL");
        if (ok) pass++; else fail++;
    }

    printf("--- 10000 random ---\n");
    int random_pass = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t a[4], b[4], out_u[4], out_r[4];
        for (int j = 0; j < 4; j++) { a[j] = splitmix64(&rng); b[j] = splitmix64(&rng); }
        uint64_t cu = fe_add_ucode(a, b, out_u);
        uint64_t cr = fe_add_ref(a, b, out_r);
        if (cu == cr && !memcmp(out_u, out_r, 32)) random_pass++;
    }
    printf("  %d / 10000 PASS\n", random_pass);
    if (random_pass == 10000) pass++; else fail++;

    printf("\n=== verify: %d sets pass, %d fail ===\n\n", pass, fail);
    return fail;
}

/* ─────── benchmark ─────── */

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

#define BATCH 10000
#define REPS  200

int main(void) {
    printf("=== Chained-ADC 4-limb add: 4×64 microcode vs reference ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_patch();

    if (verify()) {
        printf("verify failed, skipping bench\n");
        return 1;
    }

    uint64_t a[4] = {0x123, 0x456, 0x789, 0xABC};
    uint64_t b[4] = {0xDEF, 0x111, 0x222, 0x333};
    uint64_t out[4];

    printf("--- bench: %d ops × %d reps ---\n\n", BATCH, REPS);

    /* native __uint128_t reference */
    uint64_t min = UINT64_MAX, sum = 0;
    for (int r = 0; r < REPS; r++) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_add_ref(a, b, out);
        uint64_t t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Native __uint128_t: min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* microcode chained ADC */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_add_ucode(a, b, out);
        uint64_t t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode chain:    min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    init_match_and_patch(); do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
