/*
 * test_two_patch.c — Minimal test of two-patch vmwrite/vmread mechanism
 *
 * Constants loaded into registers BEFORE vmwrite.
 * Patches use ADD_DSZ64_DRR (register-register only, no immediates).
 *
 * Build: make PROG=test_two_patch
 * Run:   sudo taskset -c 0 ./test_two_patch_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

int main(void) {
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    printf("=== Two-patch test (register ops only) ===\n\n");

    /*
     * Setup: caller loads R9=0xAA, R10=0xBB before triggering.
     *
     * Patch A (vmwrite): RAX = RAX + R9        → RAX = 0xAA
     * Patch B (vmread):  RAX = RAX + R10       → RAX = 0xAA + 0xBB = 0x165
     *                    R11 = R9 + R10        → R11 = 0xAA + 0xBB = 0x165
     */

    ucode_t patch_a[] = {
        { ADD_DSZ64_DRR(RAX, RAX, R9),   /* RAX += R9 (=0xAA) */
          NOP, NOP, END_SEQWORD }
    };

    ucode_t patch_b[] = {
        { ADD_DSZ64_DRR(RAX, RAX, R10),  /* RAX += R10 (=0xBB) */
          ADD_DSZ64_DRR(R11, R9, R10),   /* R11 = R9 + R10 (tests state from A) */
          NOP, END_SEQWORD }
    };

    uint64_t addr_a = 0x7c00;
    uint64_t addr_b = addr_a + ARRAY_SZ(patch_a) * 4;

    printf("Patch A: %d triad at U%04lx\n", (int)ARRAY_SZ(patch_a), addr_a);
    printf("Patch B: %d triad at U%04lx\n\n", (int)ARRAY_SZ(patch_b), addr_b);

    patch_ucode(addr_a, patch_a, ARRAY_SZ(patch_a));
    hook_match_and_patch(0, 0x0cd8, addr_a);

    patch_ucode(addr_b, patch_b, ARRAY_SZ(patch_b));
    hook_match_and_patch(1, 0x0618, addr_b);

    /* ── Test 1: Patch A only ── */
    uint64_t res;
    asm volatile(
        "xor eax, eax\n\t"
        "mov r9, 0xAA\n\t"
        "mov r10, 0xBB\n\t"
        "vmwrite rcx, rdx\n\t"
        : "=a"(res) : : "rcx", "rdx", "r9", "r10", "r11", "memory", "cc"
    );
    printf("Test 1 (vmwrite): RAX=0x%lx expect 0xAA → %s\n",
           res, res == 0xAA ? "PASS" : "FAIL");

    /* ── Test 2: Patch B only ── */
    asm volatile(
        "xor eax, eax\n\t"
        "mov r9, 0xAA\n\t"
        "mov r10, 0xBB\n\t"
        ".byte 0x0f, 0x78, 0xca\n\t"
        : "=a"(res) : : "rcx", "rdx", "r9", "r10", "r11", "memory", "cc"
    );
    printf("Test 2 (vmread):  RAX=0x%lx expect 0xBB → %s\n",
           res, res == 0xBB ? "PASS" : "FAIL");

    /* ── Test 3: A then B sequentially ── */
    uint64_t results[3];
    register uint64_t *_p asm("r15") = results;
    asm volatile(
        "push r15\n\t"
        "xor eax, eax\n\t"
        "mov r9, 0xAA\n\t"
        "mov r10, 0xBB\n\t"
        "xor r11d, r11d\n\t"
        "vmwrite rcx, rdx\n\t"        /* A: RAX=0xAA */
        ".byte 0x0f, 0x78, 0xca\n\t"  /* B: RAX+=0xBB=0x165, R11=0xAA+0xBB=0x165 */
        "pop r15\n\t"
        "mov [r15],    rax\n\t"
        "mov [r15+8],  r9\n\t"
        "mov [r15+16], r11\n\t"
        : "+r"(_p)
        :
        : "rax", "rcx", "rdx", "r9", "r10", "r11", "memory", "cc"
    );
    printf("Test 3 (A then B):\n");
    printf("  RAX=0x%lx  expect 0x165 (0xAA+0xBB)\n", results[0]);
    printf("  R9 =0x%lx  expect 0xAA  (unchanged by patches)\n", results[1]);
    printf("  R11=0x%lx  expect 0x165 (R9+R10, computed in B)\n", results[2]);
    int pass3 = (results[0] == 0x165 && results[1] == 0xAA && results[2] == 0x165);
    printf("  %s\n", pass3 ? "PASS" : "FAIL");

    /* ── Test 4: TMP persistence across patches ── */
    init_match_and_patch();
    do_fix_IN_patch();

    /* Patch A2: TMP0 = R9 (save register to TMP) */
    ucode_t patch_a2[] = {
        { ZEROEXT_DSZ64_DR(TMP0, R9),
          NOP, NOP, END_SEQWORD }
    };
    /* Patch B2: RAX = TMP0 (read TMP set by A2) */
    ucode_t patch_b2[] = {
        { ZEROEXT_DSZ64_DR(RAX, TMP0),
          NOP, NOP, END_SEQWORD }
    };

    addr_a = 0x7c00;
    addr_b = addr_a + ARRAY_SZ(patch_a2) * 4;

    patch_ucode(addr_a, patch_a2, ARRAY_SZ(patch_a2));
    hook_match_and_patch(0, 0x0cd8, addr_a);
    patch_ucode(addr_b, patch_b2, ARRAY_SZ(patch_b2));
    hook_match_and_patch(1, 0x0618, addr_b);

    asm volatile(
        "xor eax, eax\n\t"
        "mov r9, 0x42\n\t"
        "vmwrite rcx, rdx\n\t"        /* A2: TMP0 = 0x42 */
        ".byte 0x0f, 0x78, 0xca\n\t"  /* B2: RAX = TMP0 */
        : "=a"(res) : : "rcx", "rdx", "r9", "memory", "cc"
    );
    printf("\nTest 4 (TMP persistence): RAX=0x%lx expect 0x42 → %s\n",
           res, res == 0x42 ? "PASS" : "FAIL");

    int all_pass = pass3 && (res == 0x42);
    printf("\n=== Two-patch mechanism: %s ===\n", all_pass ? "WORKS" : "BROKEN");

    init_match_and_patch();
    do_fix_IN_patch();
    return all_pass ? 0 : 1;
}
