/*
 * probe_keccak_io.c — Bisect the Phase 3a hang.
 *
 * Tests progressively larger LDZX+STAD patches to find the wall.
 * Each step: load N lanes into mixed arch+TMP, store back, verify.
 *
 * Build: make PROG=probe_keccak_io
 * Run:   sudo taskset -c 0 ./probe_keccak_io_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#ifndef SEG
#define SEG 3
#endif

static uint64_t g_in[25];
static uint64_t g_out[25];

#define LD_T(reg, i)  { LDZX_DSZ64_ASZ32_SC1_DRI(reg, RCX, (i)*8, SEG), NOP, NOP, NOP_SEQWORD }
#define ST_T(reg, i)  { STAD_DSZ64_ASZ32_SC1_RRI(reg, RDX, (i)*8, SEG), NOP, NOP, NOP_SEQWORD }

/* dst registers in the order we'd use them. Mix arch + TMP. */
static const int dst_order[25] = {
    RDI, RSI, RBX, RDX, RBP, R8, R9, R10, R11, R12, R13, R14, R15,
    TMP0, TMP1, TMP2, TMP3, TMP4, TMP5, TMP6, TMP7, TMP8, TMP9, TMP10, TMP11,
};

static void fire(void) {
    /* test_memops pattern: hook on rdrand (0x0428), trigger via `rdrand rax`.
     * RCX and RDX hold base pointers before the trigger. */
    asm volatile(
        "lea rcx, [%[in]]\n\t"
        "lea rdx, [%[out]]\n\t"
        "rdrand rax\n\t"
        :
        : [in] "m"(g_in[0]), [out] "m"(g_out[0])
        : "rax", "rbx", "rcx", "rdx", "rdi", "rsi", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );
}

static int test_n(int n) {
    printf("--- N=%d ---\n", n);
    fflush(stdout);

    /* Build patch: load lanes 0..n-1 into dst_order[0..n-1] via RCX base,
     * then store them via RDX base. END at end. */
    ucode_t patch[2 * 25 + 1];  /* worst case */
    int k = 0;
    for (int i = 0; i < n; i++) {
        patch[k++] = (ucode_t)LD_T(dst_order[i], i);
    }
    for (int i = 0; i < n; i++) {
        patch[k++] = (ucode_t)ST_T(dst_order[i], i);
    }
    /* Replace seqword of last triad with END_SEQWORD. */
    patch[k-1].seqw = END_SEQWORD;

    init_match_and_patch();
    do_fix_IN_patch();
    hook_match_and_patch(0, 0x0428, 0x7c00);  /* rdrand hook (test_memops pattern) */
    patch_ucode(0x7c00, patch, k);

    /* Seed inputs, zero outputs. */
    for (int i = 0; i < 25; i++) {
        g_in[i]  = 0xA000000000000000ULL | i;
        g_out[i] = 0;
    }

    fire();

    /* Verify. */
    int fails = 0;
    for (int i = 0; i < n; i++) {
        if (g_out[i] != g_in[i]) {
            if (fails < 3)
                printf("  [%2d] in=%016" PRIx64 "  out=%016" PRIx64 "  ***\n",
                       i, g_in[i], g_out[i]);
            fails++;
        }
    }
    printf("  N=%d: %s (%d/%d lanes matched)\n",
           n, fails == 0 ? "PASS" : "FAIL", n - fails, n);
    return fails;
}

int main(void) {
    printf("=== Keccak I/O bisect (SEG=%d) ===\n", SEG);
    printf("g_in @ %p  g_out @ %p\n",
           (void*)g_in, (void*)g_out);
    if ((uint64_t)g_in >= 0x100000000ULL || (uint64_t)g_out >= 0x100000000ULL) {
        printf("FATAL: buffer above 4GB. Need -no-pie.\n");
        return 1;
    }

    assign_to_core(0);

    /* Walk N upward. */
    int tested[] = { 1, 2, 5, 10, 13, 14, 15, 20, 25 };
    for (size_t i = 0; i < sizeof(tested)/sizeof(tested[0]); i++) {
        if (test_n(tested[i]) != 0) {
            printf("Stopping at first failure.\n");
            break;
        }
    }

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
