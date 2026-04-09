/*
 * patch_ram_capacity_test.c — Find usable patch RAM depth from a given address
 *
 * Strategy: Progressively test chains of N triads starting at REGION_START.
 * Each chain of length N:
 *   - Triads 0..N-2: each XORs a unique value into TMP0
 *   - Triad N-1: moves TMP0 to RAX + END_SEQWORD
 *
 * The unique value per triad is simply (i+1), so for N triads:
 *   expected = XOR of (1, 2, 3, ..., N-1)  [triad 0 initializes TMP0]
 *
 * We increase N from 2 up to MAX_TEST_LEN. The first N that fails
 * (wrong result or hang) reveals the limit.
 *
 * Hook: vmwrite rcx, rcx (0x0cd8)
 * Output: RAX = accumulated XOR
 *
 * Build: make PROG=patch_ram_capacity_test
 * Run:   sudo taskset -c 0 ./patch_ram_capacity_test_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <signal.h>
#include <setjmp.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define REGION_START  0x7d7c   /* 133 free slots per your dump */
#define MP_INDEX      19       /* free match-and-patch slot */
#define HOOK_ADDR     0x0cd8   /* vmwrite */

/* Test up to this many triads. 133 is the reported free size.
 * We test a bit beyond to find the real wall. */
#define MAX_TEST_LEN  140

static jmp_buf jmpbuf;
static volatile int got_signal = 0;

static void sig_handler(int sig) {
    got_signal = sig;
    longjmp(jmpbuf, 1);
}

static inline uint64_t do_vmwrite(uint64_t val) {
    uint64_t result;
    asm volatile(
        "mov rcx, %[v]\n\t"
        "vmwrite rcx, rcx\n\t"
        : "=a"(result)
        : [v] "r"(val)
        : "rcx", "rdx", "r8", "memory"
    );
    return result;
}

/*
 * Build a chain of `len` triads:
 *   Triad 0:     MOVE TMP0, 1       (initialize accumulator)
 *   Triad 1..N-2: XOR TMP0, TMP0, (i+1)  via MOVE TMP1 + XOR
 *   Triad N-1:   MOVE RAX, TMP0 + END_SEQWORD
 *
 * Returns expected RAX value.
 */
static uint64_t install_chain(int len) {
    ucode_t *patch = (ucode_t *)calloc(len, sizeof(ucode_t));
    if (!patch) {
        perror("calloc");
        exit(1);
    }

    uint64_t expected = 0;

    for (int i = 0; i < len; i++) {
        if (i == 0) {
            /* First triad: initialize TMP0 = 1 */
            patch[i].uop0 = MOVE_DSZ64_DI(TMP0, 1);
            patch[i].uop1 = NOP;
            patch[i].uop2 = NOP;
            patch[i].seqw = NOP_SEQWORD;
            expected = 1;
        } else if (i < len - 1) {
            /* Middle triads: TMP1 = (i+1), TMP0 ^= TMP1 */
            uint64_t val = (uint64_t)(i + 1);
            patch[i].uop0 = MOVE_DSZ64_DI(TMP1, val);
            patch[i].uop1 = XOR_DSZ64_DRR(TMP0, TMP0, TMP1);
            patch[i].uop2 = NOP;
            patch[i].seqw = NOP_SEQWORD;
            expected ^= val;
        } else {
            /* Last triad: RAX = TMP0, end */
            patch[i].uop0 = MOVE_DSZ64_DR(RAX, TMP0);
            patch[i].uop1 = NOP;
            patch[i].uop2 = NOP;
            patch[i].seqw = END_SEQWORD;
        }
    }

    patch_ucode(REGION_START, patch, len);
    hook_match_and_patch(MP_INDEX, HOOK_ADDR, REGION_START);

    free(patch);
    return expected;
}

int main(void) {
    printf("=== Patch RAM capacity test @ U%04x ===\n\n", REGION_START);

    assign_to_core(0);
    do_fix_IN_patch();

    /* Catch crashes from bad microcode */
    signal(SIGSEGV, sig_handler);
    signal(SIGILL, sig_handler);
    signal(SIGBUS, sig_handler);

    int last_pass = 0;

    for (int len = 2; len <= MAX_TEST_LEN; len++) {
        uint64_t expected = install_chain(len);

        got_signal = 0;
        if (setjmp(jmpbuf) != 0) {
            printf("  len=%3d: CRASH (signal %d) — limit found\n", len, got_signal);
            break;
        }

        uint64_t result = do_vmwrite(0);

        if (result == expected) {
            last_pass = len;
            /* Print every 10 and a few key points */
            if (len <= 5 || len % 10 == 0 || len >= 125) {
                printf("  len=%3d: RAX=0x%04" PRIx64 "  expected=0x%04" PRIx64 "  PASS\n",
                       len, result, expected);
            }
        } else {
            printf("  len=%3d: RAX=0x%04" PRIx64 "  expected=0x%04" PRIx64 "  FAIL\n",
                   len, result, expected);
            printf("\n  First failure at len=%d\n", len);
            printf("  Last passing length: %d triads\n", last_pass);
            break;
        }

        if (len == MAX_TEST_LEN) {
            printf("\n  All %d lengths passed!\n", MAX_TEST_LEN);
        }
    }

    printf("\n  Maximum verified usable triads from U%04x: %d\n", REGION_START, last_pass);
    printf("  That gives you addresses U%04x - U%04x\n",
           REGION_START, REGION_START + (last_pass - 1) * 4);

    return 0;
}
