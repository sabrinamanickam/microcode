/*
 * test_adc_gfl_adc_intratriad.c — does packing chain steps inside one triad
 * preserve the CF bridge for a chained ADC?
 *
 * The chained-ADC primitive (genflagsrr_exhaustive.c, 2026-05-22) verifies the
 * bridge across TRIADS:
 *     T0: ADD t0 = a0 + b0    ; sets t0's TMP-CF
 *     T1: GFL_RR(t0, t0)      ; arch CF = t0's TMP-CF
 *     T2: ADC t1 = a1 + b1 + arch CF
 * What hasn't been tested is whether the bridge survives intra-triad packing:
 *     T0: { ADD t0=a0+b0, GFL_RR(t0,t0), ADC t1=a1+b1+CF }
 * If slot 2's ADC reads the arch CF set by slot 1's GFL_RR (per the confirmed
 * sequential semantics in test_raw_war_waw.c), then we can pack chains at
 * ~1.5 ops/triad instead of 1, shrinking long chains by ~33%.
 *
 * Probe design: compute   t1 = a1 + b1 + carry_from(a0 + b0)   in a single
 * triad, then read t1's value out. With a0+b0 chosen to overflow, the correct
 * answer requires the bridged CF to carry into the ADC. We verify against the
 * known good behavior of the cross-triad version.
 *
 * Inputs: a0 = 0xFFFFFFFFFFFFFFFF, b0 = 1   (sum overflows, CF=1)
 *         a1 = 0,                  b1 = 0   (so t1 = arch CF = 1 if bridged)
 *
 * Expected:
 *   T0 packed pattern:      t1 == 1 iff intra-triad bridge works
 *   T0/T1/T2 cross-triad:   t1 == 1 (control, known good)
 *
 * Build: make PROG=test_adc_gfl_adc_intratriad   (from simple/)
 * Run:   sudo taskset -c 0 ./test_adc_gfl_adc_intratriad_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* Stage inputs in R8 (a0), R9 (b0), R10 (a1), R11 (b1).
 * Read output t1 from R12 (writeback target).
 * Use TMP0 for t0, TMP1 for t1.
 */
static void install_packed(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        /* Single triad: ADD ; GFL_RR(TMP0,TMP0) ; ADC */
        { ADD_DSZ64_DRR(TMP0, R8, R9),
          GENARITHFLAGS_RR(TMP0, TMP0),
          ADC_DSZ64_DRR(TMP1, R10, R11), NOP_SEQWORD },
        /* Writeback */
        { ZEROEXT_DSZ64_DR(R12, TMP1), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_cross_triad(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        /* Control: cross-triad, known good */
        { ADD_DSZ64_DRR(TMP0, R8, R9), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(TMP0, TMP0), NOP, NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP1, R10, R11), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(R12, TMP1), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static uint64_t fire(uint64_t a0, uint64_t b0, uint64_t a1, uint64_t b1) {
    uint64_t out;
    register uint64_t _a0 asm("r8")  = a0;
    register uint64_t _b0 asm("r9")  = b0;
    register uint64_t _a1 asm("r10") = a1;
    register uint64_t _b1 asm("r11") = b1;
    register uint64_t _o  asm("r12");
    asm volatile(
        "mov rax, 1\n\t"
        "mov rbx, 1\n\t"
        "vmwrite rax, rbx\n\t"
        : "=r"(_o)
        : "r"(_a0), "r"(_b0), "r"(_a1), "r"(_b1)
        : "rax", "rbx", "memory", "cc"
    );
    out = _o;
    return out;
}

int main(void) {
    assign_to_core(0);
    printf("=== intra-triad ADC+GFL+ADC bridge probe ===\n\n");

    /* Test 1: cross-triad control (should be 1) */
    install_cross_triad();
    uint64_t r_ctrl = fire(0xFFFFFFFFFFFFFFFFULL, 1, 0, 0);
    printf("Cross-triad control: t1 = %" PRIu64 " (expect 1)\n", r_ctrl);

    /* Test 2: intra-triad packed (the actual probe) */
    install_packed();
    uint64_t r_pack = fire(0xFFFFFFFFFFFFFFFFULL, 1, 0, 0);
    printf("Intra-triad packed: t1 = %" PRIu64 " (expect 1 if bridge works intra-triad)\n", r_pack);

    /* Counter-tests: no carry (sums don't overflow) */
    install_cross_triad();
    uint64_t r_ctrl_noc = fire(1, 2, 0, 0);
    printf("Cross-triad no-carry: t1 = %" PRIu64 " (expect 0)\n", r_ctrl_noc);

    install_packed();
    uint64_t r_pack_noc = fire(1, 2, 0, 0);
    printf("Intra-triad no-carry: t1 = %" PRIu64 " (expect 0)\n", r_pack_noc);

    printf("\n");
    if (r_ctrl == 1 && r_pack == 1 && r_ctrl_noc == 0 && r_pack_noc == 0) {
        printf("★ Intra-triad ADC+GFL+ADC packing WORKS.\n");
    } else if (r_ctrl == 1 && r_pack != 1) {
        printf("Intra-triad packing FAILS — bridge requires separate triads.\n");
    } else {
        printf("Unexpected results, investigate further.\n");
    }

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
