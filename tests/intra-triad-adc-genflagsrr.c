/*
 * intra-triad-adc-genflagsrr.c — Test GENARITHFLAGS_RR(src0, src1) as the
 * actual CF bridge.
 *
 * Lead: Sabrina/experiments/flag_probes.c uses GENARITHFLAGS_RR(A, B)
 * (two-operand form) after ADD TMP0 = A + B. The hypothesis is that
 * GENARITHFLAGS_RR re-computes A+B inside the flag domain and publishes
 * the arithmetic flags (including CF) to arch RFLAGS. Unlike the single-
 * operand GENARITHFLAGS_R(TMP0) we tested (which only fires when TMP=0),
 * the _RR variant should bridge for general arithmetic.
 *
 * Three patches, all using R64DST=RBX, R64SRC=RCX:
 *
 * Pattern A — separate triads (canonical 128-bit add from adc_findings.md):
 *   T0: ADD TMP0 = R64DST + R64SRC ; GENARITHFLAGS_RR(R64DST, R64SRC)
 *   T1: ADC R64DST = R64SRC + R64SRC + arch_CF
 *   T2: END
 *
 * Pattern B — all three in one triad:
 *   T0: slot 0 = ADD TMP0 = R64DST + R64SRC
 *       slot 1 = GENARITHFLAGS_RR(R64DST, R64SRC)
 *       slot 2 = ADC R64DST = R64SRC + R64SRC + arch_CF
 *   T1: END
 *
 * Pattern C — chained ADCs (the 4×64 question):
 *   T0: ADD TMP0 = R64DST + R64SRC ; GENARITHFLAGS_RR(R64DST, R64SRC)
 *   T1: ADC TMP1 = R64SRC + R64SRC ; GENARITHFLAGS_RR(R64SRC, R64SRC)
 *   T2: ADC R64DST = R64SRC + R64SRC + arch_CF
 *   T3: END
 *
 * Probe (all patterns):
 *   RBX = 0xFFFFFFFFFFFFFFFF, RCX = 1, entry arch CF = 0
 *   slot-0 ADD: 0xFFFF…FFFF + 1 = 0  (overflow → CF=1)
 *   final ADC: 1 + 1 + arch_CF
 *
 * Pattern A and B detector:
 *   RBX_out = 3  → GENARITHFLAGS_RR bridged the carry (1+1+1)
 *   RBX_out = 2  → no bridge
 *
 * Pattern C detector (only meaningful if A/B passed):
 *   GENARITHFLAGS_RR(RCX, RCX) regenerates flags for 1+1 (no overflow → CF=0).
 *   So if the bridge resets arch CF each time, the final ADC sees CF=0,
 *   not the carry from the ORIGINAL ADD.
 *   RBX_out = 2 → arch CF was overwritten by T1's GENARITHFLAGS_RR
 *   RBX_out = 3 → arch CF retained from T0 (T1's didn't fire)
 *
 * Build: make PROG=intra-triad-adc-genflagsrr
 * Run:   sudo taskset -c 0 ./intra-triad-adc-genflagsrr_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

static void install_A(void) {
    /* Separate-triad GENARITHFLAGS_RR bridge. */
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R64DST, R64SRC),
          GENARITHFLAGS_RR(R64DST, R64SRC),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(R64DST, R64SRC, R64SRC),
          NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_B(void) {
    /* All three in one triad. */
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R64DST, R64SRC),
          GENARITHFLAGS_RR(R64DST, R64SRC),
          ADC_DSZ64_DRR(R64DST, R64SRC, R64SRC), NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_C(void) {
    /* Chained ADCs to test if subsequent GENARITHFLAGS_RR overwrites arch CF.
     *
     * T0 ADD: 0xFFFF…FFFF + 1 = 0, CF=1. GENARITHFLAGS_RR(RBX, RCX) should
     *         publish arch CF=1.
     * T1 ADC: TMP1 = RCX + RCX + arch_CF = 1+1+1 = 3, no CF-out.
     *         GENARITHFLAGS_RR(RCX, RCX) recomputes flags for 1+1 = 2,
     *         no overflow → arch CF=0.
     * T2 ADC: RBX = 1+1+0 = 2. So RBX_out = 2 indicates T1's bridge cleared CF.
     *         If T1's GENARITHFLAGS_RR was a NOP (didn't fire), arch CF
     *         from T0 persists, RBX = 3. */
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R64DST, R64SRC),
          GENARITHFLAGS_RR(R64DST, R64SRC),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP1, R64SRC, R64SRC),
          GENARITHFLAGS_RR(R64SRC, R64SRC),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(R64DST, R64SRC, R64SRC),
          NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static uint64_t fire(uint64_t a, uint64_t b, int cf_in) {
    uint64_t res;
    uint64_t flags = cf_in ? 0x3ULL : 0x2ULL;
    asm volatile(
        "mov  rbx, %[a]\n\t"
        "mov  rcx, %[b]\n\t"
        "push %[flg]\n\t"
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  %[res], rbx\n\t"
        : [res] "=r"(res)
        : [a] "r"(a), [b] "r"(b), [flg] "r"(flags)
        : "rbx", "rcx", "cc", "memory"
    );
    return res;
}

int main(void) {
    printf("============================================================\n");
    printf("  GENARITHFLAGS_RR as CF bridge\n");
    printf("============================================================\n\n");
    printf("  Inputs (all patterns): RBX=0xFFFF…FFFF, RCX=1, entry CF=0\n");
    printf("  Pattern A/B: RBX_out=3 means GENARITHFLAGS_RR bridged the carry\n");
    printf("  Pattern C: RBX_out=2 if mid-chain GENARITHFLAGS_RR resets CF,\n");
    printf("             RBX_out=3 if T1's GENARITHFLAGS_RR was a NOP\n\n");

    assign_to_core(0);

    install_A();
    uint64_t a = fire(0xFFFFFFFFFFFFFFFFULL, 1, 0);
    printf("  Pattern A (separate triads)        RBX_out = %" PRIu64 "  %s\n",
           a,
           a == 3 ? "★ GENARITHFLAGS_RR DOES bridge CF" :
           a == 2 ? "no bridge" :
                    "unexpected");

    install_B();
    uint64_t b = fire(0xFFFFFFFFFFFFFFFFULL, 1, 0);
    printf("  Pattern B (all in one triad)       RBX_out = %" PRIu64 "  %s\n",
           b,
           b == 3 ? "★ intra-triad bridge works" :
           b == 2 ? "intra-triad bridge does NOT work" :
                    "unexpected");

    install_C();
    uint64_t c = fire(0xFFFFFFFFFFFFFFFFULL, 1, 0);
    printf("  Pattern C (chained ADCs)           RBX_out = %" PRIu64 "  %s\n",
           c,
           c == 3 ? "T1 bridge was a NOP — chaining broken (CF leaked through)" :
           c == 2 ? "T1 bridge overwrites CF — needs per-step bridging" :
                    "unexpected");

    printf("\n");
    printf("============================================================\n");
    printf("  Decision:\n");
    printf("    Pattern A=3 → GENARITHFLAGS_RR is a real bridge for ADD's CF.\n");
    printf("                  Useful for one-shot carry between ADD and ADC.\n");
    printf("    Pattern B=3 → intra-triad bridging works — 1 triad per limb-step.\n");
    printf("    Pattern C: tells us whether arch CF can be re-set between ADCs.\n");
    printf("               If B=3 and C=2, we can chain with 2-op bridges per limb.\n");
    printf("============================================================\n");
    return 0;
}
