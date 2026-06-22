/*
 * test_adc_chain.c — Re-investigate ADC chaining for 4x64 microcode viability.
 *
 * Prior finding (adc_findings.md, 2026-04-17): ADC reads arch RFLAGS.CF
 * which is frozen at patch entry; GENARITHFLAGS_R is the only known
 * bridge from domain #1 (TMP-CF set by ADD) into domain #2 (arch CF
 * that ADC reads). A 128-bit add works in 3 triads.
 *
 * Open questions left in adc_findings.md:
 *   Q1: Does ADD+GENARITHFLAGS+ADC in ONE triad work?
 *       (slot 0/1/2 packing — saves 1 triad per limb)
 *   Q2: Does ADC's OWN carry-out propagate forward — i.e., does
 *       "ADC ; GENARITHFLAGS_R(dst) ; ADC" promote *this* ADC's carry
 *       for the next limb? If yes, N-limb add is ~2 triads per limb.
 *   Q3: Does GENARITHFLAGS still work inside a *large* patch
 *       (CLAUDE.md: "fails unreliably in large patches" — at what size?)
 *
 * Also probe:
 *   Q4: Alternative bridges — MOVEINSERTFLGS_DSZ64_DRR / MOVEMERGEFLGS
 *       (mentioned in inst.h around line 481 / 505). Untested.
 *
 * If Q2+Q3 pass, 4x64 fe_mul becomes ~1 triad per carry-propagation step
 * (vs 3 ops via SETCC dance). That could close the per-mul gap to
 * amd64-64's hardware ADC.
 *
 * Build: make PROG=test_adc_chain
 * Run:   sudo taskset -c 0 ./test_adc_chain_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* ─── invocation helper ─────────────────────────────────────────────── */

/* Fires the installed patch with arch CF forced to entry_cf. All 256-bit
 * inputs (a, b each a 4-limb little-endian array) are placed into memory
 * the patch wrapper expects. Result returned in `out`. */

/* For simplicity, every patch under test:
 *   - on entry: RAX=a_lo, RCX=a_hi, RDX=b_lo, RBX=b_hi (for 128-bit tests),
 *     or expanded for 4-limb tests as documented per-test
 *   - on exit (via END_SEQWORD): result limbs in (RAX, RCX, RDX, R8...) */

static void invoke_2reg(uint64_t a, uint64_t b, int entry_cf,
                        uint64_t *out_lo, uint64_t *out_hi) {
    /* 128-bit add: a in RAX:RCX, b in RDX:RBX. Result in RAX:RCX. */
    uint64_t r_lo, r_hi;
    uint64_t a_lo = a, a_hi = 0;
    uint64_t b_lo = b, b_hi = 0;
    uint64_t flags_img = entry_cf ? 0x3ULL : 0x2ULL;
    asm volatile(
        "push %[flg]\n\t"
        "popfq\n\t"
        "mov rax, %[alo]\n\t"
        "mov rcx, %[ahi]\n\t"
        "mov rdx, %[blo]\n\t"
        "mov rbx, %[bhi]\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %[rlo], rax\n\t"
        "mov %[rhi], rcx\n\t"
        : [rlo] "=&r"(r_lo), [rhi] "=&r"(r_hi)
        : [alo] "r"(a_lo), [ahi] "r"(a_hi),
          [blo] "r"(b_lo), [bhi] "r"(b_hi),
          [flg] "r"(flags_img)
        : "rax", "rcx", "rdx", "rbx", "cc", "memory"
    );
    *out_lo = r_lo;
    *out_hi = r_hi;
}

/* 4-limb add: a in (R8, R9, R10, R11), b in (R12, R13, R14, R15).
 * Result returned in (RAX, RCX, RDX, RBX), written to out[0..3]. */
