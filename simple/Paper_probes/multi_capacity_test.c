/*
 * multi_region_capacity_test.c — Find usable triad capacity per free region
 *
 * Tests each 4-aligned free region to find how many triads it can hold
 * before seqword addressing breaks.
 *
 * After mapping capacities, reports a placement plan for 55 triads
 * (with link triads accounted for).
 *
 * Hook: vmwrite rcx, rcx (0x0cd8)
 * Build: make PROG=multi_region_capacity_test
 * Run:   sudo taskset -c 0 ./multi_region_capacity_test_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>
#include <setjmp.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MP_INDEX    19
#define HOOK_ADDR   0x0cd8

/* Free regions from your dump, 4-aligned start addresses.
 * For regions not 4-aligned, we round up to next 4-aligned addr
 * and reduce size accordingly.
 *
 * Original dump:
 *   U7c47 size=24  -> align to U7c48, size=23
 *   U7c6b size=13  -> align to U7c6c, size=12
 *   U7c98 size=6   -> already aligned, size=6
 *   U7cc7 size=24  -> align to U7cc8, size=23
 *   U7cea size=14  -> align to U7cec, size=12  (U7cea % 4 = 2)
 *   U7cfd size=4   -> align to U7d00, size=1   (too small)
 *   U7d02 size=4   -> align to U7d04, size=2   (too small)
 *   U7d46 size=29  -> align to U7d48, size=27
 *   U7d6a size=14  -> align to U7d6c, size=12
 *   U7d7c size=133 -> already aligned, size=133
 *
 * Note: "size" from dump is in uop slots. Triads need 4 slots each.
 * Max triads per region = floor(aligned_size / 4)
 */
typedef struct {
    uint64_t start;     /* 4-aligned uaddr */
    int dump_slots;     /* slots available after alignment */
    int max_triads;     /* floor(dump_slots / 4) */
    int tested_triads;  /* actual usable triads (from test) */
} region_t;

static region_t regions[] = {
    { 0x7c48, 23, 0, 0 },
    { 0x7c6c, 12, 0, 0 },
    { 0x7c98,  6, 0, 0 },
    { 0x7cc8, 23, 0, 0 },
    { 0x7cec, 12, 0, 0 },
    { 0x7d48, 27, 0, 0 },
    { 0x7d6c, 12, 0, 0 },
    { 0x7d7c, 133, 0, 0 },
};
#define NUM_REGIONS (sizeof(regions)/sizeof(regions[0]))

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
 * Install a chain of `len` triads at `start_addr`.
 * Returns expected RAX value.
 */
static uint64_t install_chain(uint64_t start_addr, int len) {
    ucode_t *patch = (ucode_t *)calloc(len, sizeof(ucode_t));
    if (!patch) { perror("calloc"); exit(1); }

    uint64_t expected = 0;

    for (int i = 0; i < len; i++) {
        if (i == 0) {
            patch[i].uop0 = MOVE_DSZ64_DI(TMP0, 1);
            patch[i].uop1 = NOP;
            patch[i].uop2 = NOP;
            patch[i].seqw = NOP_SEQWORD;
            expected = 1;
        } else if (i < len - 1) {
            uint64_t val = (uint64_t)(i + 1);
            patch[i].uop0 = MOVE_DSZ64_DI(TMP1, val);
            patch[i].uop1 = XOR_DSZ64_DRR(TMP0, TMP0, TMP1);
            patch[i].uop2 = NOP;
            patch[i].seqw = NOP_SEQWORD;
            expected ^= val;
        } else {
            patch[i].uop0 = MOVE_DSZ64_DR(RAX, TMP0);
            patch[i].uop1 = NOP;
            patch[i].uop2 = NOP;
            patch[i].seqw = END_SEQWORD;
        }
    }

    patch_ucode(start_addr, patch, len);
    hook_match_and_patch(MP_INDEX, HOOK_ADDR, start_addr);

    free(patch);
    return expected;
}

