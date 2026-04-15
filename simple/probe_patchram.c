/*
 * probe_patchram.c — Determine usable Patch RAM size
 *
 * Tests:
 * 1. Seqword array (ms_array 2): write unique values to increasing addresses,
 *    read back to find aliasing (wrap) and maximum valid address.
 * 2. Code array (ms_array 4): same approach.
 * 3. Functional test: install a 1-triad patch at various base addresses,
 *    hook + invoke, check if the result is correct.
 *
 * Build: make PROG=probe_patchram
 * Run:   sudo taskset -c 0 ./probe_patchram_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/ldat.h"
#include "../../include/misc.h"

/* ── Test 1: Probe seqword array (ms_array 2) size ──────────── */

static void probe_seqword_array(void) {
    printf("=== Seqword array (ms_array 2) probe ===\n");

    /* Write unique marker values to addresses 0..255 */
    int max_writable = 0;
    for (int i = 0; i < 256; i++) {
        uint64_t marker = 0xA5000000ULL | i;
        ms_array_write(2, 0, 0, i, marker);
    }

    /* Read back and check for aliasing */
    printf("  Addr  Written     ReadBack    Alias?\n");
    int first_alias = -1;
    for (int i = 0; i < 256; i++) {
        uint64_t val = ms_array_read(2, 0, 0, i);
        uint64_t expected = 0xA5000000ULL | i;
        /* Mask to relevant bits (seqwords are 30 bits) */
        uint64_t got = val & 0x3FFFFFFFULL;
        uint64_t exp_masked = expected & 0x3FFFFFFFULL;
        int alias = (got != exp_masked);
        if (alias && first_alias < 0) first_alias = i;
        if (i < 140 || alias) {  /* print around boundaries */
            printf("  %3d   0x%08lx  0x%08lx  %s\n",
                   i, exp_masked, got,
                   alias ? "<-- ALIAS" : "ok");
        }
        if (!alias) max_writable = i;
    }
    printf("\n  Max valid seqword address: %d\n", max_writable);
    if (first_alias >= 0)
        printf("  First alias at address: %d (aliases with %d)\n",
               first_alias, first_alias % 128);
    printf("\n");
}

/* ── Test 2: Probe code array (ms_array 4) size ────────────── */

static void probe_code_array(void) {
    printf("=== Code array (ms_array 4) probe ===\n");

    /* Write unique markers to addresses 0..767 (enough for 256 triads * 3 uops) */
    int max_writable = 0;
    int test_range = 768;
    for (int i = 0; i < test_range; i++) {
        uint64_t marker = 0xC0DE0000ULL | i;
        ms_array_write(4, 0, 0, i, marker);
    }

    /* Read back and check */
    int first_alias = -1;
    for (int i = 0; i < test_range; i++) {
        uint64_t val = ms_array_read(4, 0, 0, i);
        uint64_t expected = 0xC0DE0000ULL | i;
        /* uops are 48 bits */
        uint64_t mask = 0xFFFFFFFFFFFFULL;
        uint64_t got = val & mask;
        uint64_t exp_masked = expected & mask;
        int alias = (got != exp_masked);
        if (alias && first_alias < 0) first_alias = i;
        if (!alias) max_writable = i;
    }
    printf("  Max valid code address: %d (0x%x)\n", max_writable, max_writable);
    if (first_alias >= 0)
        printf("  First alias at address: %d (0x%x)\n", first_alias, first_alias);
    printf("\n");
}

/* ── Test 3: Functional patch probe ─────────────────────────── */

