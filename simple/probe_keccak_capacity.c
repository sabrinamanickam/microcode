/*
 * probe_keccak_capacity.c — Phase 1: how many triads can we install at U7c00?
 *
 * Strategy: after init_match_and_patch() (wipes all 32 match-and-patch hook
 * slots), install N consecutive triads starting at U7c00 with the last one
 * carrying END_SEQWORD. Each non-terminal triad XORs (i+1) into TMP0. The
 * terminal triad copies TMP0 to RAX. If RAX matches the C-side XOR sum, the
 * chain executed end-to-end.
 *
 * Walk N from 2 upward to find the wall. Previous testing said the BIOS-resident
 * patches in slots 1-18 cap usable capacity at ~25 triads from any single
 * fragment, but those are precisely the hooks `init_match_and_patch()` clears.
 * Starting at U7c00 puts seqwords in bank 0 at entries [0..N-1] — bank 0 has
 * 128 entries, so the upper bound is 128.
 *
 * KEY: `hook_match_and_patch()` writes a 5-triad bootstrap to U7de0-U7df0
 * (see source/patch.c). The bootstrap is invoked once to plumb the hook, then
 * dead. If we call hook_match_and_patch BEFORE patch_ucode, the bootstrap is
 * already dead by the time our Keccak patch is written — so patch_ucode can
 * overwrite U7de0-U7df0 safely. v1 of this probe called hook after patch each
 * iter and hit a wall at 120; v2 installs the hook once up front and tests
 * the full 128-triad bank-0 ceiling.
 *
 * MOVE is broken on this hardware — see feedback_move_dsz64. Use ZEROEXT
 * everywhere (DSZ64_DR for reg-to-reg, DSZ32_DI for small immediates).
 *
 * Build: make PROG=probe_keccak_capacity
 * Run:   sudo taskset -c 0 ./probe_keccak_capacity_static
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

#define REGION_START 0x7c00
#define HOOK_ADDR    0x0cd8    /* vmwrite — same hook the rest of the project uses */
#define MAX_TEST_LEN 130       /* bank 0 has 128 seqword entries; probe past it */

static jmp_buf jmpbuf;
static volatile int got_signal = 0;

static void sig_handler(int sig) {
    got_signal = sig;
    longjmp(jmpbuf, 1);
}

static inline uint64_t fire_hook(void) {
    uint64_t result;
    asm volatile(
        "xor rcx, rcx\n\t"
        "vmwrite rcx, rcx\n\t"
        : "=a"(result)
        :
        : "rcx", "rdx", "r8", "memory"
    );
    return result;
}

/*
 * Build a chain of `len` triads at U7c00.
 *   Triad 0:     TMP0 = 1                              (zeroext imm)
 *   Triad 1..N-2: TMP1 = (i+1); TMP0 ^= TMP1            (zeroext imm + xor)
 *   Triad N-1:   RAX = TMP0; END_SEQWORD                (zeroext reg)
 *
 * Returns expected RAX value (running XOR of 1..N-1).
 */
static uint64_t install_chain(int len) {
    ucode_t *patch = (ucode_t *)calloc(len, sizeof(ucode_t));
    if (!patch) { perror("calloc"); exit(1); }

    uint64_t expected = 0;

    for (int i = 0; i < len; i++) {
        if (i == 0) {
            patch[i].uop0 = ZEROEXT_DSZ32_DI(TMP0, 1);
            patch[i].uop1 = NOP;
            patch[i].uop2 = NOP;
            patch[i].seqw = NOP_SEQWORD;
            expected = 1;
        } else if (i < len - 1) {
            uint64_t val = (uint64_t)(i + 1);
            patch[i].uop0 = ZEROEXT_DSZ32_DI(TMP1, val);
            patch[i].uop1 = XOR_DSZ64_DRR(TMP0, TMP0, TMP1);
            patch[i].uop2 = NOP;
            patch[i].seqw = NOP_SEQWORD;
            expected ^= val;
        } else {
            patch[i].uop0 = ZEROEXT_DSZ64_DR(RAX, TMP0);
            patch[i].uop1 = NOP;
            patch[i].uop2 = NOP;
            patch[i].seqw = END_SEQWORD;
        }
    }

    /* Hook is installed ONCE in main() before the loop — see below.
     * We only need to re-write the patch RAM each iteration. */
    patch_ucode(REGION_START, patch, len);

    free(patch);
    return expected;
}

int main(void) {
    printf("=== Phase 1 probe: usable triads at U%04x after init ===\n\n", REGION_START);

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    /* Install the vmwrite hook ONCE up front so its bootstrap at U7de0-U7df0
     * gets invoked-then-dies before our Keccak patch is written. Subsequent
     * patch_ucode calls can extend into U7de0-U7dfc safely. */
    hook_match_and_patch(0, HOOK_ADDR, REGION_START);

    signal(SIGSEGV, sig_handler);
    signal(SIGILL,  sig_handler);
    signal(SIGBUS,  sig_handler);

    int last_pass = 0;
    int first_fail = -1;

    for (int len = 2; len <= MAX_TEST_LEN; len++) {
        uint64_t expected = install_chain(len);

        got_signal = 0;
        if (setjmp(jmpbuf) != 0) {
            printf("  len=%3d: CRASH (signal %d)\n", len, got_signal);
            first_fail = len;
            break;
        }

        uint64_t result = fire_hook();
        if (result == expected) {
            last_pass = len;
            if (len <= 5 || len % 10 == 0 || (len >= 60 && len <= 70) || len >= 125) {
                printf("  len=%3d: RAX=0x%04" PRIx64 "  expected=0x%04" PRIx64 "  PASS\n",
                       len, result, expected);
            }
        } else {
            printf("  len=%3d: RAX=0x%04" PRIx64 "  expected=0x%04" PRIx64 "  FAIL\n",
                   len, result, expected);
            first_fail = len;
            break;
        }
    }

    printf("\n  Last passing length: %d triads\n", last_pass);
    if (first_fail < 0)
        printf("  All %d tested lengths passed.\n", MAX_TEST_LEN);
    else
        printf("  First failing length: %d\n", first_fail);
    printf("  Patch occupied U%04x – U%04x.\n",
           REGION_START, REGION_START + (last_pass - 1) * 4);

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
