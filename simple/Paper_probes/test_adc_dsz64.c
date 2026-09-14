/*
 * test_adc_dsz64.c — Validate speculative _ADC_DSZ64 = 0x37e opcode
 *
 * ═══════════════════════════════════════════════════════════════════
 *  Hypothesis: Goldmont ALU opcode encoding uses bits [7:6] as a
 *  data-size field (ADD_DSZ32=0x000, ADD_DSZ64=0x040; MUL_DSZ32=0x22c,
 *  MUL_DSZ64=0x26c — delta 0x40). If that extrapolates to ADC, then
 *  _ADC_DSZ64 lives at _ADC (0x33e) + 0x40 = 0x37e.
 *
 *  Method: chain ADD + ADC in the same triad to compute a 128-bit
 *  sum. Known from flags.c Strategy A: when ADD writes to TMP0, its
 *  CF is visible to the next slot in the same triad. So ADC_DSZ64
 *  in slot 1 should consume that CF.
 *
 *  Patch layout (2 triads):
 *    T0: ADD_DSZ64_DRR(TMP0, RAX, RDX)   ; lo = a_lo + b_lo (sets CF)
 *        ADC_DSZ64_DRR(TMP1, RCX, RBX)   ; hi = a_hi + b_hi + CF
 *        NOP
 *    T1: ZEROEXT_DSZ64_DR(RAX, TMP0)     ; return lo in RAX
 *        ZEROEXT_DSZ64_DR(RCX, TMP1)     ; return hi in RCX
 *        NOP | END
 *
 *  Interpretation:
 *    - All cases pass → 0x37e is ADC_DSZ64, hypothesis confirmed.
 *    - Hi is short by 1 whenever lo overflows → opcode executes as
 *      plain 64-bit add with no CF consumption (either 0x37e aliases
 *      ADD_DSZ64 or CF isn't actually visible between those slots).
 *    - Garbage / hang / #UD → 0x37e is something else.
 *
 *  Build: make PROG=test_adc_dsz64
 *  Run:   sudo taskset -c 0 ./test_adc_dsz64_static
 * ═══════════════════════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"


static void install_hook(void) {
        ucode_t patch[] = {
                {
                        ADC_DSZ64_DRR(TMP0, RAX, RDX),
                        ADC_DSZ64_DRR(TMP1, RCX, RBX),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ZEROEXT_DSZ64_DR(RAX, TMP0),
                        ZEROEXT_DSZ64_DR(RCX, TMP1),
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 2);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* force arch CF to a known value, then trigger the hook */
static void invoke_hook_128(uint64_t a_lo, uint64_t a_hi,
                            uint64_t b_lo, uint64_t b_hi,
                            int entry_cf,
                            uint64_t *out_lo, uint64_t *out_hi) {
        uint64_t r_lo, r_hi;
        /* Build an RFLAGS image with CF=entry_cf. Bit 1 must be 1. */
        uint64_t flags_img = entry_cf ? 0x3ULL : 0x2ULL;
        asm volatile(
                "push %[flg]\n\t"
                "popfq\n\t"             /* set arch RFLAGS (CF controlled) */
                "mov rax, %[alo]\n\t"   /* mov does not touch flags */
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


typedef struct {
        uint64_t a_lo, a_hi;
        uint64_t b_lo, b_hi;
        uint64_t expect_lo, expect_hi;
        const char *desc;
} case128_t;


/*  Cases marked * distinguish real ADC from a plain-ADD alias: they
 *  overflow at the low half, so hi differs by 1 between the two. */
static case128_t cases[] = {
        { 0, 0,
          0, 0,
          0, 0,
          "0 + 0                                  (no carry)" },

        { 1, 0,
          1, 0,
          2, 0,
          "1 + 1                                  (no carry)" },

        { 0xFFFFFFFFFFFFFFFFULL, 0,
          1,                      0,
          0, 1,
          "FF..F + 1                              (* carry)" },

        { 0xFFFFFFFFFFFFFFFFULL, 0,
          2,                      0,
          1, 1,
          "FF..F + 2                              (* carry)" },

        { 0xFFFFFFFFFFFFFFFFULL, 0,
          0xFFFFFFFFFFFFFFFFULL, 0,
          0xFFFFFFFFFFFFFFFEULL, 1,
          "FF..F + FF..F                          (* carry)" },

        { 0, 0xFFFFFFFFFFFFFFFFULL,
          0, 1,
          0, 0,
          "hi wrap: 0||FF..F + 1||0               (hi wraps, no carry)" },

        { 0x8000000000000000ULL, 0,
          0x8000000000000000ULL, 0,
          0, 1,
          "80..0 + 80..0                          (* carry)" },

        { 0x1234567890ABCDEFULL, 0xDEADBEEFCAFEBABEULL,
          0xFEDCBA9876543210ULL, 0x0123456789ABCDEFULL,
          0x1111111106FFFFFFULL, 0xDFD1045754AA88AEULL,
          "mixed-high-bits                        (* carry)" },
};
static const int ncases = sizeof(cases) / sizeof(cases[0]);


/* Compute the expected (lo, hi) for the given entry CF, assuming ADC
 * consumes arch CF (not ADD's just-produced CF). */
static void expected_if_arch_cf(const case128_t *c, int entry_cf,
                                uint64_t *elo, uint64_t *ehi) {
        __uint128_t lo128 = (__uint128_t)c->a_lo + (__uint128_t)c->b_lo;
        uint64_t hi = c->a_hi + c->b_hi + (uint64_t)entry_cf;
        *elo = (uint64_t)lo128;
        *ehi = hi;
}


static int run_pass(int entry_cf, const char *label) {
        printf("============================================================\n");
        printf("  PASS: entry CF = %d  (%s)\n", entry_cf, label);
        printf("============================================================\n\n");

        int match_adc_from_add = 0;        /* matches hypothesis: ADC reads ADD's CF   */
        int match_adc_from_arch = 0;       /* matches hypothesis: ADC reads arch CF    */
        int total = 0;

        for (int i = 0; i < ncases; i++) {
                uint64_t got_lo = 0, got_hi = 0;
                invoke_hook_128(cases[i].a_lo, cases[i].a_hi,
                                cases[i].b_lo, cases[i].b_hi,
                                entry_cf, &got_lo, &got_hi);

                /* (1) expected under "ADC reads CF from slot-0 ADD" */
                uint64_t e1_lo = cases[i].expect_lo;
                uint64_t e1_hi = cases[i].expect_hi;

                /* (2) expected under "ADC reads arch CF" */
                uint64_t e2_lo, e2_hi;
                expected_if_arch_cf(&cases[i], entry_cf, &e2_lo, &e2_hi);

                int m1 = (got_lo == e1_lo) && (got_hi == e1_hi);
                int m2 = (got_lo == e2_lo) && (got_hi == e2_hi);
                match_adc_from_add  += m1;
                match_adc_from_arch += m2;
                total++;

                printf("  %s\n", cases[i].desc);
                printf("    got:     %016" PRIx64 " %016" PRIx64 "\n",
                       got_hi, got_lo);
                printf("    ADD-CF:  %016" PRIx64 " %016" PRIx64 "  %s\n",
                       e1_hi, e1_lo, m1 ? "match" : "----");
                printf("    arch-CF: %016" PRIx64 " %016" PRIx64 "  %s\n\n",
                       e2_hi, e2_lo, m2 ? "match" : "----");
        }

        printf("  matches ADD-CF  model: %d / %d\n", match_adc_from_add,  total);
        printf("  matches arch-CF model: %d / %d\n", match_adc_from_arch, total);
        printf("\n");
        return match_adc_from_arch;
}


int main(void) {
        printf("============================================================\n");
        printf("  ADC_DSZ64 opcode validation (speculative _ADC_DSZ64=0x37e)\n");
        printf("  Disambiguating: does ADC read CF from slot-0 ADD or arch?\n");
        printf("============================================================\n\n");

        install_hook();

        int n_arch_cf0 = run_pass(0, "CF cleared before vmwrite");
        int n_arch_cf1 = run_pass(1, "CF set before vmwrite");

        printf("============================================================\n");
        printf("  VERDICT\n");
        printf("============================================================\n");
        if (n_arch_cf0 == ncases && n_arch_cf1 == ncases) {
                printf("  >>> ADC reads ARCH CF (not slot-0 ADD's CF).\n");
                printf("      0x37e is a valid ADC_DSZ64, but the slot-0 ADD\n");
                printf("      in this patch does NOT feed its carry into it.\n");
                printf("      For 128-bit add, we need a different plumbing\n");
                printf("      (e.g. SETCC_CONDB + explicit carry add, or an\n");
                printf("      explicit flag-load uop before the ADC).\n");
        } else {
                printf("  >>> Mixed/unexpected — inspect per-case output.\n");
        }
        printf("\n");
        return 0;
}