static void functional_probe(void) {
    printf("=== Functional patch probe (1-triad at various addresses) ===\n");
    printf("  NOTE: hook_match_and_patch() uses 0x7de0-0x7df0 as staging area\n");
    printf("  Testing which ucode base addresses produce correct results...\n\n");

    /*
     * Patch: 2-triad test. Triad 0: RAX = 0x42. Triad 1: RAX = RAX (nop-ish) + END.
     * We need >=2 triads because hook_match_and_patch's staging code at 0x7de0
     * clobbers seqwords 120-124. With 1 triad, the seqword might get stomped.
     *
     * Actually the simpler approach: just write a 1-triad patch and check the result.
     * hook_match_and_patch writes to 0x7de0 area, which only clobbers seqword slots
     * 120-124 and code at offsets 0x1E0+. As long as our test patch is below triad 120,
     * there's no conflict.
     */
    int pass_count = 0;
    int first_fail = -1;
    int last_pass = -1;

    /* Step by 4 (triad-aligned) from 0x7c00 up to before the staging area */
    for (int triad = 0; triad < 128; triad++) {
        uint64_t addr = 0x7c00 + triad * 4;

        /* Clean slate each time */
        init_match_and_patch();
        do_fix_IN_patch();

        /* 1-triad patch: set RAX = triad number + 0x1000 */
        uint64_t marker = 0x1000 + triad;
        ucode_t patch[] = {
            { ZEROEXT_DSZ32_DI(RAX, marker),
              NOP, NOP, END_SEQWORD }
        };

        patch_ucode(addr, patch, 1);
        /* hook_match_and_patch writes staging code to 0x7de0 (triads 120-124) */
        hook_match_and_patch(0, 0x0cd8, addr);

        /* Invoke: fire vmwrite, check RAX */
        uint64_t result;
        asm volatile(
            "vmwrite rcx, rdx\n\t"
            : "=a"(result)
            :
            : "rcx", "rdx", "rbx", "memory", "cc"
        );

        int ok = (result == marker);
        if (ok) {
            pass_count++;
            last_pass = triad;
        } else if (first_fail < 0) {
            first_fail = triad;
        }

        /* Print selectively */
        if (triad < 5 || !ok || triad >= 115 || triad % 20 == 0) {
            printf("  triad %3d (U%04lx): %s  (expect 0x%04lx, got 0x%lx)\n",
                   triad, addr, ok ? "PASS" : "FAIL", marker, result);
        }
    }

    printf("\n  Passed: %d / 128\n", pass_count);
    printf("  Last passing triad: %d (U%04x)\n", last_pass, 0x7c00 + last_pass*4);
    if (first_fail >= 0)
        printf("  First failing triad: %d (U%04x)\n", first_fail, 0x7c00 + first_fail*4);
    printf("  => Usable range: triads 0..%d  (%d triads, U7c00..U%04x)\n",
           last_pass, last_pass + 1, 0x7c00 + last_pass * 4);
    printf("\n");
}

/* ── Test 4: Two-patch coexistence test ─────────────────────── */

static void two_patch_test(void) {
    printf("=== Two-patch coexistence test ===\n");
    printf("  Patch A (vmwrite hook): RAX = RAX | 0xAA\n");
    printf("  Patch B (vmread  hook): RAX = RAX | 0xBB\n\n");

    /* Try placing B at increasing offsets after A */
    for (int b_triad = 1; b_triad < 128; b_triad++) {
        uint64_t addr_a = 0x7c00;
        uint64_t addr_b = 0x7c00 + b_triad * 4;

        init_match_and_patch();
        do_fix_IN_patch();

        /* Patch A: 1 triad at addr_a */
        ucode_t patch_a[] = {
            { OR_DSZ64_DRM(RAX, RAX, 0xAA),
              NOP, NOP, END_SEQWORD }
        };
        patch_ucode(addr_a, patch_a, 1);
        hook_match_and_patch(0, 0x0cd8, addr_a);  /* vmwrite */

        /* Patch B: 1 triad at addr_b */
        ucode_t patch_b[] = {
            { OR_DSZ64_DRM(RAX, RAX, 0xBB),
              NOP, NOP, END_SEQWORD }
        };
        patch_ucode(addr_b, patch_b, 1);
        hook_match_and_patch(1, 0x0618, addr_b);  /* vmread */

        /* Test patch A (vmwrite) */
        uint64_t res_a;
        asm volatile(
            "xor eax, eax\n\t"
            "vmwrite rcx, rdx\n\t"
            : "=a"(res_a) : : "rcx", "rdx", "memory", "cc"
        );

        /* Test patch B (vmread) */
        uint64_t res_b;
        asm volatile(
            "xor eax, eax\n\t"
            ".byte 0x0f, 0x78, 0xca\n\t"  /* vmread rdx, rcx (reg,reg form) */
            : "=a"(res_b) : : "rcx", "rdx", "memory", "cc"
        );

        int ok_a = (res_a == 0xAA);
        int ok_b = (res_b == 0xBB);

        if (b_triad <= 5 || !ok_a || !ok_b ||
            b_triad % 16 == 0 || b_triad >= 125) {
            printf("  B@triad %3d (U%04lx): A=%s(0x%02lx) B=%s(0x%02lx)%s\n",
                   b_triad, addr_b,
                   ok_a ? "ok" : "FAIL", res_a,
                   ok_b ? "ok" : "FAIL", res_b,
                   (ok_a && ok_b) ? "" : " <---");
        }
    }
    printf("\n");
}

int main(void) {
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    probe_seqword_array();
    probe_code_array();
    functional_probe();
    two_patch_test();

    /* clean up */
    init_match_and_patch();
    do_fix_IN_patch();
    printf("Done.\n");
    return 0;
}