static int test_region_capacity(uint64_t start_addr, int max_triads) {
    int last_pass = 0;

    for (int len = 2; len <= max_triads; len++) {
        /* Reset match & patch each iteration to avoid stale hooks */
        init_match_and_patch();
        do_fix_IN_patch();

        uint64_t expected = install_chain(start_addr, len);

        got_signal = 0;
        if (setjmp(jmpbuf) != 0) {
            /* Crashed — re-init to recover */
            init_match_and_patch();
            do_fix_IN_patch();
            break;
        }

        uint64_t result = do_vmwrite(0);

        if (result == expected) {
            last_pass = len;
        } else {
            break;
        }
    }

    /* Clean up: reset match & patch */
    init_match_and_patch();
    do_fix_IN_patch();

    return last_pass;
}

int main(void) {
    printf("=== Multi-region patch RAM capacity test ===\n\n");

    assign_to_core(0);

    signal(SIGSEGV, sig_handler);
    signal(SIGILL, sig_handler);
    signal(SIGBUS, sig_handler);

    /* Compute max triads from dump slot count */
    for (int r = 0; r < NUM_REGIONS; r++) {
        regions[r].max_triads = regions[r].dump_slots / 4;
    }

    printf("%-6s  %-8s  %-10s  %-12s  %-12s\n",
           "Idx", "Start", "DumpSlots", "MaxTriads", "Usable");
    printf("------  --------  ----------  ------------  ------------\n");

    int total_usable = 0;

    for (int r = 0; r < NUM_REGIONS; r++) {
        if (regions[r].max_triads < 2) {
            printf("%-6d  U%04" PRIx64 "  %-10d  %-12d  skipped (too small)\n",
                   r, regions[r].start, regions[r].dump_slots, regions[r].max_triads);
            continue;
        }

        int usable = test_region_capacity(regions[r].start, regions[r].max_triads);
        regions[r].tested_triads = usable;
        total_usable += usable;

        printf("%-6d  U%04" PRIx64 "  %-10d  %-12d  %d triads\n",
               r, regions[r].start, regions[r].dump_slots,
               regions[r].max_triads, usable);
    }

    printf("\nTotal usable triads across all regions: %d\n", total_usable);

    /* Plan for 55 triads */
    int needed = 55;
    int fragments = 0;
    int allocated = 0;

    printf("\n=== Placement plan for %d triads ===\n", needed);

    /* Sort regions by usable size descending for greedy allocation */
    region_t sorted[NUM_REGIONS];
    memcpy(sorted, regions, sizeof(regions));
    for (int i = 0; i < NUM_REGIONS - 1; i++) {
        for (int j = i + 1; j < NUM_REGIONS; j++) {
            if (sorted[j].tested_triads > sorted[i].tested_triads) {
                region_t tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }

    for (int r = 0; r < NUM_REGIONS && allocated < needed; r++) {
        int avail = sorted[r].tested_triads;
        if (avail < 2) continue;

        /* Reserve 1 triad for link jump if not the last fragment */
        int remaining = needed - allocated;
        int use;

        if (remaining <= avail) {
            /* Fits entirely — no link triad needed */
            use = remaining;
        } else {
            /* Need link triad at end */
            use = avail - 1;  /* -1 for the GOTO link */
        }

        if (use < 1) continue;

        printf("  Fragment %d: U%04" PRIx64 " — %d triads (%d payload + %s)\n",
               fragments, sorted[r].start, 
               (allocated + use >= needed) ? use : use + 1,
               use,
               (allocated + use >= needed) ? "no link" : "1 link");

        allocated += use;
        fragments++;
    }

    if (allocated >= needed) {
        printf("\n  Plan feasible: %d triads across %d fragments\n", needed, fragments);
    } else {
        printf("\n  NOT ENOUGH SPACE: could only fit %d of %d triads\n", allocated, needed);
    }

    return 0;
}
