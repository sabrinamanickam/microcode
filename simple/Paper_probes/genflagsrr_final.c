/*
 * genflagsrr_final.c — Final, definitive GENARITHFLAGS_RR probe.
 *
 * We know from readaflags_probe.c P4 that GENARITHFLAGS_R(TMP) DOES
 * write some arch-flag register that READAFLAGS reads back — including
 * a CF=1 bit when the overflow case was TMP=0 with TMP-CF=1.
 *
 * We also know from intra-triad-adc-genflagsrr.c that the same
 * GENARITHFLAGS_RR with R64DST/R64SRC operands does NOT update what
 * ADC reads. That test used the macroinstruction-operand encoding.
 *
 * This final test fires GENARITHFLAGS_RR with EXPLICIT arch reg IDs
 * (RBX=0x23, RCX=0x21) — exactly like Sabrina/experiments/flag_probes.c
 * probes 1b/2/3/4 use it — and follows with ADC to check whether the
 * carry reaches ADC's port.
 *
 * Pattern:
 *   T0: GENARITHFLAGS_RR(RBX, RCX)             ; compute and publish flags for RBX+RCX
 *   T1: ADC RAX = R8 + R9 + arch_CF            ; R8=R9=0; RAX_out = arch CF that ADC saw
 *   T2: END
 *
 * Wrapper sets R8=R9=0, RAX=0, RBX and RCX to inputs, entry CF=0 via popfq.
 *
 * Five inputs sweep the discriminator probes:
 *   no overflow                                              → expect RAX=0
 *   overflow, RBX+RCX = 0 (TMP wraps to 0)                   → if bridge works, RAX=1
 *   overflow, RBX+RCX = 1 (DISCRIMINATOR — TMP ≠ 0)          → if real CF bridge, RAX=1
 *   overflow, RBX+RCX = random non-zero (DISCRIMINATOR)      → if real CF bridge, RAX=1
 *   no overflow, RBX+RCX = 0 (both zero)                     → expect RAX=0
 *
 * Build: make PROG=genflagsrr_final
 * Run:   sudo taskset -c 0 ./genflagsrr_final_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

static void install(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        /* GENARITHFLAGS_RR with EXPLICIT arch reg IDs (RBX=0x23, RCX=0x21).
         * This is the form flag_probes.c uses. */
        { GENARITHFLAGS_RR(RBX, RCX), NOP, NOP, NOP_SEQWORD },
        /* ADC RAX = R8 + R9 + arch_CF. R8=R9=0, so RAX = arch CF. */
        { ADC_DSZ64_DRR(RAX, R8, R9), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Set RBX=a, RCX=b, R8=R9=0, entry CF=0. Read RAX after patch. */
static uint64_t fire(uint64_t a, uint64_t b) {
    uint64_t res;
    asm volatile(
        "mov  rbx, %[a]\n\t"
        "mov  rcx, %[b]\n\t"
        "xor  r8,  r8\n\t"          /* R8=0  */
        "xor  r9,  r9\n\t"          /* R9=0  */
        "xor  rax, rax\n\t"         /* RAX=0 (XOR happens BEFORE popfq) */
        "push 2\n\t"                /* entry CF=0; LAST flag op before vmwrite */
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  %[res], rax\n\t"
        : [res] "=r"(res)
        : [a] "r"(a), [b] "r"(b)
        : "rax", "rbx", "rcx", "rdx", "r8", "r9", "cc", "memory"
    );
    return res;
}

int main(void) {
    printf("============================================================\n");
    printf("  GENARITHFLAGS_RR(RBX, RCX) — definitive ADC bridge probe\n");
    printf("============================================================\n\n");
    printf("  Pattern: GENARITHFLAGS_RR(RBX, RCX) ; ADC RAX = R8 + R9 + arch_CF\n");
    printf("  With R8=R9=0, RAX_out IS the arch CF that ADC observed.\n\n");

    assign_to_core(0);
    install();

    struct {
        uint64_t a, b;
        int true_cf;
        const char *label;
    } cases[] = {
        { 1,                     1,                     0, "1 + 1 (no overflow)" },
        { 0xFFFFFFFFFFFFFFFFULL, 1,                     1, "0xFFFF…FF + 1 (overflow, TMP=0)" },
        { 0xFFFFFFFFFFFFFFFEULL, 3,                     1, "0xFFFF…FE + 3 (overflow, TMP=1 DISCRIMINATOR)" },
        { 0xDEADBEEF12345678ULL, 0xCAFEFEEDABCDEF01ULL, 1, "DEAD + CAFE (overflow, TMP=random DISCRIMINATOR)" },
        { 0x8000000000000000ULL, 0x8000000000000000ULL, 1, "0x8000…0 + 0x8000…0 (overflow, TMP=0)" },
        { 0,                     0,                     0, "0 + 0 (no overflow)" },
        { 5,                     0,                     0, "5 + 0 (no overflow, TMP=5)" },
    };

    int h1_consistent = 1;  /* real CF bridge */
    int h2_consistent = 1;  /* (TMP==0 AND CF==1) only */

    printf("  %-55s  RAX  true CF  pattern matches\n", "case");
    printf("  -------------------------------------------------------  ---  -------  ----------------\n");
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint64_t got = fire(cases[i].a, cases[i].b);
        uint64_t sum_lo = cases[i].a + cases[i].b;
        int h1 = cases[i].true_cf;
        int h2 = (cases[i].true_cf == 1 && sum_lo == 0) ? 1 : 0;
        const char *match;
        if      ((int)got == h1 && (int)got == h2) match = "(matches both)";
        else if ((int)got == h1)                   match = "★ matches H1 (real CF bridge)";
        else if ((int)got == h2)                   match = "matches H2 (TMP==0 only)";
        else                                        match = "matches NEITHER";
        if ((int)got != h1) h1_consistent = 0;
        if ((int)got != h2) h2_consistent = 0;
        printf("  %-55s  %3" PRIu64 "  %4d     %s\n",
               cases[i].label, got, cases[i].true_cf, match);
    }

    printf("\n");
    printf("============================================================\n");
    if (h1_consistent && !h2_consistent) {
        printf("  ★ GENARITHFLAGS_RR(RBX, RCX) IS a real CF bridge for ADC.\n");
        printf("    This contradicts the prior negative findings. Use this!\n");
    } else if (!h1_consistent && h2_consistent) {
        printf("  GENARITHFLAGS_RR(RBX, RCX) has the same (TMP==0 only) quirk\n");
        printf("  as the 1-arg form. Not a bridge for general arithmetic.\n");
    } else if (h1_consistent && h2_consistent) {
        printf("  Test cases didn't fully discriminate. Probably both H1 and H2\n");
        printf("  return RAX=0 across the board (no writes happening at all).\n");
    } else {
        printf("  Behaviour matches neither H1 nor H2. Unknown semantics —\n");
        printf("  read the data row by row.\n");
    }
    printf("============================================================\n");
    return 0;
}
