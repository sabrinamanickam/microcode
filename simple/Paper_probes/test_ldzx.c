/*
 * test_ldzx.c — Minimal LDZX smoke test
 *
 * Tries LDZX_DSZ64_ASZ32_SC1 with various SEG values to find
 * which one works on this NUC.  Single-triad patch: load [RCX] → RAX, END.
 *
 * Build: make PROG=test_ldzx
 * Run:   sudo taskset -c 0 ./test_ldzx_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <signal.h>
#include <setjmp.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/ldat.h"
#include "../../include/misc.h"

static sigjmp_buf jmpbuf;
static volatile sig_atomic_t got_signal = 0;

static void handler(int sig) {
    got_signal = sig;
    siglongjmp(jmpbuf, sig);
}

/* Static canary — guaranteed < 4GB with -static */
static volatile uint64_t g_canary __attribute__((aligned(8))) = 0xDEADBEEFCAFE1234ULL;
static volatile uint64_t g_array[5] __attribute__((aligned(64))) = {111, 222, 333, 444, 555};

int main(void) {
    struct sigaction sa = {0};
    sa.sa_handler = handler;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);

    assign_to_core(0);

    printf("=== LDZX smoke test ===\n\n");
    printf("g_canary at %p (fits ASZ32: %s)\n",
           (void*)&g_canary,
           (uint64_t)&g_canary < 0x100000000ULL ? "YES" : "NO");
    printf("g_array  at %p (fits ASZ32: %s)\n\n",
           (void*)g_array,
           (uint64_t)g_array < 0x100000000ULL ? "YES" : "NO");

    if ((uint64_t)&g_canary >= 0x100000000ULL) {
        printf("FATAL: static data above 4GB. Rebuild with -no-pie.\n");
        return 1;
    }

    /* ── Test 1: LDZX [RCX] → RAX with known SEG values ────────── */
    printf("--- Test 1: LDZX [RCX] -> RAX, single value ---\n");
    int segs[] = { SEG_DS, SEG_SS, SEG_ES, SEG_CS, SEG_PHYS, 0, 3, 6 };
    const char *seg_names[] = { "DS(0x18)", "SS(0x1a)", "ES(0x08)", "CS(0x09)",
                                "PHYS(0x01)", "0", "3", "6" };
    int nsegs = sizeof(segs)/sizeof(segs[0]);
    for (int si = 0; si < nsegs; si++) {
        int seg = segs[si];
        init_match_and_patch();
        do_fix_IN_patch();

        /* Build 1-triad patch: LDZX(RAX, RCX, seg), NOP, NOP, END */
        ucode_t patch[1] = {{
            LDZX_DSZ64_ASZ32_SC1_DR(RAX, RCX, seg),
            NOP, NOP, END_SEQWORD
        }};
        patch_ucode(0x7c00, patch, 1);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);

        got_signal = 0;
        alarm(3);
        if (sigsetjmp(jmpbuf, 1) != 0) {
            printf("  SEG=%-10s: %s (signal %d)\n", seg_names[si],
                   got_signal == SIGALRM ? "HUNG" : "CRASHED",
                   (int)got_signal);
            init_match_and_patch();
            do_fix_IN_patch();
            continue;
        }

        uint64_t result;
        asm volatile(
            "lea rcx, [%[addr]]\n\t"
            "vmwrite rcx, rcx\n\t"
            : "=a"(result)
            : [addr] "m"(g_canary)
            : "rcx", "rdx", "r8", "memory"
        );
        alarm(0);

        if (result == 0xDEADBEEFCAFE1234ULL)
            printf("  SEG=%-10s: PASS (got 0x%016" PRIx64 ")\n", seg_names[si], result);
        else
            printf("  SEG=%-10s: WRONG (got 0x%016" PRIx64 ", expect 0xDEADBEEFCAFE1234)\n",
                   seg_names[si], result);
    }

    /* ── Test 2: LDZX with offset — load g_array[2] (=333) ────── */
    printf("\n--- Test 2: LDZX [RCX+16] -> RAX (offset load) ---\n");
    for (int si = 0; si < nsegs; si++) {
        int seg = segs[si];
        init_match_and_patch();
        do_fix_IN_patch();

        ucode_t patch[1] = {{
            LDZX_DSZ64_ASZ32_SC1_DRI(RAX, RCX, 16, seg),
            NOP, NOP, END_SEQWORD
        }};
        patch_ucode(0x7c00, patch, 1);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);

        got_signal = 0;
        alarm(3);
        if (sigsetjmp(jmpbuf, 1) != 0) {
            printf("  SEG=%-10s: %s\n", seg_names[si],
                   got_signal == SIGALRM ? "HUNG" : "CRASHED");
            init_match_and_patch();
            do_fix_IN_patch();
            continue;
        }

        uint64_t result;
        asm volatile(
            "lea rcx, [%[addr]]\n\t"
            "vmwrite rcx, rcx\n\t"
            : "=a"(result)
            : [addr] "m"(g_array[0])
            : "rcx", "rdx", "r8", "memory"
        );
        alarm(0);

        if (result == 333)
            printf("  SEG=%-10s: PASS (got %" PRIu64 ")\n", seg_names[si], result);
        else
            printf("  SEG=%-10s: WRONG (got %" PRIu64 ", expect 333)\n", seg_names[si], result);
    }

    /* ── Test 3: STAD — store RAX to [RDX], verify ────────────── */
    printf("\n--- Test 3: STAD RAX -> [RDX] (store test) ---\n");
    for (int si = 0; si < nsegs; si++) {
        int seg = segs[si];
        init_match_and_patch();
        do_fix_IN_patch();

        ucode_t patch[2] = {
            { ZEROEXT_DSZ64_DR(RAX, RCX), NOP, NOP, NOP_SEQWORD },
            { STAD_DSZ64_ASZ32_SC1_RR(RAX, RDX, seg),
              NOP, NOP, END_SEQWORD }
        };
        patch_ucode(0x7c00, patch, 2);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);

        static volatile uint64_t g_store_target = 0;
        g_store_target = 0;

        got_signal = 0;
        alarm(3);
        if (sigsetjmp(jmpbuf, 1) != 0) {
            printf("  SEG=%-10s: %s\n", seg_names[si],
                   got_signal == SIGALRM ? "HUNG" : "CRASHED");
            init_match_and_patch();
            do_fix_IN_patch();
            continue;
        }

        uint64_t marker = 0xAAAABBBBCCCCDDDDULL;
        asm volatile(
            "mov rcx, %[val]\n\t"
            "lea rdx, [%[dst]]\n\t"
            "vmwrite rcx, rdx\n\t"
            :
            : [val] "r"(marker), [dst] "m"(g_store_target)
            : "rcx", "rdx", "rax", "r8", "memory"
        );
        alarm(0);

        if (g_store_target == marker)
            printf("  SEG=%-10s: PASS (stored 0x%016" PRIx64 ")\n", seg_names[si], g_store_target);
        else
            printf("  SEG=%-10s: WRONG (stored 0x%016" PRIx64 ", expect 0x%016" PRIx64 ")\n",
                   seg_names[si], g_store_target, marker);
    }

    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone. M&P reset.\n");
    return 0;
}
