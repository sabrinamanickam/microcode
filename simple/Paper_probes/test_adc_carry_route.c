/*
 * test_adc_carry_route.c — Can we route ADD's CF into ADC?
 *
 * ═══════════════════════════════════════════════════════════════════
 *  Prior finding: ADC_DSZ64 reads arch RFLAGS.CF (domain #2), frozen
 *  inside a patch. ADD→TMP writes the internal ALU domain (#1) that
 *  SETCC reads. We want a 128-bit add inside one patch — so we need
 *  to either feed domain-#1 CF into domain #2 where ADC sees it, or
 *  find a slot layout that makes ADC read domain #1 directly.
 *
 *  Five strategies, tested side by side. Each is run under arch CF=0
 *  forced at hook entry, so any observed +1 on the hi half must come
 *  from ADD's own carry (propagating through some mechanism), not
 *  from stale arch CF.
 *
 *  Two probe cases per strategy:
 *    N (no-overflow): lo=1+1  → hi must be 0
 *    C (overflow):    lo=FF..F+1 → hi must be 1 for strategy to work
 *
 *  Each strategy passes iff both probes give the correct hi.
 *
 *  Strategies:
 *    A. same triad, all arch-reg sources           (baseline, expect fail)
 *    B. same triad, ADC reads from TMP sources     (test source-domain)
 *    C. cross-triad ADD then ADC                   (test triad boundary)
 *    D. same triad, ADD + GENARITHFLAGS_R(TMP0)    (internal→arch bridge)
 *    E. same triad, ADD + GENARITHFLAGS_RR(TMP0,RAX) (bridge with op hint)
 *
 *  Build: make PROG=test_adc_carry_route
 *  Run:   sudo taskset -c 0 ./test_adc_carry_route_static
 * ═══════════════════════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"


/* ─── strategy installers ─────────────────────────────────────────── */

/* A: baseline — same triad, both arch sources */
static void install_A(void) {
        ucode_t p[] = {
                { ADD_DSZ64_DRR(TMP0, RAX, RDX),
                  ADC_DSZ64_DRR(TMP1, RCX, RBX),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP0),
                  ZEROEXT_DSZ64_DR(RCX, TMP1),
                  NOP, END_SEQWORD },
        };
        patch_ucode(0x7c00, p, sizeof(p)/sizeof(p[0]));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* B: same triad, but ADC has TMP sources (preload a_hi, b_hi to TMPs) */
static void install_B(void) {
        ucode_t p[] = {
                { MOVE_DSZ64_DR(TMP2, RCX),
                  MOVE_DSZ64_DR(TMP3, RBX),
                  NOP, NOP_SEQWORD },
                { ADD_DSZ64_DRR(TMP0, RAX, RDX),
                  ADC_DSZ64_DRR(TMP1, TMP2, TMP3),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP0),
                  ZEROEXT_DSZ64_DR(RCX, TMP1),
                  NOP, END_SEQWORD },
        };
        patch_ucode(0x7c00, p, sizeof(p)/sizeof(p[0]));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* C: cross-triad — ADD in T0, ADC in T1 */
static void install_C(void) {
        ucode_t p[] = {
                { ADD_DSZ64_DRR(TMP0, RAX, RDX),
                  NOP, NOP, NOP_SEQWORD },
                { ADC_DSZ64_DRR(TMP1, RCX, RBX),
                  NOP, NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP0),
                  ZEROEXT_DSZ64_DR(RCX, TMP1),
                  NOP, END_SEQWORD },
        };
        patch_ucode(0x7c00, p, sizeof(p)/sizeof(p[0]));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* D: same triad, ADD + GENARITHFLAGS_R(TMP0) before ADC (next triad) */
static void install_D(void) {
        ucode_t p[] = {
                { ADD_DSZ64_DRR(TMP0, RAX, RDX),
                  GENARITHFLAGS_R(TMP0),
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

/* E: same as D but with GENARITHFLAGS_RR(TMP0, RAX) — operand-hinted */
static void install_E(void) {
        ucode_t p[] = {
                { ADD_DSZ64_DRR(TMP0, RAX, RDX),
                  GENARITHFLAGS_RR(TMP0, RAX),
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


/* ─── harness ─────────────────────────────────────────────────────── */

static void invoke(uint64_t a_lo, uint64_t a_hi,
                   uint64_t b_lo, uint64_t b_hi,
                   int entry_cf,
                   uint64_t *out_lo, uint64_t *out_hi) {
        uint64_t r_lo, r_hi;
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


typedef void (*install_fn)(void);

typedef struct {
        const char *name;
        const char *desc;
        install_fn install;
} strategy_t;

static strategy_t strategies[] = {
        { "A", "baseline: ADD+ADC same triad, arch sources",         install_A },
        { "B", "ADC with TMP sources (preloaded)",                   install_B },
        { "C", "cross-triad: ADD in T0, ADC in T1",                  install_C },
        { "D", "bridge: ADD + GENARITHFLAGS_R(TMP0)",                install_D },
        { "E", "bridge: ADD + GENARITHFLAGS_RR(TMP0,RAX)",           install_E },
};
static const int nstrat = sizeof(strategies)/sizeof(strategies[0]);


typedef struct {
        uint64_t a_lo, a_hi, b_lo, b_hi;
        uint64_t expect_lo, expect_hi;
        const char *label;
} probe_t;

/* Under arch CF=0, the correct 128-bit sum is what we expect. Any
 * strategy that routes ADD's CF into ADC must produce these values. */
static probe_t probes[] = {
        { 1, 0,
          1, 0,
          2, 0,
          "N (no-overflow)" },
        { 0xFFFFFFFFFFFFFFFFULL, 0,
          1,                     0,
          0, 1,
          "C (lo-overflow — CF must propagate)" },
};
static const int nprobes = sizeof(probes)/sizeof(probes[0]);


int main(void) {
        printf("============================================================\n");
        printf("  ADC carry-routing survey (arch CF forced to 0 at entry)\n");
        printf("============================================================\n\n");

        assign_to_core(0);
        do_fix_IN_patch();

        for (int s = 0; s < nstrat; s++) {
                printf("Strategy %s: %s\n", strategies[s].name, strategies[s].desc);
                strategies[s].install();

                int pass = 0;
                for (int p = 0; p < nprobes; p++) {
                        uint64_t got_lo = 0, got_hi = 0;
                        invoke(probes[p].a_lo, probes[p].a_hi,
                               probes[p].b_lo, probes[p].b_hi,
                               0, &got_lo, &got_hi);
                        int ok = (got_lo == probes[p].expect_lo) &&
                                 (got_hi == probes[p].expect_hi);
                        pass += ok;
                        printf("  %-40s  expect hi=%" PRIu64 "  got hi=%" PRIu64 "  %s\n",
                               probes[p].label,
                               probes[p].expect_hi, got_hi,
                               ok ? "PASS" : "FAIL");
                }
                printf("  → %s\n\n",
                       pass == nprobes ? "STRATEGY WORKS (CF routed into ADC)"
                                       : "strategy does not route CF");
        }

        printf("============================================================\n");
        printf("  If any row shows 'STRATEGY WORKS', that's the carry route.\n");
        printf("  If all fail, ADC cannot be driven by ADD's CF via these\n");
        printf("  paths — fallback is SETCC_CONDB + explicit add.\n");
        printf("============================================================\n");

        return 0;
}
