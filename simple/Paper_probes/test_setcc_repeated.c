/*
 * test_setcc_repeated.c — Does SETCC_CONDB work across repeated vmwrite?
 *
 * ═══════════════════════════════════════════════════════════════════
 *  All single-invocation SETCC tests pass. But the MAC hook fails
 *  when called 3x per asm block (as in the curve25519 square).
 *
 *  Hypothesis: END_SEQWORD restores pre-hook EFLAGS. On the 2nd/3rd
 *  vmwrite, SETCC reads stale CF=0 instead of the ADD's carry.
 *
 *  This test:
 *  - Installs the 4-triad MAC hook (ADD+SETCC, confirmed working alone)
 *  - Calls vmwrite 1x, 2x, 3x with accumulating values
 *  - Checks if R8 (acc_hi) collects all carries
 *
 *  Build:  make PROG=test_setcc_repeated
 *  Run:    sudo taskset -c 0 ./test_setcc_repeated_static
 * ═══════════════════════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"


/* 4-triad MAC: ADD+SETCC carry capture (works alone, fails in sequence?) */
static void install_mac_setcc(void) {
        ucode_t patch[] = {
                /* T0: save acc_lo, multiply */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1: accumulate lo + capture carry */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        SETCC_CONDB_DR(TMP5, RAX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T2: accumulate hi */
                {
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T3: fold carry */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 4);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* 6-triad MAC: bit-manipulation carry (confirmed working) */
static void install_mac_bitmanip(void) {
        ucode_t patch[] = {
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, 6);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  TEST: Call vmwrite 1x, 2x, 3x and check accumulator
 *
 *  We pick values where:
 *  - MAC 1: 0xFFFFFFFFFFFFFF × 1 = product_lo=0xFFFFFFFFFFFFFF, hi=0
 *           acc = 0 + 0xFFFFFFFFFFFFFF = 0xFFFFFFFFFFFFFF (no carry)
 *  - MAC 2: 0xFFFFFFFFFFFFFF × 1 = same product
 *           acc = 0xFFFFFFFFFFFFFF + 0xFFFFFFFFFFFFFF → OVERFLOWS!
 *           CF=1, carry must go to R8
 *  - MAC 3: 1 × 1 = 1
 *           acc = (wrapped value) + 1, unlikely to overflow
 *
 *  Expected after 2 MACs:
 *    acc_lo = (0xFFFFFFFFFFFFFF + 0xFFFFFFFFFFFFFF) & 0xFFFFFFFFFFFFFFFF
 *           = 0x1FFFFFFFFFFFFFE
 *    Wait, 0xFFFFFFFFFFFFFF is 56 bits, so adding two of them:
 *    0xFFFFFFFFFFFFFF + 0xFFFFFFFFFFFFFF = 0x1FFFFFFFFFFFFFE (57 bits)
 *    No 64-bit overflow. Need bigger values.
 *
 *  Let's use values that definitely overflow 64 bits:
 *  MAC 1: 0xFFFFFFFF × 0xFFFFFFFF = product_lo = 0xFFFFFFFE00000001
 *           acc = 0 + 0xFFFFFFFE00000001 (no carry, but acc is large)
 *  MAC 2: 0xFFFFFFFF × 0xFFFFFFFF = same product
 *           acc = 0xFFFFFFFE00000001 + 0xFFFFFFFE00000001 → OVERFLOW!
 *           = 0x1FFFFFFFC00000002, CF=1
 *
 *  Actually simpler: use big direct values.
 * ══════════════════════════════════════════════════════════════════ */

/* Single vmwrite — just check basic accumulation */
static void test_single(const char *label) {
        uint64_t lo, hi;

        /* 0xFFFFFFFF * 0xFFFFFFFF = hi:lo = 0xFFFFFFFE:00000001 */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[a]\n\t"
                "mov rdx, %[b]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(lo), [hi] "=r"(hi)
                : [a] "r"((uint64_t)0xFFFFFFFFULL), [b] "r"((uint64_t)0xFFFFFFFFULL)
                : "rax", "rcx", "rdx", "r8"
        );
        printf("  %-20s  1 vmwrite:  lo=0x%016" PRIx64 "  hi=0x%016" PRIx64 "\n",
               label, lo, hi);
        /* Expected: lo=0xFFFFFFFE00000001, hi=0 */
}

/* Two vmwrites — second should cause 64-bit overflow in acc_lo */
static void test_double(const char *label) {
        uint64_t lo, hi;

        /* MAC1: 0xFFFFFFFF * 0xFFFFFFFF → acc = 0:0xFFFFFFFE00000001
         * MAC2: 0xFFFFFFFF * 0xFFFFFFFF → acc += 0:0xFFFFFFFE00000001
         *       acc_lo = 0xFFFFFFFE00000001 + 0xFFFFFFFE00000001
         *              = 0xFFFFFFFC00000002 + carry (1) to acc_hi
         *       Actually: 0xFFFFFFFE00000001 * 2 = 0x1FFFFFFFC00000002
         *       lo = 0xFFFFFFFC00000002, hi should be 0+0+1 = 1
         */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[a]\n\t"
                "mov rdx, %[b]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov rcx, %[a]\n\t"
                "mov rdx, %[b]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(lo), [hi] "=r"(hi)
                : [a] "r"((uint64_t)0xFFFFFFFFULL), [b] "r"((uint64_t)0xFFFFFFFFULL)
                : "rax", "rcx", "rdx", "r8"
        );
        printf("  %-20s  2 vmwrites: lo=0x%016" PRIx64 "  hi=0x%016" PRIx64 "\n",
               label, lo, hi);

        /* Expected: lo=0xFFFFFFFC00000002, hi=1 (if carry works) */
        /* If broken: lo=0xFFFFFFFC00000002, hi=0 (carry lost) */
        uint64_t expect_hi = 1;  /* one carry from the 64-bit overflow */
        if (hi == expect_hi)
                printf("  %22s  -> hi=%lu CORRECT (carry propagated)\n", "", hi);
        else
                printf("  %22s  -> hi=%lu WRONG (expected %lu, carry LOST)\n",
                       "", hi, expect_hi);
}

/* Three vmwrites — stress test */
static void test_triple(const char *label) {
        uint64_t lo, hi;

        /* 3 × (0xFFFFFFFF * 0xFFFFFFFF) = 3 × 0xFFFFFFFE00000001
         * = 0x2FFFFFFFA00000003
         * lo = 0xFFFFFFFA00000003, hi = 2
         */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[a]\n\t" "mov rdx, %[b]\n\t" "vmwrite rcx, rdx\n\t"
                "mov rcx, %[a]\n\t" "mov rdx, %[b]\n\t" "vmwrite rcx, rdx\n\t"
                "mov rcx, %[a]\n\t" "mov rdx, %[b]\n\t" "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(lo), [hi] "=r"(hi)
                : [a] "r"((uint64_t)0xFFFFFFFFULL), [b] "r"((uint64_t)0xFFFFFFFFULL)
                : "rax", "rcx", "rdx", "r8"
        );
        printf("  %-20s  3 vmwrites: lo=0x%016" PRIx64 "  hi=0x%016" PRIx64 "\n",
               label, lo, hi);

        uint64_t expect_hi = 2;
        if (hi == expect_hi)
                printf("  %22s  -> hi=%lu CORRECT\n", "", hi);
        else
                printf("  %22s  -> hi=%lu WRONG (expected %lu)\n",
                       "", hi, expect_hi);
}

/* Targeted: first MAC doesn't overflow, second does */
static void test_targeted(const char *label) {
        uint64_t lo, hi;

        /* MAC1: 1 * 1 = 1, acc = 0+1 = 1 (no overflow)
         * MAC2: 0xFFFFFFFFFFFFFFFF * 1 = 0xFFFFFFFFFFFFFFFF
         *       acc = 1 + 0xFFFFFFFFFFFFFFFF = 0 with CF=1
         *       hi should get 0 (product_hi) + 1 (carry) = 1
         */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                /* MAC 1: 1 * 1 */
                "mov rcx, %[one]\n\t"
                "mov rdx, %[one]\n\t"
                "vmwrite rcx, rdx\n\t"
                /* MAC 2: 0xFFFFFFFFFFFFFFFF * 1 */
                "mov rcx, %[big]\n\t"
                "mov rdx, %[one]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(lo), [hi] "=r"(hi)
                : [one] "r"((uint64_t)1), [big] "r"((uint64_t)0xFFFFFFFFFFFFFFFFULL)
                : "rax", "rcx", "rdx", "r8"
        );
        printf("  %-20s  targeted:   lo=0x%016" PRIx64 "  hi=0x%016" PRIx64 "\n",
               label, lo, hi);

        /* Expected: lo=0, hi=1 */
        /* 1 + 0xFFFFFFFFFFFFFFFF = 0x10000000000000000, lo=0, carry=1 */
        /* product_hi of (0xFFFFFFFFFFFFFFFF * 1) = 0, so hi = 0 + 1 = 1 */
        if (hi == 1 && lo == 0)
                printf("  %22s  -> CORRECT (carry from 2nd vmwrite propagated)\n", "");
        else if (hi == 0 && lo == 0)
                printf("  %22s  -> CARRY LOST on 2nd vmwrite!\n", "");
        else
                printf("  %22s  -> unexpected result\n", "");
}