static void invoke_4limb(const uint64_t a[4], const uint64_t b[4],
                         int entry_cf, uint64_t out[4]) {
    /* Stage inputs through memory; load to regs inside asm to avoid
     * GCC's "too many register constraints" issue. */
    uint64_t buf[9];
    buf[0] = a[0]; buf[1] = a[1]; buf[2] = a[2]; buf[3] = a[3];
    buf[4] = b[0]; buf[5] = b[1]; buf[6] = b[2]; buf[7] = b[3];
    buf[8] = entry_cf ? 0x3ULL : 0x2ULL;

    asm volatile(
        "push  qword ptr [%[buf]+64]\n\t"   /* flags img → push */
        "popfq\n\t"
        "mov r8,  [%[buf]]\n\t"
        "mov r9,  [%[buf]+8]\n\t"
        "mov r10, [%[buf]+16]\n\t"
        "mov r11, [%[buf]+24]\n\t"
        "mov r12, [%[buf]+32]\n\t"
        "mov r13, [%[buf]+40]\n\t"
        "mov r14, [%[buf]+48]\n\t"
        "mov r15, [%[buf]+56]\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov [%[out]],    rax\n\t"
        "mov [%[out]+8],  rcx\n\t"
        "mov [%[out]+16], rdx\n\t"
        "mov [%[out]+24], rbx\n\t"
        :
        : [buf] "r"(buf), [out] "r"(out)
        : "rax", "rbx", "rcx", "rdx",
          "r8", "r9", "r10", "r11",
          "r12", "r13", "r14", "r15",
          "cc", "memory"
    );
}

/* ═══ Q1: ADD+GENARITHFLAGS+ADC in the SAME triad ═══════════════════════
 * If slot 0/1/2 sequential semantics propagate the GENARITHFLAGS write
 * to ADC's read in the same triad, 128-bit add fits in 2 triads. */
static void install_Q1(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RAX, RDX),
          GENARITHFLAGS_R(TMP0),
          ADC_DSZ64_DRR(TMP1, RCX, RBX), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP0),
          ZEROEXT_DSZ64_DR(RCX, TMP1),
          NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, sizeof(p)/sizeof(p[0]));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* ═══ Q2: 4-limb chain — ADC carry-out via GENARITHFLAGS ════════════════
 * Does GENARITHFLAGS_R on an ADC's destination promote that ADC's carry
 * for the next ADC? Pattern:
 *   T0: ADD t0 = a0+b0;     GENARITHFLAGS(t0)              → CF for T1
 *   T1: ADC t1 = a1+b1+CF;  GENARITHFLAGS(t1)              → CF for T2
 *   T2: ADC t2 = a2+b2+CF;  GENARITHFLAGS(t2)              → CF for T3
 *   T3: ADC t3 = a3+b3+CF;                                 (done)
 *   T4: writeback
 * 5 triads total for 4-limb add. If this works, 4x64 microcode becomes
 * realistic. */
static void install_Q2(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R12),
          GENARITHFLAGS_R(TMP0),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP1, R9, R13),
          GENARITHFLAGS_R(TMP1),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP2, R10, R14),
          GENARITHFLAGS_R(TMP2),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP3, R11, R15),
          NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP0),
          ZEROEXT_DSZ64_DR(RCX, TMP1),
          ZEROEXT_DSZ64_DR(RDX, TMP2), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RBX, TMP3),
          NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, sizeof(p)/sizeof(p[0]));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* ═══ Q2b: 4-limb chain with ADD/ADC + GENARITHFLAGS in SAME triad ═════
 * If Q1 works, can we pack a limb-step into 1 triad?
 *   T0: ADD t0 = a0+b0; GENARITHFLAGS(t0); ADC t1 = a1+b1+CF
 *   T1: GENARITHFLAGS(t1); ADC t2 = a2+b2+CF; (slot 0 GF, slot 1 ADC, slot 2 ?)
 *   ...
 * 4-limb add in ~3 triads if this packs. */
