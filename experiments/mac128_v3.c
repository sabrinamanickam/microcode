/*
 * mac128_v3.c — MAC128 with CMOVCC carry capture
 *
 * CONFIRMED HARDWARE BEHAVIOR:
 *   MUL_DSZ64_DRR(dst, src0, src1):
 *     dst  = HIGH 64 bits
 *     src1 = LOW  64 bits (overwritten)
 *     src0 = unchanged
 *
 *   SETCC_CONDB_DR: does NOT capture CF from ADD  ← confirmed by test
 *   CMOVCC_CONDB:   trying as alternative
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"


void install_mac128(void) {
        ucode_t mac128_patch[] = {

                /* ── Triad 0: Multiply + pre-zero carry register ──
                 *  TMP0 = HIGH, RDX = LOW
                 *  TMP2 = 0 (ready for conditional set)
                 * ──────────────────────────────────────────────── */
                {
                        MUL_DSZ64_DRR(TMP0, RCX, RDX),
                        MOVE_DSZ64_DI(TMP2, 0),
                        NOP,
                        NOP_SEQWORD
                },

                /* ── Triad 1: Accumulate low half (sets CF) ─────── */
                {
                        ADD_DSZ64_DRR(RAX, RAX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },

                /* ── Triad 2: Capture carry via CMOVCC + high acc ─
                 *  CMOVCC: if CF from triad 1, TMP2 = 1; else 0
                 *  ADD:    acc_hi += prod_hi (independent of CF)
                 * ──────────────────────────────────────────────── */
                {
                        CMOVCC_DSZ64_CONDB_DRI(TMP2, TMP2, 1),
                        ADD_DSZ64_DRR(R8, R8, TMP0),
                        NOP,
                        NOP_SEQWORD
                },

                /* ── Triad 3: Fold carry, terminate ───────────── */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP2),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac128_patch, ARRAY_SZ(mac128_patch));
        hook_match_and_patch(0, 0x0428, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  TEST SUITE
 * ══════════════════════════════════════════════════════════════════ */

typedef struct {
        const char *name;
        uint64_t acc_lo_in, acc_hi_in;
        uint64_t src1, src2;
        uint64_t exp_lo, exp_hi;
} mac_test_t;

static int run_one_test(const mac_test_t *t) {
        uint64_t rax_out, r8_out;

        asm volatile(
                "mov rax, %[alo]\n\t"
                "mov r8,  %[ahi]\n\t"
                "mov rcx, %[s1]\n\t"
                "mov rdx, %[s2]\n\t"
                "rdrand rax\n\t"
                "mov %[rax], rax\n\t"
                "mov %[r8],  r8\n\t"
                : [rax] "=r"(rax_out), [r8] "=r"(r8_out)
                : [alo] "r"(t->acc_lo_in), [ahi] "r"(t->acc_hi_in),
                  [s1]  "r"(t->src1),      [s2]  "r"(t->src2)
                : "rax", "rcx", "rdx", "r8"
        );

        int pass = (rax_out == t->exp_lo && r8_out == t->exp_hi);
        printf("%s:\n", t->name);
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")%s\n",
               rax_out, t->exp_lo, (rax_out == t->exp_lo) ? "" : " ✗");
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")%s\n",
               r8_out, t->exp_hi, (r8_out == t->exp_hi) ? "" : " ✗");
        printf("  %s\n\n", pass ? "✓ PASS" : "✗ FAIL");
        return pass;
}


