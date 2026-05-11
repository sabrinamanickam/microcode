/*
 * test_mul_slot2.c — does MUL_DSZ64_DRR work in slot 2 of a triad?
 *
 * Open question from microcode_findings.md: production patches place MUL
 * only in slot 0 or slot 1. Slot 2 has never been tested. This file
 * answers the question with three controlled tests:
 *
 *   Test A: MUL in slot 0 (control — known working)
 *   Test B: MUL in slot 1 (control — known working)
 *   Test C: MUL in slot 2 (the unknown)
 *
 * Each test uses a fixed pair of inputs (a, b) and checks that MUL
 * produced the right lo (in srcB after MUL) and hi (in dst).
 *
 * Inputs:    a = 0x100000007 = 2^32 + 7
 *            b = 0x100000003 = 2^32 + 3
 * Expected:  a * b = 2^64 + 10·2^32 + 21
 *            lo = 21·2^0  + 10·2^32 = 0xA00000015
 *            hi = 1
 *
 * Pattern per test:
 *   Pre-vmwrite: RDI=a, R9=b
 *   Patch fires MUL_DSZ64_DRR(RCX, RDI, R9) in the slot under test.
 *   After MUL, R9 holds lo and RCX holds hi (since srcB receives lo).
 *   Patch then ZEROEXTs R9 → RAX (return lo to C) and RCX → RBX (return hi).
 *
 * Diagnostic readings if the test_C MUL fires correctly:
 *     RAX = 0xA00000015
 *     RBX = 1
 * If MUL silently does nothing in slot 2 (acts as NOP):
 *     RAX = 0x100000003   (R9 unchanged = original b)
 *     RBX = whatever junk RCX held at vmwrite entry
 * Any other pattern means slot 2 MUL has nonstandard semantics.
 *
 * Build:  make PROG=test_mul_slot2
 * Run:    sudo taskset -c 0 ./test_mul_slot2_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define IN_A  UINT64_C(0x100000007)   /* 2^32 + 7 */
#define IN_B  UINT64_C(0x100000003)   /* 2^32 + 3 */
#define EXP_LO UINT64_C(0xA00000015)  /* (2^32+7)(2^32+3) mod 2^64 */
#define EXP_HI UINT64_C(0x1)

/* Run the currently-installed patch with R9=IN_B, RDI=IN_A, return RAX (=lo)
 * and RBX (=hi). Caller is responsible for installing one of patch_a/b/c
 * before calling. */
static void run_patch(uint64_t *out_lo, uint64_t *out_hi) {
    uint64_t lo, hi;
    asm volatile(
        "push rbx\n\t"
        "mov rdi, %[a]\n\t"
        "mov r9,  %[b]\n\t"
        "xor eax, eax\n\t"          /* clear RAX so failure is diagnosable */
        "xor ebx, ebx\n\t"          /* clear RBX too */
        "vmwrite rcx, rdx\n\t"
        "mov %[lo], rax\n\t"
        "mov %[hi], rbx\n\t"
        "pop rbx\n\t"
        : [lo] "=&r"(lo), [hi] "=&r"(hi)
        : [a] "r"(IN_A), [b] "r"(IN_B)
        : "rax", "rcx", "rdx", "rdi", "r9", "memory", "cc"
    );
    *out_lo = lo;
    *out_hi = hi;
}

/* Test A: MUL in slot 0
 *   T0: MUL,                                                       NOP, NOP
 *   T1: ZEROEXT(RAX, R9), ZEROEXT(RBX, RCX),                       NOP   <- END
 */
static void install_test_a(void) {
    ucode_t p[] = {
        { MUL_DSZ64_DRR(RCX, RDI, R9), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, R9), ZEROEXT_DSZ64_DR(RBX, RCX),
          NOP, END_SEQWORD }
    };
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Test B: MUL in slot 1
 *   T0: NOP, MUL, NOP
 *   T1: ZEROEXT(RAX, R9), ZEROEXT(RBX, RCX), NOP   <- END
 */
static void install_test_b(void) {
    ucode_t p[] = {
        { NOP, MUL_DSZ64_DRR(RCX, RDI, R9), NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, R9), ZEROEXT_DSZ64_DR(RBX, RCX),
          NOP, END_SEQWORD }
    };
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Test C: MUL in slot 2 — THE UNKNOWN
 *   T0: NOP, NOP, MUL
 *   T1: ZEROEXT(RAX, R9), ZEROEXT(RBX, RCX), NOP   <- END
 */
static void install_test_c(void) {
    ucode_t p[] = {
        { NOP, NOP, MUL_DSZ64_DRR(RCX, RDI, R9), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, R9), ZEROEXT_DSZ64_DR(RBX, RCX),
          NOP, END_SEQWORD }
    };
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void diagnose(const char *label, uint64_t lo, uint64_t hi) {
    int lo_ok = (lo == EXP_LO);
    int hi_ok = (hi == EXP_HI);
    int unchanged = (lo == IN_B);

    printf("  %s: RAX=0x%016" PRIx64 "  RBX=0x%016" PRIx64 "  ",
           label, lo, hi);
    if (lo_ok && hi_ok) {
        printf("PASS — MUL fired correctly\n");
    } else if (unchanged) {
        printf("FAIL — MUL did not fire (R9 unchanged = original b)\n");
    } else {
        printf("FAIL — unexpected (expected lo=0x%" PRIx64 " hi=0x%" PRIx64 ")\n",
               EXP_LO, EXP_HI);
    }
}

int main(void) {
    assign_to_core(0);

    printf("=== MUL slot-position test ===\n");
    printf("Inputs: a=0x%016" PRIx64 "  b=0x%016" PRIx64 "\n",
           IN_A, IN_B);
    printf("Expected: lo=0x%016" PRIx64 "  hi=0x%016" PRIx64 "\n\n",
           EXP_LO, EXP_HI);

    uint64_t lo, hi;

    /* Run each test 3 times to verify determinism. */
    printf("Test A — MUL in slot 0 (control, known working):\n");
    install_test_a();
    for (int i = 0; i < 3; i++) {
        run_patch(&lo, &hi);
        diagnose("  run", lo, hi);
    }

    printf("\nTest B — MUL in slot 1 (control, known working):\n");
    install_test_b();
    for (int i = 0; i < 3; i++) {
        run_patch(&lo, &hi);
        diagnose("  run", lo, hi);
    }

    printf("\nTest C — MUL in slot 2 (the unknown):\n");
    install_test_c();
    for (int i = 0; i < 3; i++) {
        run_patch(&lo, &hi);
        diagnose("  run", lo, hi);
    }

    /* Clean up the hook */
    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
