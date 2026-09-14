/*
 * test_genarithflags_semantics.c — Does GENARITHFLAGS read TMP-CF or TMP-value?
 *
 * Background: Prior adc_findings.md (2026-04-17) concluded that
 * GENARITHFLAGS_R bridges domain #1 → domain #2. But every test case
 * used inputs where ADD-with-overflow happened to produce TMP=0 (e.g.
 * 0xFFFF..FF + 1 = 0 with CF=1). That coincidence means the original
 * tests can't distinguish:
 *
 *   (H1) GENARITHFLAGS_R(src) sets arch CF = src's TMP-CF (the actual
 *        carry from ADD).
 *   (H2) GENARITHFLAGS_R(src) sets arch CF = (src == 0).
 *
 * Both hypotheses produce identical outputs when the only failing
 * probes happen to have TMP=0.
 *
 * DISCRIMINATOR: pick inputs where ADD overflows (CF=1) but the lo
 * half is NON-ZERO. Then H1 predicts ADC adds 1; H2 predicts ADC adds 0.
 *
 *   a = 0xFFFFFFFFFFFFFFFE, b = 3
 *   ADD → lo = 1, CF = 1, TMP-value = 1
 *   ADC TMP1 = 0 + 0 + arch_CF
 *     H1 (CF-bridge): TMP1 = 1
 *     H2 (value-based): TMP1 = 0
 *
 * Multiple variations to nail down behavior.
 *
 * Build: make PROG=test_genarithflags_semantics
 * Run:   sudo taskset -c 0 ./test_genarithflags_semantics_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* Fires patch. Inputs in (R9, R10) → a, b. Result in RAX. arch CF=0. */
static uint64_t fire(uint64_t a, uint64_t b) {
    uint64_t res;
    asm volatile(
        "push 2\n\t"            /* clear arch CF (set bit 1 only = 0x2) */
        "popfq\n\t"
        "mov r9,  %[a]\n\t"
        "mov r10, %[b]\n\t"
        "xor eax, eax\n\t"
        "xor ecx, ecx\n\t"
        "xor edx, edx\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %[res], rax\n\t"
        : [res] "=r"(res)
        : [a] "r"(a), [b] "r"(b)
        : "rax", "rcx", "rdx", "r9", "r10", "cc", "memory"
    );
    return res;
}

static void install_genflags_pattern(void) {
    /* Canonical 3-triad pattern from adc_findings.md:
     *   T0: ADD t0 = R9 + R10;  GENARITHFLAGS_R(t0)
     *   T1: ADC RAX = RAX + RCX  (RCX=0, RAX=0; result = 0 + arch_CF)
     *   T2: END
     * RAX after = arch CF. */
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R9, R10), GENARITHFLAGS_R(TMP0),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, RAX, RCX), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

typedef struct {
    uint64_t a, b;
    uint64_t add_result;  /* what TMP0 becomes after ADD */
    int true_cf;          /* what the actual unsigned ADD CF is */
    const char *label;
} probe_t;

static probe_t probes[] = {
    { 1,                     1,                     2, 0, "no overflow, TMP=2"              },
    { 0xFFFFFFFFFFFFFFFFULL, 1,                     0, 1, "overflow, TMP=0  (canonical case)" },
    { 0xFFFFFFFFFFFFFFFEULL, 3,                     1, 1, "overflow, TMP=1  (DISCRIMINATOR)" },
    { 0xFFFFFFFFFFFFFFFEULL, 5,                     3, 1, "overflow, TMP=3  (DISCRIMINATOR)" },
    { 0xDEADBEEF12345678ULL, 0xCAFEFEEDABCDEF01ULL,
      0xA9ACBDDCBE024579ULL, 1, "overflow, TMP=random" },
    { 0x8000000000000000ULL, 0x8000000000000000ULL,
      0,                     1, "overflow, TMP=0  (sum wraps to 0)" },
    { 0,                     0,                     0, 0, "no overflow, TMP=0"              },
    { 5,                     0,                     5, 0, "no overflow, TMP=5"              },
};

int main(void) {
    printf("============================================================\n");
    printf("  Does GENARITHFLAGS_R bridge TMP-CF, or is it value-based?\n");
    printf("============================================================\n\n");

    assign_to_core(0);
    install_genflags_pattern();

    printf("Pattern: ADD t0 = a + b ; GENARITHFLAGS_R(t0) ; ADC rax = 0 + 0\n");
    printf("RAX_out = arch CF that ADC reads.\n\n");

    int h1_consistent = 1;  /* GENARITHFLAGS reads TMP-CF */
    int h2_consistent = 1;  /* GENARITHFLAGS reads (TMP == 0) */

    for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
        uint64_t got = fire(probes[i].a, probes[i].b);
        int h1_predict = probes[i].true_cf;
        int h2_predict = (probes[i].add_result == 0) ? 1 : 0;
        printf("  %-40s\n", probes[i].label);
        printf("    a=%016" PRIx64 " b=%016" PRIx64 "\n", probes[i].a, probes[i].b);
        printf("    TMP0=%016" PRIx64 "  true_CF=%d\n", probes[i].add_result, probes[i].true_cf);
        printf("    H1 predicts (CF-bridge):  RAX = %d\n", h1_predict);
        printf("    H2 predicts (value==0):   RAX = %d\n", h2_predict);
        printf("    got RAX = %" PRIu64 "  %s\n",
               got,
               (int)got == h1_predict && (int)got == h2_predict ? "(both match)" :
               (int)got == h1_predict ? "<- matches H1 (real CF bridge)" :
               (int)got == h2_predict ? "<- matches H2 (value==0)" :
               "<- matches NEITHER");
        printf("\n");
        if ((int)got != h1_predict) h1_consistent = 0;
        if ((int)got != h2_predict) h2_consistent = 0;
    }

    printf("============================================================\n");
    printf("  Verdict:\n");
    if (h1_consistent && !h2_consistent) {
        printf("    GENARITHFLAGS_R is a REAL CF bridge.\n");
        printf("    → 4x64 microcode with ADC chains is VIABLE.\n");
    } else if (!h1_consistent && h2_consistent) {
        printf("    GENARITHFLAGS_R sets arch CF = (src == 0). NOT a CF bridge.\n");
        printf("    → 4x64 microcode via ADC is NOT viable.\n");
        printf("    → Prior adc_findings.md conclusion was wrong (only tested\n");
        printf("       cases where TMP=0 coincided with CF=1).\n");
    } else if (h1_consistent && h2_consistent) {
        printf("    Test cases didn't discriminate. Add probes with CF=1, TMP!=0.\n");
    } else {
        printf("    Behavior matches neither H1 nor H2. Unknown semantics.\n");
    }
    printf("============================================================\n");
    return 0;
}
