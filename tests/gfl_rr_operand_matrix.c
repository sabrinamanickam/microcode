/*
 * gfl_rr_operand_matrix.c — enumerate GENARITHFLAGS_RR operand combinations
 * to find which forms bridge TMP-CF → arch CF.
 *
 * Established baseline (chain_length.c, arch_gfl_chain.c, 2026-05-22):
 *   GFL_RR(TMP_dest, TMP_dest) — same TMP twice : BRIDGES the prior ADD/ADC's CF.
 *   GFL_RR(arch_dest, arch_dest) — same arch twice : DOES NOT bridge.
 *
 * Hypotheses to discriminate:
 *   H1: bridging requires the destination register name to appear twice (literal
 *       same-operand requirement). Works only for TMPs because only TMPs carry
 *       per-register TMP-CF state.
 *   H2: bridging only requires the destination register name to appear in EITHER
 *       slot. The other slot can hold anything.
 *   H3: bridging only requires a TMP somewhere in the operand list — any TMP
 *       publishes its TMP-CF, even if it's not the destination of the prior ADD.
 *   H4: bridging works only when both operands are the same; specifically the
 *       same register class matters (i.e. for arch-dest chains, GFL_RR(arch, TMP)
 *       might still bridge via the TMP).
 *
 * Tested patterns (4-limb chain each; final ADC RAX = 0+0+arch_CF reports
 * the final chain CF):
 *
 *   P1  dest=TMP, GFL_RR(TMP_dest, TMP_dest)       [baseline good]
 *   P2  dest=arch, GFL_RR(arch_dest, arch_dest)    [baseline bad]
 *   P3  dest=TMP, GFL_RR(TMP_dest, TMP_zero)       diff TMPs, dest in slot 0
 *   P4  dest=TMP, GFL_RR(TMP_zero, TMP_dest)       diff TMPs, dest in slot 1
 *   P5  dest=TMP, GFL_RR(TMP_dest, arch_zero)      TMP dest first, arch second
 *   P6  dest=TMP, GFL_RR(arch_zero, TMP_dest)      arch first, TMP dest second
 *   P7  dest=arch, GFL_RR(arch_dest, TMP_zero)     arch dest, TMP zero second
 *   P8  dest=arch, GFL_RR(TMP_zero, arch_dest)     TMP zero first, arch dest
 *
 * Inputs (uniform per-limb in the chain):
 *   X1  a=FFF, b=1   → every limb wraps   → expect chain CF=1
 *   X2  a=FFF, b=0   → no overflow        → expect chain CF=0
 *   X3  a=FFE, b=1   → result FFF, no ov  → expect chain CF=0
 *
 * Build: make PROG=gfl_rr_operand_matrix
 * Run:   sudo taskset -c 0 ./gfl_rr_operand_matrix_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* Shared setup for chain destinations: TMPs (TMP0..TMP3) or arch (R8..R11).
 * TMP14 = a, TMP15 = b for inputs (always TMPs).
 * TMP9 = TMP zero source. R12 = arch zero source.  */

/* P1: TMP dest, GFL_RR(dest, dest) — baseline good. */
static void install_P1(void) {
    init_match_and_patch(); do_fix_IN_patch();
    ucode_t p[] = {
        { ZEROEXT_DSZ64_DR(TMP14, R10), ZEROEXT_DSZ64_DR(TMP15, R11),
          ZEROEXT_DSZ32_DI(TMP9, 0), NOP_SEQWORD },
        { ADD_DSZ64_DRR(TMP0, TMP14, TMP15), GENARITHFLAGS_RR(TMP0, TMP0),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP1, TMP14, TMP15), GENARITHFLAGS_RR(TMP1, TMP1),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP2, TMP14, TMP15), GENARITHFLAGS_RR(TMP2, TMP2),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP3, TMP14, TMP15), GENARITHFLAGS_RR(TMP3, TMP3),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, RDX, RSI), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* P2: arch dest, GFL_RR(dest, dest) — baseline bad. */