static void install_Q2b(void) {
    ucode_t p[] = {
        /* T0: a0+b0 → t0; bridge → CF; a1+b1+CF → t1 */
        { ADD_DSZ64_DRR(TMP0, R8, R12),
          GENARITHFLAGS_R(TMP0),
          ADC_DSZ64_DRR(TMP1, R9, R13), NOP_SEQWORD },
        /* T1: bridge t1 → CF; a2+b2+CF → t2; nothing slot 2 */
        { GENARITHFLAGS_R(TMP1),
          ADC_DSZ64_DRR(TMP2, R10, R14),
          NOP, NOP_SEQWORD },
        /* T2: bridge t2 → CF; a3+b3+CF → t3 */
        { GENARITHFLAGS_R(TMP2),
          ADC_DSZ64_DRR(TMP3, R11, R15),
          NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP0),
          ZEROEXT_DSZ64_DR(RCX, TMP1),
          ZEROEXT_DSZ64_DR(RDX, TMP2), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RBX, TMP3),
          NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, sizeof(p)/sizeof(p[0]));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* ═══ Q3: ADC chain embedded in a LARGE patch ═══════════════════════════
 * CLAUDE.md claims GENARITHFLAGS "fails unreliably in large patches".
 * Surround the 4-limb add with filler triads (NOPs / harmless ops) so
 * the patch is similar in size to fe_mul (~66 triads). Does the carry
 * chain still produce correct results?
 *
 * If this passes, CLAUDE.md's caveat is overcautious. If it fails, we
 * need to characterize the failure mode (which patch size, which slot
 * positions, etc.). */
static void install_Q3(int filler_triads) {
    ucode_t *p = calloc(filler_triads + 10, sizeof(ucode_t));
    int i = 0;

    /* Filler: harmless ZEROEXTs that don't touch the chain regs (we use
     * TMP14 as a self-target so no state is disturbed). */
    for (int j = 0; j < filler_triads / 2; j++) {
        p[i].uop0 = ZEROEXT_DSZ64_DR(TMP14, TMP14);
        p[i].uop1 = ZEROEXT_DSZ64_DR(TMP14, TMP14);
        p[i].uop2 = ZEROEXT_DSZ64_DR(TMP14, TMP14);
        p[i].seqw = NOP_SEQWORD;
        i++;
    }

    /* 4-limb add chain (Q2 pattern). */
    p[i].uop0 = ADD_DSZ64_DRR(TMP0, R8, R12);
    p[i].uop1 = GENARITHFLAGS_R(TMP0);
    p[i].uop2 = NOP;
    p[i].seqw = NOP_SEQWORD;
    i++;
    p[i].uop0 = ADC_DSZ64_DRR(TMP1, R9, R13);
    p[i].uop1 = GENARITHFLAGS_R(TMP1);
    p[i].uop2 = NOP;
    p[i].seqw = NOP_SEQWORD;
    i++;
    p[i].uop0 = ADC_DSZ64_DRR(TMP2, R10, R14);
    p[i].uop1 = GENARITHFLAGS_R(TMP2);
    p[i].uop2 = NOP;
    p[i].seqw = NOP_SEQWORD;
    i++;
    p[i].uop0 = ADC_DSZ64_DRR(TMP3, R11, R15);
    p[i].uop1 = NOP;
    p[i].uop2 = NOP;
    p[i].seqw = NOP_SEQWORD;
    i++;

    /* Second filler block. */
    for (int j = filler_triads / 2; j < filler_triads; j++) {
        p[i].uop0 = ZEROEXT_DSZ64_DR(TMP14, TMP14);
        p[i].uop1 = ZEROEXT_DSZ64_DR(TMP14, TMP14);
        p[i].uop2 = ZEROEXT_DSZ64_DR(TMP14, TMP14);
        p[i].seqw = NOP_SEQWORD;
        i++;
    }

    /* Writeback. */
    p[i].uop0 = ZEROEXT_DSZ64_DR(RAX, TMP0);
    p[i].uop1 = ZEROEXT_DSZ64_DR(RCX, TMP1);
    p[i].uop2 = ZEROEXT_DSZ64_DR(RDX, TMP2);
    p[i].seqw = NOP_SEQWORD;
    i++;
    p[i].uop0 = ZEROEXT_DSZ64_DR(RBX, TMP3);
    p[i].uop1 = NOP;
    p[i].uop2 = NOP;
    p[i].seqw = END_SEQWORD;
    i++;

    patch_ucode(0x7c00, p, i);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    free(p);
}

