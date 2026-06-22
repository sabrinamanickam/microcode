/*
 * arch_gfl_chain.c — Probe whether GENARITHFLAGS_RR(arch, arch) bridges CF
 * across triads as reliably as the TMP-only chain.
 *
 * Established (chain_length.c):
 *   ADD/ADC TMP + GFL_RR(TMP, TMP) chains scale cleanly to 32+ limbs (TMP-only).
 *
 * Under test here: same pattern but the chain runs on ARCH registers.
 * Suspicion (from the fe_mul rewrite failure 2026-05-22): arch-reg GFL_RR
 * may behave differently across triads when one operand is an arch reg.
 *
 * Two patches built, same algorithm, different reg classes:
 *
 *   Pattern T (TMP-only):
 *     setup:    TMP_A = R10 (a), TMP_B = R11 (b)
 *     limb 0:   ADD TMP_acc0 = TMP_A + TMP_B ; GFL_RR(TMP_acc0, TMP_acc0)
 *     limb 1:   ADC TMP_acc1 = TMP_A + TMP_B ; GFL_RR(TMP_acc1, TMP_acc1)
 *     limb 2:   ADC TMP_acc2 = TMP_A + TMP_B ; GFL_RR(TMP_acc2, TMP_acc2)
 *     limb 3:   ADC TMP_acc3 = TMP_A + TMP_B ; GFL_RR(TMP_acc3, TMP_acc3)
 *     final:    ADC RAX = RDX + RSI + arch_CF   (RDX=RSI=0, RAX = final CF)
 *
 *   Pattern A (arch destinations, distinct registers per limb):
 *     setup:    RBX = a, RCX = b
 *     limb 0:   ADD R8  = RBX + RCX ; GFL_RR(R8, R8)
 *     limb 1:   ADC R9  = RBX + RCX ; GFL_RR(R9, R9)
 *     limb 2:   ADC R10 = RBX + RCX ; GFL_RR(R10, R10)
 *     limb 3:   ADC R11 = RBX + RCX ; GFL_RR(R11, R11)
 *     final:    ADC RAX = RDX + RSI + arch_CF
 *
 * If both produce identical RAX for the same inputs, arch-GFL_RR bridges fine.
 * If only Pattern T works, we know to keep the carry chain entirely in TMPs.
 *
 * Patterns tested (4-limb chain, uniform inputs):
 *   X1: a=FFF, b=1 → every limb wraps, final CF=1
 *   X2: a=FFF, b=0 → no wraps, final CF=0
 *   X3: a=FFE, b=1 → never wraps (FFF), final CF=0
 *
 * Build: make PROG=arch_gfl_chain
 * Run:   sudo taskset -c 0 ./arch_gfl_chain_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* Pattern T: chain entirely through TMPs (the known-good baseline). */
