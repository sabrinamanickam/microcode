/*
 * chain_length.c — Verify chained ADC + GFL_RR(TMP,TMP) scales cleanly
 * to arbitrary chain lengths.
 *
 * Pattern under test, N-limb chain:
 *   T_setup: TMP_A = R10, TMP_B = R11        (stash the uniform inputs)
 *   T_limb0: ADD TMP_acc = TMP_A + TMP_B
 *   T_gfl0:  GFL_RR(TMP_acc, TMP_acc)
 *   T_limb1: ADC TMP_acc = TMP_A + TMP_B
 *   T_gfl1:  GFL_RR(TMP_acc, TMP_acc)
 *   ... (N-1 ADC+GFL pairs after the initial ADD+GFL) ...
 *   T_final: ADC RAX = RDX + RSI + arch_CF   (RDX=RSI=0; RAX = final chain CF)
 *
 * Same TMP_A, TMP_B reused every limb (uniform values). The
 * accumulator TMP_acc is overwritten each step but its TMP-CF
 * carries the chain.
 *
 * Test patterns for each N:
 *   A: a=FFF, b=1 → every limb wraps with CF=1 → final CF=1
 *   B: a=FFF, b=0 → no overflow ever          → final CF=0
 *   C: a=FFE, b=1 → never wraps (FFF, CF=0)   → final CF=0
 *
 * Tested chain lengths: 4, 8, 16, 32. If all pass for all three
 * patterns at every length, the carry chain scales cleanly to
 * anything microcode fe_mul might need.
 *
 * Build: make PROG=chain_length
 * Run:   sudo taskset -c 0 ./chain_length_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* Build a chain patch of length N (N >= 1). Total triads:
 *   1 (stash a,b)  +  1 (first ADD)  +  1 (first GFL)  +  (N-1)*(ADC+GFL)
 *   + 1 (final ADC for RAX) + 1 (END)
 *   = 4 + 2*(N-1)  triads
 * For N=32 → 66 triads.  Patch cap is 128, fits. */
static void install_chain(int N) {
    init_match_and_patch();
    do_fix_IN_patch();

    int max_triads = 6 + 2 * N;
    ucode_t *p = calloc(max_triads, sizeof(ucode_t));
    int i = 0;

    /* T_setup: TMP14 = R10  (a), TMP15 = R11 (b). */
    p[i].uop0 = ZEROEXT_DSZ64_DR(TMP14, R10);
    p[i].uop1 = ZEROEXT_DSZ64_DR(TMP15, R11);
    p[i].uop2 = NOP;
    p[i].seqw = NOP_SEQWORD;
    i++;

    /* T_limb0: ADD TMP0 = TMP14 + TMP15 (sets TMP0-CF). */
    p[i].uop0 = ADD_DSZ64_DRR(TMP0, TMP14, TMP15);
    p[i].uop1 = NOP;
    p[i].uop2 = NOP;
    p[i].seqw = NOP_SEQWORD;
    i++;
    /* T_gfl0: bridge to arch CF. */
    p[i].uop0 = GENARITHFLAGS_RR(TMP0, TMP0);
    p[i].uop1 = NOP;
    p[i].uop2 = NOP;
    p[i].seqw = NOP_SEQWORD;
    i++;

    /* (N-1) more ADC+GFL pairs, each overwriting TMP0 with new TMP-CF. */
    for (int k = 1; k < N; k++) {
        p[i].uop0 = ADC_DSZ64_DRR(TMP0, TMP14, TMP15);
        p[i].uop1 = NOP;
        p[i].uop2 = NOP;
        p[i].seqw = NOP_SEQWORD;
        i++;
        p[i].uop0 = GENARITHFLAGS_RR(TMP0, TMP0);
        p[i].uop1 = NOP;
        p[i].uop2 = NOP;
        p[i].seqw = NOP_SEQWORD;
        i++;
    }

    /* T_final: ADC RAX = RDX + RSI + arch_CF. RDX, RSI = 0 from wrapper. */
    p[i].uop0 = ADC_DSZ64_DRR(RAX, RDX, RSI);
    p[i].uop1 = NOP;
    p[i].uop2 = NOP;
    p[i].seqw = NOP_SEQWORD;
    i++;
    p[i].uop0 = NOP;
    p[i].uop1 = NOP;
    p[i].uop2 = NOP;
    p[i].seqw = END_SEQWORD;
    i++;

    patch_ucode(0x7c00, p, i);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    free(p);
}

/* Wrapper: R10=a, R11=b, RDX=0, RSI=0, RAX=0, entry CF=0. Return RAX. */
static uint64_t fire(uint64_t a, uint64_t b) {
    uint64_t res;
    asm volatile(
        "mov  r10, %[a]\n\t"
        "mov  r11, %[b]\n\t"
        "xor  rdx, rdx\n\t"
        "xor  rsi, rsi\n\t"
        "xor  rax, rax\n\t"
        "mov  rbx, 1\n\t"        /* vmwrite dummy */
        "mov  rcx, 1\n\t"
        "push 2\n\t"
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  %[res], rax\n\t"
        : [res] "=r"(res)
        : [a] "r"(a), [b] "r"(b)
        : "rax", "rbx", "rcx", "rdx", "rsi",
          "r10", "r11", "cc", "memory"
    );
    return res;
}

typedef struct {
    uint64_t a, b;
    int expect_cf;
    const char *label;
} pat_t;

static pat_t patterns[] = {
    { 0xFFFFFFFFFFFFFFFFULL, 1, 1, "A  a=FFF, b=1   every limb wraps (expect CF=1)" },
    { 0xFFFFFFFFFFFFFFFFULL, 0, 0, "B  a=FFF, b=0   no overflow         (expect CF=0)" },
    { 0xFFFFFFFFFFFFFFFEULL, 1, 0, "C  a=FFE, b=1   FFF, no overflow    (expect CF=0)" },
};

int main(void) {
    printf("============================================================\n");
    printf("  Chain length scaling test: chained ADC + GFL_RR(TMP, TMP)\n");
    printf("============================================================\n\n");
    printf("  Pattern per limb (uniform a, b): ADC TMP_acc = a + b ; GFL_RR(TMP_acc, TMP_acc)\n");
    printf("  After N limbs, ADC RAX = 0 + 0 + arch_CF reports final chain CF.\n\n");

    assign_to_core(0);

    int Ns[] = { 4, 8, 16, 32 };
    int total = 0, fail = 0;

    for (size_t ni = 0; ni < sizeof(Ns)/sizeof(Ns[0]); ni++) {
        int N = Ns[ni];
        printf("--- N = %d limbs ---\n", N);
        install_chain(N);
        for (size_t pi = 0; pi < sizeof(patterns)/sizeof(patterns[0]); pi++) {
            uint64_t got = fire(patterns[pi].a, patterns[pi].b);
            int ok = (int)got == patterns[pi].expect_cf;
            printf("  %-50s  got=%" PRIu64 "  %s\n",
                   patterns[pi].label, got, ok ? "✓" : "✗ FAIL");
            total++;
            if (!ok) fail++;
        }
        printf("\n");
    }

    printf("============================================================\n");
    if (fail == 0)
        printf("  ★ ALL %d probes passed — carry chain scales cleanly to N=32.\n", total);
    else
        printf("  %d/%d probes failed — chain breaks at some length.\n", fail, total);
    printf("============================================================\n");
    return fail ? 1 : 0;
}