/* ═══ Q4: Alternative bridges — MOVEINSERTFLGS / MOVEMERGEFLGS ══════════
 * inst.h has MOVEINSERTFLGS_DSZ64_DRR(dst, src0, src1) and
 * MOVEMERGEFLGS_DSZ64_DRR. These might be an alternative way to insert
 * domain-#1 CF into arch flags. Untested.
 *
 * Try: ADD t0 = a+b; MOVEINSERTFLGS scratch = t0,?; ADC t1 = c+d. */
static void install_Q4_insertflgs(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RAX, RDX),
          MOVEINSERTFLGS_DSZ64_DRR(TMP15, TMP0, TMP0),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP1, RCX, RBX),
          NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP0),
          ZEROEXT_DSZ64_DR(RCX, TMP1),
          NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, sizeof(p)/sizeof(p[0]));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_Q4_mergeflgs(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RAX, RDX),
          MOVEMERGEFLGS_DSZ64_DRR(TMP15, TMP0, TMP0),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP1, RCX, RBX),
          NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP0),
          ZEROEXT_DSZ64_DR(RCX, TMP1),
          NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, sizeof(p)/sizeof(p[0]));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* ─── test probes ─────────────────────────────────────────────────────── */

typedef struct {
    uint64_t a_lo, b_lo;
    uint64_t expect_lo, expect_hi;
    const char *label;
} probe128_t;

static probe128_t probes128[] = {
    { 1,                     1,                     2, 0, "no overflow" },
    { 0xFFFFFFFFFFFFFFFFULL, 1,                     0, 1, "lo overflow → CF must propagate" },
    { 0xDEADBEEF12345678ULL, 0xCAFEFEEDABCDEF01ULL, 0xA9ACBDDCBE024579ULL, 1, "random overflow" },
};

typedef struct {
    uint64_t a[4], b[4];
    uint64_t expect[4];
    const char *label;
} probe256_t;

static probe256_t probes256[] = {
    /* Simple */
    { {1,0,0,0}, {1,0,0,0}, {2,0,0,0}, "trivial" },
    /* Single-limb overflow */
    { {0xFFFFFFFFFFFFFFFFULL, 0, 0, 0}, {1, 0, 0, 0}, {0, 1, 0, 0}, "limb0 overflow" },
    /* Three-in-a-row carry */
    { {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0},
      {1, 0, 0, 0},
      {0, 0, 0, 1}, "3-limb chain carry" },
    /* All limbs overflow except top — full chain */
    { {0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0},
      {0xFFFFFFFFFFFFFFFFULL, 0, 0, 0},
      {0xFFFFFFFFFFFFFFFEULL, 0, 0, 1}, "max+max chain" },
};

static int run_128bit_test(const char *name, void (*install)(void)) {
    printf("--- %s ---\n", name);
    install();
    int pass = 0, total = 0;
    for (size_t i = 0; i < sizeof(probes128)/sizeof(probes128[0]); i++) {
        uint64_t got_lo = 0, got_hi = 0;
        invoke_2reg(probes128[i].a_lo, probes128[i].b_lo, 0, &got_lo, &got_hi);
        int ok = (got_lo == probes128[i].expect_lo) &&
                 (got_hi == probes128[i].expect_hi);
        printf("  %-45s  expect=(%016" PRIx64 ",%" PRIu64 ")  got=(%016" PRIx64 ",%" PRIu64 ")  %s\n",
               probes128[i].label,
               probes128[i].expect_lo, probes128[i].expect_hi,
               got_lo, got_hi,
               ok ? "PASS" : "FAIL");
        pass += ok; total++;
    }
    printf("  → %d/%d passed\n\n", pass, total);
    return pass == total;
}

