/*
 * test_raw_war_waw.c — Concrete RAW / WAR / WAW hazard tests for both
 *                     architectural and TMP registers, intra-triad.
 *
 * For each ordered pair (earlier=A, later=B) of slots in {(0,1),(0,2),(1,2)}:
 *   RAW: slot A writes  X, slot B reads  X — expect new value to be visible
 *   WAR: slot A reads   X, slot B writes X — expect reader to see OLD value
 *   WAW: slot A writes  X, slot B writes X — expect later value to win
 *
 * X ∈ { arch reg R13, temp reg TMP0 }  →  3 × 3 × 2 = 18 sub-tests.
 *
 * Witness convention (set up by inline-asm prologue before vmwrite):
 *   R13      target architectural register (initialised to V_OLD)
 *   R14      source carrying V_NEW1 (the "earlier writer" value)
 *   R15      source carrying V_NEW2 (the "later writer" value, WAW only)
 *   R12      primary witness — pre-zeroed; holds what the reader-slot saw
 *   R11      secondary witness — pre-zeroed; holds final TMP0 (TMP variants)
 *
 *   V_OLD  = 0xAAAAAAAAAAAAAAA1  initial value of R13 / TMP0
 *   V_NEW1 = 0xBBBBBBBBBBBBBBB2  value the "earlier" slot writes
 *   V_NEW2 = 0xCCCCCCCCCCCCCCC3  value the "later"   slot writes (WAW only)
 *
 * Build:  make PROG=test_raw_war_waw
 * Run:    sudo taskset -c 0 ./test_raw_war_waw_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define V_OLD   UINT64_C(0xAAAAAAAAAAAAAAA1)
#define V_NEW1  UINT64_C(0xBBBBBBBBBBBBBBB2)
#define V_NEW2  UINT64_C(0xCCCCCCCCCCCCCCC3)

typedef struct { uint64_t r13, r12, r11; } result_t;

/* Fire the currently-installed patch with a controlled register state.
 * Returns R13, R12, R11 as observed after vmwrite. */
static result_t run_vmwrite(uint64_t v_old, uint64_t v_new1, uint64_t v_new2) {
    uint64_t r13_out, r12_out, r11_out;
    uint64_t in[3] = { v_old, v_new1, v_new2 };
    asm volatile(
        "mov r13, [%3 + 0]\n\t"
        "mov r14, [%3 + 8]\n\t"
        "mov r15, [%3 + 16]\n\t"
        "xor r12d, r12d\n\t"
        "xor r11d, r11d\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %0, r13\n\t"
        "mov %1, r12\n\t"
        "mov %2, r11\n\t"
        : "=&r"(r13_out), "=&r"(r12_out), "=&r"(r11_out)
        : "r"(&in[0])
        : "r11", "r12", "r13", "r14", "r15",
          "rcx", "rdx", "memory", "cc"
    );
    return (result_t){ r13_out, r12_out, r11_out };
}

static void install(ucode_t *p, int n) {
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, p, n);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}
#define INSTALL(p) install((p), ARRAY_SZ(p))

static int report(const char *name,
                  uint64_t got_w, uint64_t want_w, const char *w_meaning,
                  int has_f, uint64_t got_f, uint64_t want_f, const char *f_meaning)
{
    int w_ok = (got_w == want_w);
    int f_ok = !has_f || (got_f == want_f);
    int ok   = w_ok && f_ok;
    printf("  %-22s %s\n", name, ok ? "PASS" : "FAIL");
    if (!w_ok)
        printf("    %-18s got 0x%016" PRIx64 "  want 0x%016" PRIx64 "\n",
               w_meaning, got_w, want_w);
    if (has_f && !f_ok)
        printf("    %-18s got 0x%016" PRIx64 "  want 0x%016" PRIx64 "\n",
               f_meaning, got_f, want_f);
    return ok;
}

