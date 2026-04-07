#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/*
 * Path 1 minimal experiment
 *
 * Current (hook_match_and_patch):
 *   1. CPU decodes rdrand
 *   2. Match/patch scanner fires on every triad until it sees U0428
 *   3. Redirects to U7C4C
 *   Cost: ~1M cycles
 *
 * Path 1 (direct ROM shadow):
 *   1. patch_ucode(0x0428, redirect, 1)  -- shadows ROM at rdrand's _xlat addr
 *   2. CPU decodes rdrand, sequencer loads U0428
 *   3. Patch RAM shadow fires immediately, jumps to U7C00
 *   4. Your payload runs
 *   No match/patch scanner. Cost: should be single-digit cycles for the redirect.
 *
 * This test runs both approaches back to back and compares:
 *   - Does the direct patch work at all? (correctness)
 *   - How many cycles does each take? (performance)
 *
 * If patch_ucode cannot write below U7C00, the direct_test() rdrand call will
 * either crash, return garbage, or return the real rdrand value (ROM still runs).
 * Any of those outcomes is useful information.
 */

/* ── Payload: same as yolo() — sets RBX=0xbead, RAX=0xbead ─────────── */
static ucode_t payload[] = {
    {
        MOVE_DSZ64_DI(TMP1, 0xface),
        MOVE_DSZ64_DI(TMP1, 0xbead),   /* TMP1 = 0xbead (second write wins) */
        NOP,
        NOP_SEQWORD
    },
    {
        NOP,
        MOVE_DSZ64_DI(RBX, 0xbead),
        MOVE_DSZ64_DR(RAX, TMP1),
        END_SEQWORD
    }
};

/* ── Redirect triad: unconditional jump from U0428 → U7C00 ──────────── *
 * Sequence word encodes GOTO U7C00. Three NOP uops, control in seqword. *
 * If patch_ucode can shadow U0428, this is all that's needed.           */
static ucode_t redirect[] = {
    {
        NOP,
        NOP,
        NOP,
        SEQ_GOTO0(0x7c00)   /* jump to payload in patch RAM */
    }
};

/* ── Timing helper ──────────────────────────────────────────────────── */
static inline uint64_t rdtsc_start(void) {
    uint32_t lo, hi;
    asm volatile("cpuid\n\trdtsc" : "=a"(lo), "=d"(hi) : "a"(0) : "rbx", "rcx");
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t rdtsc_end(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "rcx");
    return ((uint64_t)hi << 32) | lo;
}

/* ═══════════════════════════════════════════════════════════════════════
 * TEST A: Original hook_match_and_patch approach (your existing yolo)
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_hook(void) {
    printf("=== TEST A: hook_match_and_patch (baseline) ===\n");

    assign_to_core(0);
    do_fix_IN_patch();
    patch_ucode(0x7c4c, payload, ARRAY_SZ(payload));
    hook_match_and_patch(0, 0x0428, 0x7c4c);

    uint64_t rax = 0, rbx = 0;
    asm volatile(
        "rdrand %%rax\n\t"
        "mov %%rax, %[a]\n\t"
        "mov %%rbx, %[b]\n\t"
        : [a] "=r"(rax), [b] "=r"(rbx)
        :
        : "rax", "rbx"
    );
    printf("  RAX = 0x%016" PRIx64 "  (want 0xbead)\n", rax);
    printf("  RBX = 0x%016" PRIx64 "  (want 0xbead)\n", rbx);
    printf("  Correctness: %s\n\n", (rax == 0xbead && rbx == 0xbead) ? "PASS" : "FAIL");

    /* Timing: 1000 iterations */
    uint64_t t0 = rdtsc_start();
    for (int i = 0; i < 1000; i++) {
        asm volatile("rdrand %%rax" : : : "rax");
    }
    uint64_t t1 = rdtsc_end();
    printf("  Cycles/call (hook): %" PRIu64 "\n\n", (t1 - t0) / 1000);
}

/* ═══════════════════════════════════════════════════════════════════════
 * TEST B: Direct ROM shadow — patch_ucode at 0x0428
 *
 * Clears the match/patch slot first, then writes the redirect directly
 * to U0428. If the patch RAM can shadow addresses below U7C00, rdrand
 * will jump to U7C00 without any scanner overhead.
 * ═══════════════════════════════════════════════════════════════════════ */
static void test_direct(void) {
    printf("=== TEST B: Direct ROM shadow at U0428 ===\n");

    assign_to_core(0);
    do_fix_IN_patch();

    /* Payload stays at U7C00 */
    patch_ucode(0x7c00, payload, ARRAY_SZ(payload));

    /* Clear match/patch slot 0 — no scanner active */
    //hook_match_and_patch_clear(0);   /* use whatever clear fn lib-micro provides */
    /* If hook_match_and_patch_clear doesn't exist, zero it manually:*/
     hook_match_and_patch(0, 0xFFFF, 0x7c00);  // unreachable match addr 

    /* Attempt to shadow ROM at U0428 with a redirect to U7C00 */
    printf("  Writing redirect to U0428 via patch_ucode...\n");
    patch_ucode(0x0428, redirect, ARRAY_SZ(redirect));

    uint64_t rax = 0, rbx = 0;
    asm volatile(
        "rdrand %%rax\n\t"
        "mov %%rax, %[a]\n\t"
        "mov %%rbx, %[b]\n\t"
        : [a] "=r"(rax), [b] "=r"(rbx)
        :
        : "rax", "rbx"
    );
    printf("  RAX = 0x%016" PRIx64 "\n", rax);
    printf("  RBX = 0x%016" PRIx64 "\n", rbx);

    if (rax == 0xbead && rbx == 0xbead) {
        printf("  Result: PASS — direct ROM shadow works!\n\n");
    } else if (rax == 0 || rbx == 0) {
        printf("  Result: WRONG VALUE — patch wrote but redirect broken\n");
        printf("          Check SEQWORD_GOTO encoding for U7C00\n\n");
    } else {
        printf("  Result: REAL RDRAND OUTPUT — patch_ucode cannot shadow U0428\n");
        printf("          Patch RAM is restricted to U7C00+. Need different approach.\n\n");
        return;
    }

    /* Timing: compare against test A */
    uint64_t t0 = rdtsc_start();
    for (int i = 0; i < 1000; i++) {
        asm volatile("rdrand %%rax" : : : "rax");
    }
    uint64_t t1 = rdtsc_end();
    printf("  Cycles/call (direct): %" PRIu64 "\n", (t1 - t0) / 1000);
    printf("  (compare to hook cycles above — difference is match/patch overhead)\n\n");
}

int main(void) {
    printf("Path 1 experiment: direct ROM shadow vs hook_match_and_patch\n");
    printf("=============================================================\n\n");

    test_hook();
    //test_direct();

    return 0;
}
