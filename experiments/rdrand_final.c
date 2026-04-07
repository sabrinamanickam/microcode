#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/*
 * Path 1 experiment
 *
 * KEY RULE: All ucode_t arrays must be LOCAL variables.
 * CRC_UOP/CRC_SEQ call parity0/parity1 which are inline functions —
 * runtime expressions. Static/global ucode_t silently zero-fills
 * any field the compiler can't evaluate at compile time, producing
 * corrupt triads.
 *
 * SEQW_GOTO encoding (from seqword.h):
 *   SEQ_GOTO2(addr) = SEQ_UP1(2) | SEQ_UADDR(addr)
 *     UP1=2 means execute uops 0,1,2 then jump to uaddr
 *   SEQ_NOSYNC = SEQ_UP2(3)  (no sync control)
 *   Combined: CRC_SEQ(SEQ_GOTO2(addr) | SEQ_NOSYNC)
 *
 * TEST A: yolo() reproduced exactly — must pass before anything else.
 *
 * TEST B: patch_ucode(0x0428, redirect) — does patch RAM shadow ROM
 *         below 0x7c00?
 *           PASS + lower cycles -> Path 1 works, no LDAT needed
 *           real random         -> patch_ucode restricted to 0x7c00+
 *           wrong value         -> seqword encoding still off
 *
 * TEST B2: ldat_array_read readback of U0428 — regardless of B's
 *          correctness result, this tells us whether the write landed
 *          at all, and gives us the LDAT address mapping for future
 *          ldat_array_write calls.
 */

#define SEQW_GOTO(addr)  CRC_SEQ( SEQ_GOTO2(addr) | SEQ_NOSYNC )

#define PATCH_ADDR  0x7c4c
#define HOOK_ENTRY  0x0428

/* ── Timing ──────────────────────────────────────────────────────── */
static inline u64 tsc_start(void) {
    u32 lo, hi;
    asm volatile("cpuid\n\trdtsc"
        : "=a"(lo), "=d"(hi) : "a"(0) : "rbx", "rcx");
    return ((u64)hi << 32) | lo;
}
static inline u64 tsc_end(void) {
    u32 lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "rcx");
    return ((u64)hi << 32) | lo;
}
static u64 measure(void) {
    u64 t0 = tsc_start();
    for (int i = 0; i < 1000; i++)
        asm volatile("rdrand %%rax" : : : "rax");
    return (tsc_end() - t0) / 1000;
}
static void fire(void) {
    u64 rax = 0, rbx = 0;
    asm volatile(
        "rdrand %%rax\n\t"
        "mov %%rax, %[a]\n\t"
        "mov %%rbx, %[b]\n\t"
        : [a]"=r"(rax), [b]"=r"(rbx) : : "rax", "rbx"
    );
    const char *verdict;
    if (rax == 0xbead && rbx == 0xbead)
        verdict = "PASS";
    else if (rax > 0xfffff)
        verdict = "-> real rdrand (patch not applied)";
    else
        verdict = "-> wrong value (patch applied, encoding issue)";
    printf("  RAX=0x%016" PRIx64 "  RBX=0x%016" PRIx64 "  %s\n",
        rax, rbx, verdict);
}

/* ═══════════════════════════════════════════════════════════════════
 * TEST A: exact yolo() reproduction
 * ═══════════════════════════════════════════════════════════════════ */