/* ══════════════════════════════════════════════════════════════════ */
int main(void) {
        printf("==========================================================\n");
        printf("  SETCC Carry Across Repeated vmwrite Invocations\n");
        printf("==========================================================\n\n");

        /* ── 4-triad MAC (SETCC-based) ──────────────────────────── */
        printf("Installing 4-triad MAC (ADD + SETCC_CONDB carry)\n\n");
        install_mac_setcc();

        test_single("SETCC 4-triad");
        test_double("SETCC 4-triad");
        test_triple("SETCC 4-triad");
        test_targeted("SETCC 4-triad");
        printf("\n");

        /* ── 6-triad MAC (bit-manip, known good) for comparison ── */
        printf("Installing 6-triad MAC (bit-manipulation carry)\n\n");
        install_mac_bitmanip();

        test_single("BitManip 6-triad");
        test_double("BitManip 6-triad");
        test_triple("BitManip 6-triad");
        test_targeted("BitManip 6-triad");
        printf("\n");

        printf("==========================================================\n");
        printf("  If SETCC double/triple show hi=0 but BitManip shows hi=1:\n");
        printf("  -> END_SEQWORD restores pre-hook EFLAGS, killing carries\n");
        printf("     on 2nd+ invocation in the same asm block.\n");
        printf("  Fix: use GENARITHFLAGS before SETCC in the MAC hook.\n");
        printf("==========================================================\n\n");

        return 0;
}