static void install_P2(void) {
    init_match_and_patch(); do_fix_IN_patch();
    ucode_t p[] = {
        { ADD_DSZ64_DRR(R8,  RBX, RCX), GENARITHFLAGS_RR(R8,  R8),  NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(R9,  RBX, RCX), GENARITHFLAGS_RR(R9,  R9),  NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(R10, RBX, RCX), GENARITHFLAGS_RR(R10, R10), NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(R11, RBX, RCX), GENARITHFLAGS_RR(R11, R11), NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, RDX, RSI), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* P3: TMP dest, GFL_RR(TMP_dest, TMP_zero) — different TMPs, dest first. */
static void install_P3(void) {
    init_match_and_patch(); do_fix_IN_patch();
    ucode_t p[] = {
        { ZEROEXT_DSZ64_DR(TMP14, R10), ZEROEXT_DSZ64_DR(TMP15, R11),
          ZEROEXT_DSZ32_DI(TMP9, 0), NOP_SEQWORD },
        { ADD_DSZ64_DRR(TMP0, TMP14, TMP15), GENARITHFLAGS_RR(TMP0, TMP9),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP1, TMP14, TMP15), GENARITHFLAGS_RR(TMP1, TMP9),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP2, TMP14, TMP15), GENARITHFLAGS_RR(TMP2, TMP9),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP3, TMP14, TMP15), GENARITHFLAGS_RR(TMP3, TMP9),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, RDX, RSI), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* P4: TMP dest, GFL_RR(TMP_zero, TMP_dest) — different TMPs, dest in slot 1. */
static void install_P4(void) {
    init_match_and_patch(); do_fix_IN_patch();
    ucode_t p[] = {
        { ZEROEXT_DSZ64_DR(TMP14, R10), ZEROEXT_DSZ64_DR(TMP15, R11),
          ZEROEXT_DSZ32_DI(TMP9, 0), NOP_SEQWORD },
        { ADD_DSZ64_DRR(TMP0, TMP14, TMP15), GENARITHFLAGS_RR(TMP9, TMP0),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP1, TMP14, TMP15), GENARITHFLAGS_RR(TMP9, TMP1),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP2, TMP14, TMP15), GENARITHFLAGS_RR(TMP9, TMP2),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP3, TMP14, TMP15), GENARITHFLAGS_RR(TMP9, TMP3),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, RDX, RSI), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* P5: TMP dest, GFL_RR(TMP_dest, arch_zero) — TMP first, arch second. R12=0 arch zero. */
static void install_P5(void) {
    init_match_and_patch(); do_fix_IN_patch();
    ucode_t p[] = {
        { ZEROEXT_DSZ64_DR(TMP14, R10), ZEROEXT_DSZ64_DR(TMP15, R11),
          NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRR(TMP0, TMP14, TMP15), GENARITHFLAGS_RR(TMP0, R12),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP1, TMP14, TMP15), GENARITHFLAGS_RR(TMP1, R12),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP2, TMP14, TMP15), GENARITHFLAGS_RR(TMP2, R12),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP3, TMP14, TMP15), GENARITHFLAGS_RR(TMP3, R12),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, RDX, RSI), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* P6: TMP dest, GFL_RR(arch_zero, TMP_dest) — arch first, TMP dest second. */
static void install_P6(void) {
    init_match_and_patch(); do_fix_IN_patch();
    ucode_t p[] = {
        { ZEROEXT_DSZ64_DR(TMP14, R10), ZEROEXT_DSZ64_DR(TMP15, R11),
          NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRR(TMP0, TMP14, TMP15), GENARITHFLAGS_RR(R12, TMP0),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP1, TMP14, TMP15), GENARITHFLAGS_RR(R12, TMP1),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP2, TMP14, TMP15), GENARITHFLAGS_RR(R12, TMP2),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP3, TMP14, TMP15), GENARITHFLAGS_RR(R12, TMP3),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, RDX, RSI), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* P7: arch dest, GFL_RR(arch_dest, TMP_zero) — arch dest first, TMP zero second. */