static int run_256bit_test(const char *name, void (*install)(void)) {
    printf("--- %s ---\n", name);
    install();
    int pass = 0, total = 0;
    for (size_t i = 0; i < sizeof(probes256)/sizeof(probes256[0]); i++) {
        uint64_t got[4] = {0};
        invoke_4limb(probes256[i].a, probes256[i].b, 0, got);
        int ok = (got[0] == probes256[i].expect[0]) &&
                 (got[1] == probes256[i].expect[1]) &&
                 (got[2] == probes256[i].expect[2]) &&
                 (got[3] == probes256[i].expect[3]);
        printf("  %-30s  expect=%016" PRIx64 ":%016" PRIx64 ":%016" PRIx64 ":%016" PRIx64 "\n",
               probes256[i].label,
               probes256[i].expect[3], probes256[i].expect[2],
               probes256[i].expect[1], probes256[i].expect[0]);
        printf("  %-30s     got=%016" PRIx64 ":%016" PRIx64 ":%016" PRIx64 ":%016" PRIx64 "  %s\n",
               "", got[3], got[2], got[1], got[0],
               ok ? "PASS" : "FAIL");
        pass += ok; total++;
    }
    printf("  → %d/%d passed\n\n", pass, total);
    return pass == total;
}

static int run_q3_size(int filler_triads) {
    char name[64];
    snprintf(name, sizeof(name), "Q3: 4-limb chain with %d filler triads", filler_triads);
    printf("--- %s ---\n", name);
    install_Q3(filler_triads);
    int pass = 0, total = 0;
    for (size_t i = 0; i < sizeof(probes256)/sizeof(probes256[0]); i++) {
        uint64_t got[4] = {0};
        invoke_4limb(probes256[i].a, probes256[i].b, 0, got);
        int ok = (got[0] == probes256[i].expect[0]) &&
                 (got[1] == probes256[i].expect[1]) &&
                 (got[2] == probes256[i].expect[2]) &&
                 (got[3] == probes256[i].expect[3]);
        pass += ok; total++;
        if (!ok) {
            printf("  FAIL on %s: got %016" PRIx64 ":%016" PRIx64 ":%016" PRIx64 ":%016" PRIx64 "\n",
                   probes256[i].label, got[3], got[2], got[1], got[0]);
        }
    }
    printf("  → %d/%d passed%s\n\n", pass, total,
           pass == total ? " (chain survives this size)" : " — bridge starts breaking!");
    return pass == total;
}

int main(void) {
    printf("============================================================\n");
    printf("  ADC chaining survey for 4x64 microcode viability\n");
    printf("============================================================\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    /* Q1: same-triad ADD+GENARITHFLAGS+ADC */
    run_128bit_test("Q1: same-triad ADD/GENARITHFLAGS/ADC",
                    install_Q1);

    /* Q2: 4-limb chain (5 triads, baseline) */
    run_256bit_test("Q2: 4-limb chain — does ADC's CF survive GENARITHFLAGS bridge?",
                    install_Q2);

    /* Q2b: packed 4-limb chain (~3 triads if Q1 + chaining both work) */
    run_256bit_test("Q2b: packed 4-limb chain (1 triad per limb)",
                    install_Q2b);

    /* Q3: chain in large patch */
    printf("============================================================\n");
    printf("  Q3: How LARGE can the patch be before the chain breaks?\n");
    printf("============================================================\n\n");
    for (int n = 10; n <= 100; n += 20) {
        run_q3_size(n);
    }

    /* Q4: alternative bridges */
    printf("============================================================\n");
    printf("  Q4: Alternative bridges (MOVEINSERTFLGS, MOVEMERGEFLGS)\n");
    printf("============================================================\n\n");
    run_128bit_test("Q4a: MOVEINSERTFLGS as bridge", install_Q4_insertflgs);
    run_128bit_test("Q4b: MOVEMERGEFLGS as bridge", install_Q4_mergeflgs);

    printf("============================================================\n");
    printf("  Decision criteria for 4x64 microcode:\n");
    printf("    Q2 PASS + Q3 PASS at ~70 triads → 4x64 is viable\n");
    printf("    Q2b PASS → can pack carry chain at 1 triad/limb\n");
    printf("    Q4 PASS → faster bridge available, even better\n");
    printf("============================================================\n");
    return 0;
}
