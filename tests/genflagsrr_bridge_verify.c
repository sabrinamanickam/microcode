/*
 * genflagsrr_bridge_verify.c — Confirm GENARITHFLAGS_RR(TMP, TMP) is a
 * real CF bridge from Domain #1 (TMP-CF) to Domain #2b (ADC carry-in),
 * and test whether ADC writes TMP-CF for its destination (chainability).
 *
 * Discovery (genflagsrr_exhaustive.c row C1):
 *   T0: ADD TMP0 = RBX + RCX
 *   T1: GENARITHFLAGS_RR(TMP0, TMP0)    ← same TMP, twice
 *   T2: ADC RAX = R8 + R9 + arch_CF
 * On the discriminator probe (overflow, TMP=1), this returned RAX=1
 * — meaning ADC saw arch CF=1. No other GENARITHFLAGS form bridged
 * the carry. The "same TMP twice" form behaves DIFFERENTLY from any
 * other operand encoding.
 *
 * This file does two things:
 *
 *  TEST 1 (generality)
 *    Run the C1 pattern on 8 inputs spanning all relevant cases:
 *    overflow vs not, TMP=0 vs TMP≠0, etc. For a real bridge, the
 *    arch CF that ADC reads should always equal the ADD's true CF.
 *
 *  TEST 2 (chainability — the make-or-break question for 4×64)
 *    Two ADCs chained:
 *      T0: ADD TMP0 = a + b
 *      T1: GFL_RR(TMP0, TMP0)          ← arch CF = ADD's CF
 *      T2: ADC TMP1 = c + d + arch_CF  ← does ADC write TMP1-CF?
 *      T3: GFL_RR(TMP1, TMP1)          ← arch CF = ADC's CF-out?
 *      T4: ADC RAX = R8 + R9 + arch_CF ← RAX = chained CF
 *    Two probes:
 *      (a) ADC OVERFLOWS  → RAX should be 1
 *      (b) ADC does not   → RAX should be 0
 *    If both pass, ADC writes TMP-CF and we can chain — 4×64 microcode
 *    becomes viable with 2 ops/limb (GFL + ADC) instead of 3 (SETCC dance).
 *
 * Build: make PROG=genflagsrr_bridge_verify
 * Run:   sudo taskset -c 0 ./genflagsrr_bridge_verify_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* ────── TEST 1: generality of GFL_RR(TMP,TMP) as bridge ───── */

static void install_T1(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(TMP0, TMP0),  NOP, NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, R8, R9),    NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* ────── TEST 2: chained ADC via two GFL_RR(TMP,TMP) bridges ─ */

/* We need different inputs for the two ADDs/ADCs. Stash a, b in RBX, RCX
 * (vmwrite operands) and c, d in R10, R11 (set by wrapper before vmwrite). */
