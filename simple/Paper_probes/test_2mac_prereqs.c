/*
 * test_2mac_prereqs.c — Prerequisites for 2-MAC-per-vmwrite hook
 *
 * ═══════════════════════════════════════════════════════════════════
 *  Goal: reduce vmwrite calls from 15 → 10 (2+1 per limb) or 8.
 *
 *  2-MAC hook needs:
 *    RCX/RDX = pair1 (standard vmwrite operands)
 *    RBX     = pair2_a (confirmed readable)
 *    ???     = pair2_b (need a register!)
 *    R8      = acc_hi (must be preserved for chaining)
 *    RAX     = acc_lo
 *
 *  R8 can't serve double duty (acc_hi + pair2_b). We need another
 *  register for pair2_b. R9 crashed as a MUL SOURCE, but might work
 *  as a plain data register read by ZEROEXT/MOVE.
 *
 *  Tests:
 *  1) Can we read R9 in microcode? (ZEROEXT TMP0, R9 → return in RAX)
 *  2) Can we read R10? R11?
 *  3) Can we write to RCX/RDX and have MUL R64SRC×R64DST pick up the
 *     new values? (needed for the second multiply)
 *  4) Full 2-MAC hook test with known values
 *
 *  Build:  make PROG=test_2mac_prereqs
 *  Run:    sudo taskset -c 0 ./test_2mac_prereqs_static
 * ═══════════════════════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* Register IDs from opcode.h */
#ifndef R9
#define R9 0x29UL
#endif
#ifndef R10
#define R10 0x2aUL
#endif
#ifndef R11
#define R11 0x2bUL
#endif


/* ══════════════════════════════════════════════════════════════════
 *  Test 1: Can we read R9 in microcode?
 *
 *  Hook: ZEROEXT TMP0, R9 | ZEROEXT RAX, TMP0 | END
 *  x86:  set R9 = known value, vmwrite, check RAX
 * ══════════════════════════════════════════════════════════════════ */