static void install_tmp_chain(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        /* Stash: TMP14 = R10 (a), TMP15 = R11 (b) */
        { ZEROEXT_DSZ64_DR(TMP14, R10), ZEROEXT_DSZ64_DR(TMP15, R11),
          NOP, NOP_SEQWORD },
        /* limb 0: ADD TMP0 = TMP14 + TMP15 ; GFL */
        { ADD_DSZ64_DRR(TMP0, TMP14, TMP15), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(TMP0, TMP0), NOP, NOP, NOP_SEQWORD },
        /* limb 1 */
        { ADC_DSZ64_DRR(TMP1, TMP14, TMP15), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(TMP1, TMP1), NOP, NOP, NOP_SEQWORD },
        /* limb 2 */
        { ADC_DSZ64_DRR(TMP2, TMP14, TMP15), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(TMP2, TMP2), NOP, NOP, NOP_SEQWORD },
        /* limb 3 */
        { ADC_DSZ64_DRR(TMP3, TMP14, TMP15), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(TMP3, TMP3), NOP, NOP, NOP_SEQWORD },
        /* final: RAX = RDX + RSI + arch_CF (RDX=RSI=0 → RAX = final CF) */
        { ADC_DSZ64_DRR(RAX, RDX, RSI), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Pattern A: chain through distinct arch destinations. */
static void install_arch_chain(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        /* No stash needed — RBX, RCX hold a, b directly */
        /* limb 0: ADD R8 = RBX + RCX ; GFL */
        { ADD_DSZ64_DRR(R8, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(R8, R8), NOP, NOP, NOP_SEQWORD },
        /* limb 1: ADC R9 = RBX + RCX ; GFL */
        { ADC_DSZ64_DRR(R9, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(R9, R9), NOP, NOP, NOP_SEQWORD },
        /* limb 2 */
        { ADC_DSZ64_DRR(R10, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(R10, R10), NOP, NOP, NOP_SEQWORD },
        /* limb 3 */
        { ADC_DSZ64_DRR(R11, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(R11, R11), NOP, NOP, NOP_SEQWORD },
        /* final */
        { ADC_DSZ64_DRR(RAX, RDX, RSI), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Pattern A2: same as A but GFL_RR + ADC packed into ONE triad each.
 * This is the pattern my fe_mul actually used.
 *   limb k: { ADC R = RBX + RCX, GFL_RR(R, R), NOP, NOP_SEQWORD }
 * Tests whether the intra-triad ADC→GFL pairing on arch dests holds.
 */
static void install_arch_packed_chain(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        /* limb 0: ADD + GFL packed */
        { ADD_DSZ64_DRR(R8, RBX, RCX), GENARITHFLAGS_RR(R8, R8),
          NOP, NOP_SEQWORD },
        /* limb 1: ADC + GFL packed */
        { ADC_DSZ64_DRR(R9, RBX, RCX), GENARITHFLAGS_RR(R9, R9),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(R10, RBX, RCX), GENARITHFLAGS_RR(R10, R10),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(R11, RBX, RCX), GENARITHFLAGS_RR(R11, R11),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, RDX, RSI), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Fire and read RAX. R10=a, R11=b for TMP version; RBX=a, RCX=b for arch. */
static uint64_t fire_tmp(uint64_t a, uint64_t b) {
    uint64_t res;
    asm volatile(
        "mov  r10, %[a]\n\t"
        "mov  r11, %[b]\n\t"
        "xor  rdx, rdx\n\t"
        "xor  rsi, rsi\n\t"
        "xor  rax, rax\n\t"
        "mov  rbx, 1\n\t"
        "mov  rcx, 1\n\t"
        "push 2\n\t"
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  %[res], rax\n\t"
        : [res] "=r"(res)
        : [a] "r"(a), [b] "r"(b)
        : "rax", "rbx", "rcx", "rdx", "rsi", "r10", "r11", "cc", "memory"
    );
    return res;
}

static uint64_t fire_arch(uint64_t a, uint64_t b) {
    uint64_t res;
    asm volatile(
        "mov  rbx, %[a]\n\t"
        "mov  rcx, %[b]\n\t"
        "xor  rdx, rdx\n\t"
        "xor  rsi, rsi\n\t"
        "xor  rax, rax\n\t"
        "xor  r8,  r8\n\t"
        "xor  r9,  r9\n\t"
        "xor  r10, r10\n\t"
        "xor  r11, r11\n\t"
        "push 2\n\t"
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  %[res], rax\n\t"
        : [res] "=r"(res)
        : [a] "r"(a), [b] "r"(b)
        : "rax", "rbx", "rcx", "rdx", "rsi",
          "r8", "r9", "r10", "r11", "cc", "memory"
    );
    return res;
}

typedef struct { uint64_t a, b; int expect_cf; const char *label; } pat_t;
static pat_t patterns[] = {
    { 0xFFFFFFFFFFFFFFFFULL, 1, 1, "X1  a=FFF, b=1   every limb wraps (expect CF=1)" },
    { 0xFFFFFFFFFFFFFFFFULL, 0, 0, "X2  a=FFF, b=0   no overflow         (expect CF=0)" },
    { 0xFFFFFFFFFFFFFFFEULL, 1, 0, "X3  a=FFE, b=1   FFF, no overflow    (expect CF=0)" },
};

int main(void) {
    printf("=========================================================\n");
    printf("  Arch-reg ADC+GFL_RR chain vs TMP-only chain (4 limbs)\n");
    printf("=========================================================\n\n");

    assign_to_core(0);

    int total = 0, fail_tmp = 0, fail_arch = 0, fail_packed = 0;

    /* Run Pattern T first to make sure TMP-only still works in this harness. */
    printf("--- Pattern T: TMP-only chain (baseline) ---\n");
    install_tmp_chain();
    for (size_t i = 0; i < sizeof(patterns)/sizeof(patterns[0]); i++) {
        uint64_t got = fire_tmp(patterns[i].a, patterns[i].b);
        int ok = (int)got == patterns[i].expect_cf;
        printf("  %-50s  got=%" PRIu64 "  %s\n",
               patterns[i].label, got, ok ? "PASS" : "FAIL");
        total++; if (!ok) fail_tmp++;
    }

    printf("\n--- Pattern A: arch destinations, ADC and GFL on separate triads ---\n");
    install_arch_chain();
    for (size_t i = 0; i < sizeof(patterns)/sizeof(patterns[0]); i++) {
        uint64_t got = fire_arch(patterns[i].a, patterns[i].b);
        int ok = (int)got == patterns[i].expect_cf;
        printf("  %-50s  got=%" PRIu64 "  %s\n",
               patterns[i].label, got, ok ? "PASS" : "FAIL");
        total++; if (!ok) fail_arch++;
    }

    printf("\n--- Pattern A2: arch destinations, ADC+GFL PACKED in same triad ---\n");
    printf("    (this is the exact pattern the fe_mul rewrite used)\n");
    install_arch_packed_chain();
    for (size_t i = 0; i < sizeof(patterns)/sizeof(patterns[0]); i++) {
        uint64_t got = fire_arch(patterns[i].a, patterns[i].b);
        int ok = (int)got == patterns[i].expect_cf;
        printf("  %-50s  got=%" PRIu64 "  %s\n",
               patterns[i].label, got, ok ? "PASS" : "FAIL");
        total++; if (!ok) fail_packed++;
    }

    printf("\n=========================================================\n");
    printf("  Pattern T (TMP-only):     %d / 3 passed\n", 3 - fail_tmp);
    printf("  Pattern A (arch, split):  %d / 3 passed\n", 3 - fail_arch);
    printf("  Pattern A2 (arch, packed): %d / 3 passed\n", 3 - fail_packed);
    printf("\n  Total: %d / %d passed\n", total - fail_tmp - fail_arch - fail_packed, total);
    printf("=========================================================\n");

    if (fail_tmp == 0 && (fail_arch || fail_packed)) {
        printf("\n  CONFIRMED: arch-reg chains LEAK CF — primitive is TMP-only.\n");
    } else if (fail_tmp == 0 && fail_arch == 0 && fail_packed == 0) {
        printf("\n  arch-reg chains work too — the fe_mul bug is elsewhere.\n");
    }

    return (fail_tmp + fail_arch + fail_packed) ? 1 : 0;
}