static void install_T2(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        /* T0: TMP0 = a + b  (set TMP0-CF) */
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        /* T1: bridge to arch CF */
        { GENARITHFLAGS_RR(TMP0, TMP0),  NOP, NOP, NOP_SEQWORD },
        /* T2: TMP1 = c + d + arch_CF (= a+b CF) — second ADC */
        { ADC_DSZ64_DRR(TMP1, R10, R11), NOP, NOP, NOP_SEQWORD },
        /* T3: bridge TMP1's CF (the second ADC's carry-out) to arch */
        { GENARITHFLAGS_RR(TMP1, TMP1),  NOP, NOP, NOP_SEQWORD },
        /* T4: RAX = 0 + 0 + arch_CF (= second ADC's CF if ADC writes TMP-CF) */
        { ADC_DSZ64_DRR(RAX, R8, R9),    NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* ───── wrappers ───── */

/* T1 wrapper: RBX=a, RCX=b, R8=R9=0, entry CF=0. Return RAX. */
static uint64_t fire_T1(uint64_t a, uint64_t b) {
    uint64_t res;
    asm volatile(
        "mov  rbx, %[a]\n\t"
        "mov  rcx, %[b]\n\t"
        "xor  r8,  r8\n\t"
        "xor  r9,  r9\n\t"
        "xor  rax, rax\n\t"
        "push 2\n\t"
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  %[res], rax\n\t"
        : [res] "=r"(res)
        : [a] "r"(a), [b] "r"(b)
        : "rax", "rbx", "rcx", "rdx", "r8", "r9", "cc", "memory"
    );
    return res;
}

/* T2 wrapper: RBX=a, RCX=b, R10=c, R11=d, R8=R9=0, entry CF=0. Return RAX. */
static uint64_t fire_T2(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    uint64_t res;
    asm volatile(
        "mov  rbx, %[a]\n\t"
        "mov  rcx, %[b]\n\t"
        "mov  r10, %[c]\n\t"
        "mov  r11, %[d]\n\t"
        "xor  r8,  r8\n\t"
        "xor  r9,  r9\n\t"
        "xor  rax, rax\n\t"
        "push 2\n\t"
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  %[res], rax\n\t"
        : [res] "=r"(res)
        : [a] "r"(a), [b] "r"(b), [c] "r"(c), [d] "r"(d)
        : "rax", "rbx", "rcx", "rdx",
          "r8", "r9", "r10", "r11",
          "cc", "memory"
    );
    return res;
}

int main(void) {
    printf("============================================================\n");
    printf("  Verify GENARITHFLAGS_RR(TMP, TMP) as a real CF bridge\n");
    printf("============================================================\n\n");

    assign_to_core(0);

    /* ────── TEST 1 ────── */
    printf("TEST 1: bridge generality\n");
    printf("  Pattern: ADD TMP0 = RBX+RCX ; GFL_RR(TMP0, TMP0) ; ADC RAX=0+0+CF\n\n");
    install_T1();

    struct {
        uint64_t a, b;
        int true_cf;
        const char *label;
    } t1[] = {
        { 1,                     1,                     0, "1 + 1                   (no overflow, TMP=2)" },
        { 0,                     0,                     0, "0 + 0                   (no overflow, TMP=0)" },
        { 5,                     0,                     0, "5 + 0                   (no overflow, TMP=5)" },
        { 0xFFFFFFFFFFFFFFFFULL, 1,                     1, "0xFFFF…FF + 1           (overflow, TMP=0)" },
        { 0xFFFFFFFFFFFFFFFEULL, 3,                     1, "0xFFFF…FE + 3           (overflow, TMP=1) ←" },
        { 0xFFFFFFFFFFFFFFFEULL, 5,                     1, "0xFFFF…FE + 5           (overflow, TMP=3) ←" },
        { 0xDEADBEEF12345678ULL, 0xCAFEFEEDABCDEF01ULL, 1, "0xDEAD… + 0xCAFE…       (overflow, TMP=random) ←" },
        { 0x8000000000000000ULL, 0x8000000000000000ULL, 1, "0x8000…0 + 0x8000…0     (overflow, TMP=0)" },
    };

    int t1_pass = 0, t1_total = 0;
    for (size_t i = 0; i < sizeof(t1)/sizeof(t1[0]); i++) {
        uint64_t got = fire_T1(t1[i].a, t1[i].b);
        int ok = (int)got == t1[i].true_cf;
        if (ok) t1_pass++;
        t1_total++;
        printf("  %-55s  true CF=%d  got RAX=%" PRIu64 "  %s\n",
               t1[i].label, t1[i].true_cf, got, ok ? "✓" : "✗ FAIL");
    }
    printf("  TEST 1 result: %d/%d  ", t1_pass, t1_total);
    if (t1_pass == t1_total) printf("★ GFL_RR(TMP,TMP) IS a real CF bridge\n");
    else                      printf("✗ inconsistent — not a general bridge\n");
    printf("\n");

    /* ────── TEST 2 ────── */
    printf("TEST 2: ADC writes TMP-CF? (chainability)\n");
    printf("  Pattern: ADD ; GFL_RR(TMP0,TMP0) ; ADC TMP1=c+d+CF ;\n");
    printf("           GFL_RR(TMP1,TMP1) ; ADC RAX=0+0+CF\n\n");
    install_T2();

    /* First ADD always overflows (a + b = 0, CF=1) — gives the second ADC a CF=1.
     * The second ADC then either overflows or doesn't, depending on c, d. */
    struct {
        uint64_t a, b, c, d;
        int expect_rax;
        const char *label;
    } t2[] = {
        { 0xFFFFFFFFFFFFFFFFULL, 1, 0xFFFFFFFFFFFFFFFFULL, 0, 1,
          "ADD overflow, ADC c=0xFF…F + 0 + 1 → 0 (overflow, expect RAX=1)" },
        { 0xFFFFFFFFFFFFFFFFULL, 1, 5, 3, 0,
          "ADD overflow, ADC c=5+3+1 = 9 (no overflow, expect RAX=0)" },
        { 0xFFFFFFFFFFFFFFFFULL, 1, 0xFFFFFFFFFFFFFFFEULL, 0, 0,
          "ADD overflow, ADC c=0xFF…FE+0+1 = 0xFF…FF (no overflow, RAX=0)" },
        { 1, 1, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 1,
          "ADD no-overflow, ADC = 0xFF…F+0xFF…F+0 = 0xFF…FE (overflow, RAX=1)" },
        { 1, 1, 5, 3, 0,
          "ADD no-overflow, ADC = 5+3+0 = 8 (no overflow, RAX=0)" },
        { 0xFFFFFFFFFFFFFFFEULL, 3, 0xFFFFFFFFFFFFFFFEULL, 0, 1,
          "ADD overflow TMP=1, ADC 0xFF…FE+0+1 = 0xFF…FF (no overflow, RAX=?)" },
    };

    int t2_pass = 0, t2_total = 0;
    for (size_t i = 0; i < sizeof(t2)/sizeof(t2[0]); i++) {
        uint64_t got = fire_T2(t2[i].a, t2[i].b, t2[i].c, t2[i].d);
        /* Re-derive expected RAX from the actual arithmetic */
        uint64_t lo_after_first_add = t2[i].a + t2[i].b;
        int first_cf = (lo_after_first_add < t2[i].a) ? 1 : 0;
        uint64_t lo_after_adc = t2[i].c + t2[i].d + first_cf;
        int adc_cf = (lo_after_adc < t2[i].c) ||
                     (lo_after_adc == t2[i].c && (t2[i].d || first_cf));
        /* careful overflow detection */
        __uint128_t sum128 = (__uint128_t)t2[i].c + t2[i].d + first_cf;
        adc_cf = (sum128 >> 64) ? 1 : 0;
        int ok = (int)got == adc_cf;
        if (ok) t2_pass++;
        t2_total++;
        printf("  %s\n", t2[i].label);
        printf("    derived: 1st CF=%d, 2nd ADC CF=%d, got RAX=%" PRIu64 "  %s\n",
               first_cf, adc_cf, got, ok ? "✓" : "✗ FAIL");
    }
    printf("  TEST 2 result: %d/%d  ", t2_pass, t2_total);
    if (t2_pass == t2_total) printf("★ ADC writes TMP-CF — chaining WORKS!\n");
    else                      printf("✗ ADC does not write TMP-CF in a usable way\n");
    printf("\n");

    /* ────── verdict ────── */
    printf("============================================================\n");
    if (t1_pass == t1_total && t2_pass == t2_total) {
        printf("  ★★★ BOTH TESTS PASS ★★★\n");
        printf("  GFL_RR(TMP, TMP) is a real Domain-#1 → Domain-#2b bridge,\n");
        printf("  AND ADC writes TMP-CF for its destination.\n");
        printf("  → 4×64 microcode with chained ADCs IS VIABLE.\n");
        printf("  → Per-limb cost: 2 ops (GFL_RR + ADC) vs 3 (SETCC dance).\n");
    } else if (t1_pass == t1_total) {
        printf("  Bridge works (Test 1 PASS) but ADC doesn't write TMP-CF.\n");
        printf("  ADC is still one-shot, but at least we can prime it with\n");
        printf("  any ADD-derived carry now — no need for x86 popfq.\n");
    } else if (t2_pass == t2_total) {
        printf("  Anomaly: chaining works but bridge isn't general. Inspect data.\n");
    } else {
        printf("  Neither test fully passed. The C1 result may have been an\n");
        printf("  artifact specific to that one input. Read data row by row.\n");
    }
    printf("============================================================\n");
    return 0;
}