static void install_read_r9(void) {
        ucode_t patch[] = {
                {
                        ZEROEXT_DSZ64_DR(TMP0, R9),
                        ZEROEXT_DSZ64_DR(RAX, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        NOP,
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 2);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_read_r10(void) {
        ucode_t patch[] = {
                {
                        ZEROEXT_DSZ64_DR(TMP0, R10),
                        ZEROEXT_DSZ64_DR(RAX, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        NOP,
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 2);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_read_r11(void) {
        ucode_t patch[] = {
                {
                        ZEROEXT_DSZ64_DR(TMP0, R11),
                        ZEROEXT_DSZ64_DR(RAX, TMP0),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        NOP,
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 2);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  Test 2: Can we write to RCX/RDX and have MUL see the new values?
 *
 *  Hook: 
 *    T0: ZEROEXT RCX, RBX           ; overwrite RCX with RBX value
 *        ZEROEXT RDX, R8            ; overwrite RDX with R8 value
 *        NOP
 *    T1: NOP
 *        MUL R64SRC, R64SRC, R64DST ; does this see new RCX/RDX?
 *        NOP
 *    T2: ZEROEXT RAX, RCX           ; return product_lo in RAX
 *        NOP | END
 *
 *  x86: RCX=garbage, RDX=garbage, RBX=3, R8=7
 *  Expected: RAX = lo(3 × 7) = 21
 * ══════════════════════════════════════════════════════════════════ */
static void install_test_rcx_rewrite(void) {
        ucode_t patch[] = {
                /* T0: overwrite RCX/RDX with RBX/R8 */
                {
                        ZEROEXT_DSZ64_DR(RCX, RBX),
                        ZEROEXT_DSZ64_DR(RDX, R8),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1: multiply — does R64SRC/R64DST see new values? */
                {
                        NOP,
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T2: return product_lo */
                {
                        ZEROEXT_DSZ64_DR(RAX, RCX),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 3);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  Test 3: Full 2-MAC hook (if R9 + RCX rewrite both work)
 *
 *  Convention:
 *    RAX = acc_lo      (accumulator, preserved across calls)
 *    R8  = acc_hi      (accumulator, preserved across calls)
 *    RCX = pair1_a     (vmwrite operand)
 *    RDX = pair1_b     (vmwrite operand)
 *    RBX = pair2_a     (extra operand)
 *    R9  = pair2_b     (extra operand, if readable)
 *
 *  Hook body (11 triads with merged SHR+ADD):
 *
 *  ;; ═══ MAC1: RCX × RDX ═══
 *  T0:  ZEROEXT TMP3, RAX            ; save acc_lo
 *       MUL R64SRC, R64SRC, R64DST   ; product → RCX:RDX
 *       NOP
 *
 *  T1:  ADD RAX, TMP3, RCX           ; acc_lo += product1_lo
 *       AND TMP1, TMP3, RCX
 *       OR  TMP2, TMP3, RCX
 *
 *  T2:  NOTAND TMP4, RAX, TMP2
 *       ADD R8, R8, RDX              ; acc_hi += product1_hi
 *       NOP
 *
 *  T3:  OR TMP5, TMP1, TMP4
 *       NOP
 *       NOP
 *
 *  T4:  SHR TMP5, TMP5, 63 | ADD R8, R8, TMP5 | NOP    ; fold carry
 *
 *  ;; ═══ Setup MAC2: RBX × R9 → RCX × RDX ═══
 *  T5:  ZEROEXT TMP3, RAX            ; save acc_lo
 *       ZEROEXT RCX, RBX             ; pair2_a → RCX
 *       ZEROEXT RDX, R9              ; pair2_b → RDX
 *
 *  T6:  NOP
 *       MUL R64SRC, R64SRC, R64DST   ; product → RCX:RDX
 *       NOP
 *
 *  ;; ═══ MAC2 carry chain ═══
 *  T7:  ADD RAX, TMP3, RCX
 *       AND TMP1, TMP3, RCX
 *       OR  TMP2, TMP3, RCX
 *
 *  T8:  NOTAND TMP4, RAX, TMP2
 *       ADD R8, R8, RDX
 *       NOP
 *
 *  T9:  OR TMP5, TMP1, TMP4
 *       NOP
 *       NOP
 *
 *  T10: SHR TMP5, TMP5, 63 | ADD R8, R8, TMP5 | END
 * ══════════════════════════════════════════════════════════════════ */
static void install_2mac(void) {
        ucode_t patch[] = {
                /* T0 */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1 */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T2 */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T3 */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T4: merged SHR+ADD */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP_SEQWORD
                },
                /* T5: setup MAC2 */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        ZEROEXT_DSZ64_DR(RCX, RBX),
                        ZEROEXT_DSZ64_DR(RDX, R9),
                        NOP_SEQWORD
                },
                /* T6: MAC2 multiply */
                {
                        NOP,
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T7 */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T8 */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T9 */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T10: merged SHR+ADD, END */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 11);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  HARNESS
 * ══════════════════════════════════════════════════════════════════ */

/* Test register readability: set reg, vmwrite, check if RAX got the value */
static int test_reg_read(const char *name,
                         void (*installer)(void),
                         uint64_t test_val,
                         int reg_num) {
        installer();

        uint64_t result;

        /* Ugly but necessary: need to set the specific register before vmwrite */
        if (reg_num == 9) {
                asm volatile(
                        "mov r9, %[val]\n\t"
                        "xor rcx, rcx\n\t"
                        "xor rdx, rdx\n\t"
                        "vmwrite rcx, rdx\n\t"
                        "mov %[out], rax\n\t"
                        : [out] "=r"(result)
                        : [val] "r"(test_val)
                        : "rax", "rcx", "rdx", "r8", "r9"
                );
        } else if (reg_num == 10) {
                asm volatile(
                        "mov r10, %[val]\n\t"
                        "xor rcx, rcx\n\t"
                        "xor rdx, rdx\n\t"
                        "vmwrite rcx, rdx\n\t"
                        "mov %[out], rax\n\t"
                        : [out] "=r"(result)
                        : [val] "r"(test_val)
                        : "rax", "rcx", "rdx", "r8", "r10"
                );
        } else if (reg_num == 11) {
                asm volatile(
                        "mov r11, %[val]\n\t"
                        "xor rcx, rcx\n\t"
                        "xor rdx, rdx\n\t"
                        "vmwrite rcx, rdx\n\t"
                        "mov %[out], rax\n\t"
                        : [out] "=r"(result)
                        : [val] "r"(test_val)
                        : "rax", "rcx", "rdx", "r8", "r11"
                );
        }

        int ok = (result == test_val);
        printf("  %-6s  sent: 0x%016" PRIx64 "  got: 0x%016" PRIx64 "  %s\n",
               name, test_val, result, ok ? "PASS" : "** FAIL **");
        return ok;
}

/* Test RCX/RDX rewrite for second MUL */
static int test_rcx_rewrite(void) {
        install_test_rcx_rewrite();

        uint64_t result;
        uint64_t rbx_val = 7, r8_val = 3;

        asm volatile(
                "mov rbx, %[bx]\n\t"
                "mov r8, %[r8v]\n\t"
                "mov rcx, 0xDEAD\n\t"   /* garbage — should be overwritten */
                "mov rdx, 0xBEEF\n\t"   /* garbage */
                "vmwrite rcx, rdx\n\t"
                "mov %[out], rax\n\t"
                : [out] "=r"(result)
                : [bx] "r"(rbx_val), [r8v] "r"(r8_val)
                : "rax", "rbx", "rcx", "rdx", "r8"
        );

        uint64_t expected = rbx_val * r8_val;  /* 7 * 3 = 21 */
        int ok = (result == expected);
        printf("  RCX/RDX rewrite for MUL: RBX=%lu × R8=%lu = %lu  "
               "got: %lu  %s\n",
               rbx_val, r8_val, expected, result,
               ok ? "PASS" : "** FAIL **");
        return ok;
}

/* Test full 2-MAC hook with known values */
static int test_2mac_known(void) {
        install_2mac();

        uint64_t lo, hi;

        /* MAC1: 3 × 5 = 15
         * MAC2: 7 × 11 = 77
         * Total: 15 + 77 = 92
         * Expected: lo=92, hi=0
         */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[p1a]\n\t"
                "mov rdx, %[p1b]\n\t"
                "mov rbx, %[p2a]\n\t"
                "mov r9, %[p2b]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(lo), [hi] "=r"(hi)
                : [p1a] "r"((uint64_t)3), [p1b] "r"((uint64_t)5),
                  [p2a] "r"((uint64_t)7), [p2b] "r"((uint64_t)11)
                : "rax", "rbx", "rcx", "rdx", "r8", "r9"
        );

        int ok = (lo == 92 && hi == 0);
        printf("  2-MAC small: 3*5 + 7*11 = 92  got lo=%lu hi=%lu  %s\n",
               lo, hi, ok ? "PASS" : "** FAIL **");
        if (!ok) return 0;

        /* Test with overflow: acc starts at 0
         * MAC1: 0xFFFFFFFF × 0xFFFFFFFF = 0xFFFFFFFE00000001
         * MAC2: 0xFFFFFFFF × 0xFFFFFFFF = 0xFFFFFFFE00000001
         * Total: 0x1FFFFFFFC00000002
         * Expected: lo=0xFFFFFFFC00000002, hi=1
         */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[v]\n\t"
                "mov rdx, %[v]\n\t"
                "mov rbx, %[v]\n\t"
                "mov r9, %[v]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(lo), [hi] "=r"(hi)
                : [v] "r"((uint64_t)0xFFFFFFFFULL)
                : "rax", "rbx", "rcx", "rdx", "r8", "r9"
        );

        int ok2 = (lo == 0xFFFFFFFC00000002ULL && hi == 1);
        printf("  2-MAC overflow: 2×(0xFFFFFFFF²)  lo=0x%016" PRIx64
               " hi=%lu  %s\n",
               lo, hi, ok2 ? "PASS" : "** FAIL **");
        if (!ok2) return 0;

        /* Test chaining: two vmwrite calls, acc_hi should accumulate */
        /* Call 1: 0xFFFFFFFF × 0xFFFFFFFF + 0xFFFFFFFF × 0xFFFFFFFF = lo1, hi1
         * Call 2: 0xFFFFFFFF × 0xFFFFFFFF + 0xFFFFFFFF × 0xFFFFFFFF = adds to acc
         * Total: 4 × 0xFFFFFFFE00000001 = 0x3FFFFFFF800000004
         * lo = 0xFFFFFFF800000004, hi = 3
         */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[v]\n\t" "mov rdx, %[v]\n\t"
                "mov rbx, %[v]\n\t" "mov r9, %[v]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov rcx, %[v]\n\t" "mov rdx, %[v]\n\t"
                "mov rbx, %[v]\n\t" "mov r9, %[v]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(lo), [hi] "=r"(hi)
                : [v] "r"((uint64_t)0xFFFFFFFFULL)
                : "rax", "rbx", "rcx", "rdx", "r8", "r9"
        );

        int ok3 = (lo == 0xFFFFFFF800000004ULL && hi == 3);
        printf("  2-MAC chained: 4×(0xFFFFFFFF²)  lo=0x%016" PRIx64
               " hi=%lu  %s\n",
               lo, hi, ok3 ? "PASS (acc_hi chains across calls!)" : "** FAIL **");

        return ok && ok2 && ok3;
}


/* ══════════════════════════════════════════════════════════════════ */
int main(void) {
        printf("==========================================================\n");
        printf("  2-MAC Hook Prerequisites\n");
        printf("==========================================================\n\n");

        /* ── Test 1: Register readability ─────────────────────── */
        printf("Test 1: Register readability in microcode\n\n");

        int r9_ok = 1, r10_ok = 1, r11_ok = 1;

        uint64_t vals[] = { 0xDEADBEEFCAFE1234ULL, 42, 0, 0xFFFFFFFFFFFFFFFFULL };

        printf("  R9:\n");
        for (int i = 0; i < 4; i++)
                r9_ok &= test_reg_read("R9", install_read_r9, vals[i], 9);

        printf("\n  R10:\n");
        for (int i = 0; i < 4; i++)
                r10_ok &= test_reg_read("R10", install_read_r10, vals[i], 10);

        printf("\n  R11:\n");
        for (int i = 0; i < 4; i++)
                r11_ok &= test_reg_read("R11", install_read_r11, vals[i], 11);

        printf("\n  Summary: R9=%s  R10=%s  R11=%s\n\n",
               r9_ok ? "OK" : "FAIL",
               r10_ok ? "OK" : "FAIL",
               r11_ok ? "OK" : "FAIL");

        /* ── Test 2: RCX/RDX rewrite for second MUL ──────────── */
        printf("Test 2: RCX/RDX rewrite → MUL R64SRC×R64DST\n\n");
        int rewrite_ok = test_rcx_rewrite();
        printf("\n");

        /* ── Test 3: Full 2-MAC hook ──────────────────────────── */
        if (r9_ok && rewrite_ok) {
                printf("Test 3: Full 2-MAC hook (R9 readable + RCX rewrite works)\n\n");
                int mac2_ok = test_2mac_known();
                printf("\n");

                if (mac2_ok) {
                        printf("==========================================================\n");
                        printf("  ALL PREREQUISITES PASS\n");
                        printf("==========================================================\n");
                        printf("  2-MAC hook is viable!\n");
                        printf("  Convention: RCX×RDX + RBX×R9 → RAX:R8\n");
                        printf("  11 triads, reduces vmwrite calls from 15 to 10\n");
                        printf("  Expected: ~85-90 cycles (from 105)\n\n");
                }
        } else {
                printf("==========================================================\n");
                printf("  Prerequisites FAILED\n");
                printf("==========================================================\n");
                if (!r9_ok)
                        printf("  R9 not readable. Try R10 or R11 if they passed.\n");
                if (!rewrite_ok)
                        printf("  RCX/RDX rewrite doesn't propagate to R64SRC/R64DST.\n"
                               "  2-MAC approach needs a different MUL strategy.\n");
                printf("\n");
        }

        return 0;
}