static void test_a(void) {
    printf("=== TEST A: hook_match_and_patch (yolo) ===\n");

    ucode_t patch[] = {
        {
            MOVE_DSZ64_DI(TMP1, 0xface),
            MOVE_DSZ64_DI(TMP1, 0xbead),
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

    assign_to_core(0);
    do_fix_IN_patch();
    patch_ucode(PATCH_ADDR, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, HOOK_ENTRY, PATCH_ADDR);

    fire();
    printf("  Cycles/call: %" PRIu64 "\n\n", measure());
}

/* ═══════════════════════════════════════════════════════════════════
 * TEST B: direct ROM shadow via patch_ucode(0x0428)
 * ═══════════════════════════════════════════════════════════════════ */
static void test_b(void) {
    printf("=== TEST B: patch_ucode(0x%04x) direct ROM shadow ===\n",
        HOOK_ENTRY);

    ucode_t payload[] = {
        {
            MOVE_DSZ64_DI(TMP1, 0xface),
            MOVE_DSZ64_DI(TMP1, 0xbead),
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

    /* One-triad redirect: NOP/NOP/NOP then GOTO PATCH_ADDR */
    ucode_t redirect[1];
    redirect[0].uop0 = CRC_UOP(NOP);
    redirect[0].uop1 = CRC_UOP(NOP);
    redirect[0].uop2 = CRC_UOP(NOP);
    redirect[0].seqw = SEQW_GOTO(PATCH_ADDR);

    printf("  Seqword encodings:\n");
    printf("    SEQW_GOTO(0x%04x) = 0x%016" PRIx64 "\n",
        PATCH_ADDR, redirect[0].seqw);
    printf("    NOP_SEQWORD       = 0x%016" PRIx64 "\n", (u64)NOP_SEQWORD);
    printf("    END_SEQWORD       = 0x%016" PRIx64 "\n\n", (u64)END_SEQWORD);

    assign_to_core(0);
    do_fix_IN_patch();
    patch_ucode(PATCH_ADDR, payload, ARRAY_SZ(payload));

    /* Neutralise hook slot 0 — match on unreachable address */
    hook_match_and_patch(0, 0xFFFF, PATCH_ADDR);

    /* Attempt to shadow ROM at U0428 */
    patch_ucode(HOOK_ENTRY, redirect, 1);

    fire();
    printf("  Cycles/call: %" PRIu64 "\n\n", measure());
}

/* ═══════════════════════════════════════════════════════════════════
 * TEST B2: LDAT readback — did the write at 0x0428 land?
 *
 * ldat_array_read(pdat_reg, array_sel, bank_sel, dword_idx, fast_addr)
 * Port 0x6a0 = microcode sequencer (from CustomProcessingUnit docs).
 * Reads back the raw content at U0428 to verify patch_ucode either
 * applied or was silently ignored, and establishes address mapping
 * for future ldat_array_write calls.
 * ═══════════════════════════════════════════════════════════════════ */
static void test_b2(void) {
    printf("=== TEST B2: LDAT readback of U0428 and U0000 ===\n");

    /* Read U0428 — should show our redirect if patch_ucode worked,
     * or original ROM content if it was ignored */
    printf("  U0428 (rdrand entry, should show redirect if B worked):\n");
    for (int slot = 0; slot < 4; slot++) {
        u64 val = ldat_array_read(0x6a0, 0, 0, slot, 0x0428);
        printf("    slot[%d] = 0x%016" PRIx64 "\n", slot, val);
    }

    /* Read U0000 — always original ROM, gives us a reference
     * to verify the port/array/bank/addr mapping is correct.
     * From the uCodeDisasm output:
     *   U0000: tmp15 := MOVEFROMCREG_DSZ64(CORE_CR_CUR_UIP)
     * If we see something non-zero here, LDAT addressing is working. */
    printf("\n  U0000 (ROM reference, should be non-zero):\n");
    for (int slot = 0; slot < 4; slot++) {
        u64 val = ldat_array_read(0x6a0, 0, 0, slot, 0x0000);
        printf("    slot[%d] = 0x%016" PRIx64 "\n", slot, val);
    }

    /* Also read PATCH_ADDR to confirm payload write landed in patch RAM */
    printf("\n  U%04x (our patch RAM payload, should be non-zero):\n",
        (u32)PATCH_ADDR);
    for (int slot = 0; slot < 4; slot++) {
        u64 val = ldat_array_read(0x6a0, 0, 0, slot, PATCH_ADDR);
        printf("    slot[%d] = 0x%016" PRIx64 "\n", slot, val);
    }
    printf("\n");
}

int main(void) {
    printf("Path 1 experiment\n");
    printf("=================\n\n");

    test_a();

    printf("Press enter for Test B (direct ROM shadow), Ctrl+C to abort.\n");
    getchar();

    test_b();

    printf("Press enter for Test B2 (LDAT readback), Ctrl+C to abort.\n");
    getchar();

    test_b2();

    return 0;
}