static void install_P7(void) {
    init_match_and_patch(); do_fix_IN_patch();
    ucode_t p[] = {
        { ZEROEXT_DSZ32_DI(TMP9, 0), NOP, NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRR(R8,  RBX, RCX), GENARITHFLAGS_RR(R8,  TMP9), NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(R9,  RBX, RCX), GENARITHFLAGS_RR(R9,  TMP9), NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(R10, RBX, RCX), GENARITHFLAGS_RR(R10, TMP9), NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(R11, RBX, RCX), GENARITHFLAGS_RR(R11, TMP9), NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, RDX, RSI), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* P8: arch dest, GFL_RR(TMP_zero, arch_dest) — TMP zero first, arch dest second. */
static void install_P8(void) {
    init_match_and_patch(); do_fix_IN_patch();
    ucode_t p[] = {
        { ZEROEXT_DSZ32_DI(TMP9, 0), NOP, NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRR(R8,  RBX, RCX), GENARITHFLAGS_RR(TMP9, R8),  NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(R9,  RBX, RCX), GENARITHFLAGS_RR(TMP9, R9),  NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(R10, RBX, RCX), GENARITHFLAGS_RR(TMP9, R10), NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(R11, RBX, RCX), GENARITHFLAGS_RR(TMP9, R11), NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, RDX, RSI), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Fire helpers: TMP-dest patterns use R10/R11 as a/b; arch-dest patterns use RBX/RCX.
 * R12 is loaded with 0 (arch zero) for P5/P6.
 * Both helpers zero R8..R11 first to avoid stale chain-dest state. */
static uint64_t fire_tmp_dest(uint64_t a, uint64_t b) {
    uint64_t res;
    asm volatile(
        "mov  r10, %[a]\n\t"
        "mov  r11, %[b]\n\t"
        "xor  r12, r12\n\t"      /* arch zero source */
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
        : "rax", "rbx", "rcx", "rdx", "rsi",
          "r10", "r11", "r12", "cc", "memory"
    );
    return res;
}

static uint64_t fire_arch_dest(uint64_t a, uint64_t b) {
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

typedef struct {
    uint64_t a, b;
    int expect_cf;
    const char *label;
} pat_t;
static pat_t pats[] = {
    { 0xFFFFFFFFFFFFFFFFULL, 1, 1, "X1  a=FFF, b=1   (wrap → expect CF=1)" },
    { 0xFFFFFFFFFFFFFFFFULL, 0, 0, "X2  a=FFF, b=0   (no overflow      )" },
    { 0xFFFFFFFFFFFFFFFEULL, 1, 0, "X3  a=FFE, b=1   (= FFF, no overflw)" },
};

typedef struct {
    const char *name;
    const char *form;        /* short description of the GFL_RR(arg1, arg2) used */
    void (*install)(void);
    uint64_t (*fire)(uint64_t, uint64_t);
} probe_t;

int main(void) {
    printf("===========================================================\n");
    printf("  GENARITHFLAGS_RR operand matrix — which forms bridge CF?\n");
    printf("===========================================================\n\n");

    assign_to_core(0);

    probe_t probes[] = {
        { "P1 TMP-TMP same",  "GFL_RR(TMP_dest, TMP_dest)",  install_P1, fire_tmp_dest },
        { "P2 arch-arch same","GFL_RR(arch_dest, arch_dest)",install_P2, fire_arch_dest },
        { "P3 TMP-TMPz",      "GFL_RR(TMP_dest, TMP_zero)",  install_P3, fire_tmp_dest },
        { "P4 TMPz-TMP",      "GFL_RR(TMP_zero, TMP_dest)",  install_P4, fire_tmp_dest },
        { "P5 TMP-archz",     "GFL_RR(TMP_dest, arch_zero)", install_P5, fire_tmp_dest },
        { "P6 archz-TMP",     "GFL_RR(arch_zero, TMP_dest)", install_P6, fire_tmp_dest },
        { "P7 arch-TMPz",     "GFL_RR(arch_dest, TMP_zero)", install_P7, fire_arch_dest },
        { "P8 TMPz-arch",     "GFL_RR(TMP_zero, arch_dest)", install_P8, fire_arch_dest },
    };
    int nprobes = sizeof(probes)/sizeof(probes[0]);

    /* matrix: rows = patterns, columns = inputs */
    int got[8][3];
    int bridge[8];   /* 1 if all 3 cases pass for this pattern */

    for (int p = 0; p < nprobes; p++) {
        probes[p].install();
        int all_ok = 1;
        for (int x = 0; x < 3; x++) {
            uint64_t v = probes[p].fire(pats[x].a, pats[x].b);
            got[p][x] = (int)v;
            if ((int)v != pats[x].expect_cf) all_ok = 0;
        }
        bridge[p] = all_ok;
    }

    /* print matrix */
    printf("%-22s  %-35s   X1   X2   X3   verdict\n", "pattern", "GFL_RR form");
    printf("----------------------  -----------------------------------   ---  ---  ---  -------\n");
    for (int p = 0; p < nprobes; p++) {
        printf("%-22s  %-35s   %3d  %3d  %3d  %s\n",
               probes[p].name, probes[p].form,
               got[p][0], got[p][1], got[p][2],
               bridge[p] ? "BRIDGES" : "FAILS  ");
    }
    printf("\n  expected:                                              1    0    0\n\n");

    /* summary */
    printf("===========================================================\n");
    int b = 0;
    for (int p = 0; p < nprobes; p++) if (bridge[p]) b++;
    printf("  %d / %d patterns bridge CF reliably.\n", b, nprobes);
    printf("===========================================================\n");

    for (int p = 0; p < nprobes; p++) {
        if (bridge[p])
            printf("  ✓ %s — %s\n", probes[p].name, probes[p].form);
    }
    return 0;
}