int main(void) {
    assign_to_core(0);
    int total = 0, pass = 0;

    printf("=== Intra-triad RAW / WAR / WAW Test ===\n");
    printf("  V_OLD  = 0x%016" PRIx64 "  (initial R13 / TMP0)\n", V_OLD);
    printf("  V_NEW1 = 0x%016" PRIx64 "  (earlier-slot writer source = R14)\n", V_NEW1);
    printf("  V_NEW2 = 0x%016" PRIx64 "  (later-slot   writer source = R15)\n\n", V_NEW2);

    /* ========================================================
     * RAW — earlier slot writes, later slot reads.
     *   Sequential semantics ⇒ reader sees the new value.
     * ======================================================== */
    printf("--- RAW (earlier writes, later reads) ---\n");

    /* RAW 0->1 arch */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(R13, R14),    /* s0: R13 ← V_NEW1   */
              ZEROEXT_DSZ64_DR(R12, R13),    /* s1: R12 ← R13      */
              NOP, END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("RAW 0->1 arch (R13)",
                                r.r12, V_NEW1, "R12 (s1 read)",
                                1, r.r13, V_NEW1, "R13 (final)");
    }

    /* RAW 0->1 tmp */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(TMP0, R13), NOP, NOP, NOP_SEQWORD },   /* TMP0 ← V_OLD */
            { ZEROEXT_DSZ64_DR(TMP0, R14),   /* s0: TMP0 ← V_NEW1 */
              ZEROEXT_DSZ64_DR(R12,  TMP0),  /* s1: R12  ← TMP0   */
              NOP, END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("RAW 0->1 tmp  (TMP0)",
                                r.r12, V_NEW1, "R12 (s1 read)",
                                0, 0, 0, NULL);
    }

    /* RAW 0->2 arch */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(R13, R14),    /* s0: R13 ← V_NEW1 */
              NOP,                            /* s1: NOP          */
              ZEROEXT_DSZ64_DR(R12, R13),    /* s2: R12 ← R13    */
              END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("RAW 0->2 arch (R13)",
                                r.r12, V_NEW1, "R12 (s2 read)",
                                1, r.r13, V_NEW1, "R13 (final)");
    }

    /* RAW 0->2 tmp */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(TMP0, R13), NOP, NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(TMP0, R14),
              NOP,
              ZEROEXT_DSZ64_DR(R12, TMP0),
              END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("RAW 0->2 tmp  (TMP0)",
                                r.r12, V_NEW1, "R12 (s2 read)",
                                0, 0, 0, NULL);
    }

    /* RAW 1->2 arch */
    {
        ucode_t p[] = {
            { NOP,
              ZEROEXT_DSZ64_DR(R13, R14),
              ZEROEXT_DSZ64_DR(R12, R13),
              END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("RAW 1->2 arch (R13)",
                                r.r12, V_NEW1, "R12 (s2 read)",
                                1, r.r13, V_NEW1, "R13 (final)");
    }

    /* RAW 1->2 tmp */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(TMP0, R13), NOP, NOP, NOP_SEQWORD },
            { NOP,
              ZEROEXT_DSZ64_DR(TMP0, R14),
              ZEROEXT_DSZ64_DR(R12, TMP0),
              END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("RAW 1->2 tmp  (TMP0)",
                                r.r12, V_NEW1, "R12 (s2 read)",
                                0, 0, 0, NULL);
    }

    /* ========================================================
     * WAR — earlier slot reads, later slot writes.
     *   Sequential semantics ⇒ reader sees OLD value;
     *                           writer's value persists as final.
     *   If WAR is broken (writer leaks into reader),
     *   the witness would equal V_NEW1 instead of V_OLD.
     * ======================================================== */
    printf("\n--- WAR (earlier reads, later writes) ---\n");

    /* WAR 0->1 arch */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(R12, R13),    /* s0: R12 ← R13(OLD) */
              ZEROEXT_DSZ64_DR(R13, R14),    /* s1: R13 ← V_NEW1   */
              NOP, END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("WAR 0->1 arch (R13)",
                                r.r12, V_OLD,  "R12 (s0 read)",
                                1, r.r13, V_NEW1, "R13 (final)");
    }

    /* WAR 0->1 tmp */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(TMP0, R13), NOP, NOP, NOP_SEQWORD }, /* TMP0 ← V_OLD */
            { ZEROEXT_DSZ64_DR(R12,  TMP0),  /* s0: R12  ← TMP0(OLD) */
              ZEROEXT_DSZ64_DR(TMP0, R14),   /* s1: TMP0 ← V_NEW1    */
              NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R11, TMP0), NOP, NOP, END_SEQWORD }  /* observe final */
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("WAR 0->1 tmp  (TMP0)",
                                r.r12, V_OLD,  "R12 (s0 read)",
                                1, r.r11, V_NEW1, "R11 (final TMP0)");
    }

    /* WAR 0->2 arch */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(R12, R13),    /* s0: R12 ← R13(OLD) */
              NOP,
              ZEROEXT_DSZ64_DR(R13, R14),    /* s2: R13 ← V_NEW1   */
              END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("WAR 0->2 arch (R13)",
                                r.r12, V_OLD,  "R12 (s0 read)",
                                1, r.r13, V_NEW1, "R13 (final)");
    }

    /* WAR 0->2 tmp */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(TMP0, R13), NOP, NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R12,  TMP0),
              NOP,
              ZEROEXT_DSZ64_DR(TMP0, R14),
              NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R11, TMP0), NOP, NOP, END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("WAR 0->2 tmp  (TMP0)",
                                r.r12, V_OLD,  "R12 (s0 read)",
                                1, r.r11, V_NEW1, "R11 (final TMP0)");
    }

    /* WAR 1->2 arch */
    {
        ucode_t p[] = {
            { NOP,
              ZEROEXT_DSZ64_DR(R12, R13),    /* s1: R12 ← R13(OLD) */
              ZEROEXT_DSZ64_DR(R13, R14),    /* s2: R13 ← V_NEW1   */
              END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("WAR 1->2 arch (R13)",
                                r.r12, V_OLD,  "R12 (s1 read)",
                                1, r.r13, V_NEW1, "R13 (final)");
    }

    /* WAR 1->2 tmp */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(TMP0, R13), NOP, NOP, NOP_SEQWORD },
            { NOP,
              ZEROEXT_DSZ64_DR(R12,  TMP0),
              ZEROEXT_DSZ64_DR(TMP0, R14),
              NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R11, TMP0), NOP, NOP, END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("WAR 1->2 tmp  (TMP0)",
                                r.r12, V_OLD,  "R12 (s1 read)",
                                1, r.r11, V_NEW1, "R11 (final TMP0)");
    }

    /* ========================================================
     * WAW — two writes to the same register; expect later to win.
     *   R14 carries V_NEW1 (earlier writer),
     *   R15 carries V_NEW2 (later   writer).
     * ======================================================== */
    printf("\n--- WAW (two writes; later wins) ---\n");

    /* WAW 0+1 arch */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(R13, R14),    /* s0: R13 ← V_NEW1 */
              ZEROEXT_DSZ64_DR(R13, R15),    /* s1: R13 ← V_NEW2 */
              NOP, END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("WAW 0+1 arch (R13)",
                                r.r13, V_NEW2, "R13 (final)",
                                0, 0, 0, NULL);
    }

    /* WAW 0+1 tmp */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(TMP0, R14),
              ZEROEXT_DSZ64_DR(TMP0, R15),
              NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R11, TMP0), NOP, NOP, END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("WAW 0+1 tmp  (TMP0)",
                                r.r11, V_NEW2, "R11 (final TMP0)",
                                0, 0, 0, NULL);
    }

    /* WAW 0+2 arch */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(R13, R14),
              NOP,
              ZEROEXT_DSZ64_DR(R13, R15),
              END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("WAW 0+2 arch (R13)",
                                r.r13, V_NEW2, "R13 (final)",
                                0, 0, 0, NULL);
    }

    /* WAW 0+2 tmp */
    {
        ucode_t p[] = {
            { ZEROEXT_DSZ64_DR(TMP0, R14),
              NOP,
              ZEROEXT_DSZ64_DR(TMP0, R15),
              NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R11, TMP0), NOP, NOP, END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("WAW 0+2 tmp  (TMP0)",
                                r.r11, V_NEW2, "R11 (final TMP0)",
                                0, 0, 0, NULL);
    }

    /* WAW 1+2 arch */
    {
        ucode_t p[] = {
            { NOP,
              ZEROEXT_DSZ64_DR(R13, R14),
              ZEROEXT_DSZ64_DR(R13, R15),
              END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("WAW 1+2 arch (R13)",
                                r.r13, V_NEW2, "R13 (final)",
                                0, 0, 0, NULL);
    }

    /* WAW 1+2 tmp */
    {
        ucode_t p[] = {
            { NOP,
              ZEROEXT_DSZ64_DR(TMP0, R14),
              ZEROEXT_DSZ64_DR(TMP0, R15),
              NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R11, TMP0), NOP, NOP, END_SEQWORD }
        };
        INSTALL(p);
        result_t r = run_vmwrite(V_OLD, V_NEW1, V_NEW2);
        total++; pass += report("WAW 1+2 tmp  (TMP0)",
                                r.r11, V_NEW2, "R11 (final TMP0)",
                                0, 0, 0, NULL);
    }

    /* ================================================================ */
    printf("\n=== %d/%d passed ===\n", pass, total);

    init_match_and_patch();
    do_fix_IN_patch();
    return (pass == total) ? 0 : 1;
}