int main(void) {

        printf("╔════════════════════════════════════════════════╗\n");
        printf("║  MAC128 v3 — CMOVCC carry capture              ║\n");
        printf("╚════════════════════════════════════════════════╝\n\n");

        install_mac128();

        mac_test_t tests[] = {
                {
                        .name = "Test 1: acc=100, acc += 7*13 (=191)",
                        .acc_lo_in = 100, .acc_hi_in = 0,
                        .src1 = 7, .src2 = 13,
                        .exp_lo = 191, .exp_hi = 0
                },
                {
                        .name = "Test 2: acc=0, acc += max*max",
                        .acc_lo_in = 0, .acc_hi_in = 0,
                        .src1 = 0xFFFFFFFFFFFFFFFFULL,
                        .src2 = 0xFFFFFFFFFFFFFFFFULL,
                        .exp_lo = 0x0000000000000001ULL,
                        .exp_hi = 0xFFFFFFFFFFFFFFFEULL
                },
                {
                        .name = "Test 3: acc=MAX128, acc += 1*2 (CARRY TEST)",
                        .acc_lo_in = 0xFFFFFFFFFFFFFFFFULL,
                        .acc_hi_in = 0xFFFFFFFFFFFFFFFFULL,
                        .src1 = 1, .src2 = 2,
                        .exp_lo = 0x0000000000000001ULL,
                        .exp_hi = 0x0000000000000000ULL
                },
                {
                        .name = "Test 4: Curve25519 limb product",
                        .acc_lo_in = 0, .acc_hi_in = 0,
                        .src1 = 0x0006000000000000ULL,
                        .src2 = 0x0004000000000000ULL,
                        .exp_lo = 0x0000000000000000ULL,
                        .exp_hi = 0x0000001800000000ULL
                },
                {
                        .name = "Test 5: acc has existing high, small MAC",
                        .acc_lo_in = 0, .acc_hi_in = 0xFF,
                        .src1 = 1, .src2 = 1,
                        .exp_lo = 1, .exp_hi = 0xFF
                },
                {
                        .name = "Test 6: Low overflow carry to hi (CARRY TEST)",
                        .acc_lo_in = 0xFFFFFFFFFFFFFFFFULL,
                        .acc_hi_in = 5,
                        .src1 = 1, .src2 = 3,
                        .exp_lo = 0x0000000000000002ULL,
                        .exp_hi = 6
                },
                {
                        .name = "Test 7: Sequential MAC (pre-accumulated)",
                        .acc_lo_in = 15, .acc_hi_in = 0,
                        .src1 = 7, .src2 = 11,
                        .exp_lo = 92, .exp_hi = 0
                },
                {
                        .name = "Test 8: Probe test vector (0x8..1 * 3)",
                        .acc_lo_in = 0, .acc_hi_in = 0,
                        .src1 = 0x8000000000000001ULL,
                        .src2 = 0x3ULL,
                        .exp_lo = 0x8000000000000003ULL,
                        .exp_hi = 0x0000000000000001ULL
                },
        };

        int n = sizeof(tests) / sizeof(tests[0]);
        int pass_count = 0;
        for (int i = 0; i < n; i++)
                pass_count += run_one_test(&tests[i]);

        printf("════════════════════════════════════════\n");
        printf("  RESULTS: %d / %d passed\n", pass_count, n);
        printf("════════════════════════════════════════\n\n");

        if (pass_count == n) {
                printf("MAC128 is fully operational!\n\n");
                printf("Documented Goldmont microcode findings:\n");
                printf("  MUL_DSZ64_DRR(dst, src0, src1):\n");
                printf("    dst  = HIGH 64 bits\n");
                printf("    src1 = LOW  64 bits (overwritten)\n");
                printf("    src0 = unchanged\n");
                printf("  SETCC_CONDB: does NOT capture ADD carry\n");
                printf("  CMOVCC_CONDB: DOES capture ADD carry\n\n");
                printf("Ready for curve25519 integration.\n");
        } else {
                printf("CMOVCC carry capture also failed.\n\n");
                printf("Remaining options to try:\n");
                printf("  1. READAFLAGS_DR to read flags register,\n");
                printf("     then AND to extract CF bit\n");
                printf("  2. ADC_DSZ64 if it exists (check inst.h\n");
                printf("     for ADD-with-carry variant)\n");
                printf("  3. Use SUB-based borrow detection:\n");
                printf("     save RAX before ADD, compare after\n");
                printf("  4. Manual carry: SHR the overflow bit\n");
                printf("     (only works for our bounded inputs)\n");
        }

        return 0;
}
