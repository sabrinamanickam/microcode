/*
 * mac128_flagfree.c — MAC128 using arithmetic carry detection
 *
 * ═══════════════════════════════════════════════════════════════════
 *  CONFIRMED GOLDMONT MICROCODE FLAG FINDINGS:
 *
 *  1. ADD_DSZ64_DRR does NOT update any readable flag state
 *  2. GENARITHFLAGS_RR(src0, src1) does NOT publish flags
 *  3. SETCC_CONDB always returns 0  (reads stale CF=0)
 *  4. CMOVCC_CONDB always fires     (reads stale CF=1)
 *  5. SELECTCC_CONDB always fires   (reads stale CF=1)
 *  6. UJMPCC_CONDB never jumps      (reads stale CF=0)
 *  7. READAFLAGS reads stale architectural RFLAGS (0x452302)
 *  8. MOVEINSERTFLGS_DRR(d,a,b) = d←b (MOV from src1)
 *
 *  CONCLUSION: Microcode ALU and conditional ops read from
 *  separate flag domains. No known bridge mechanism works
 *  with the tested GENARITHFLAGS(operandA, operandB) encoding.
 *
 *  SOLUTION: Flag-free carry detection via bit manipulation.
 *
 *  For s = a + b (mod 2^64), carry_out = MSB of:
 *    (a & b) | (~s & (a | b))
 *
 *  This computes the carry chain purely from the sum and operands,
 *  using AND, OR, NOTAND, and SHR. No flags needed.
 * ═══════════════════════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

static void do_patch(ucode_t *patch, int n_triads) {
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, n_triads);
        hook_match_and_patch(0, 0x0428, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  BONUS PROBE: GENARITHFLAGS_RR(result, operand)
 *
 *  Previous probes tested GENARITHFLAGS_RR(operandA, operandB).
 *  Maybe it actually expects (result, one_operand) to back-compute
 *  flags by comparing result against what the addition should give.
 *
 *  If this works, we can simplify MAC128 to 4 triads.
 * ══════════════════════════════════════════════════════════════════ */

void probe_genarithflags_result_operand(void) {
        /* Test: GENARITHFLAGS_RR(sum, operandA) → CMOVCC */
        ucode_t patch_a[] = {
                {
                        ADD_DSZ64_DRR(TMP0, RCX, RDX),    /* TMP0 = sum */
                        MOVE_DSZ64_DI(TMP2, 0),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        GENARITHFLAGS_RR(TMP0, RCX),      /* flags(sum, A) */
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        CMOVCC_DSZ64_CONDB_DRI(TMP2, TMP2, 1),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        MOVE_DSZ64_DR(RAX, TMP0),
                        MOVE_DSZ64_DR(RBX, TMP2),
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch_a, 4);

        uint64_t rax, rbx, r8;
        printf("=== BONUS PROBE A: GENARITHFLAGS_RR(sum, A) → CMOVCC ===\n");

        asm volatile(
                "mov rcx, 0xFFFFFFFFFFFFFFFF\n\t"
                "mov rdx, 3\n\t"
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, rbx\n\t"
                : "=r"(rax), "=r"(rbx) : : "rax","rbx","rcx","rdx","r8"
        );
        printf("  CF=1 case: carry=%" PRIu64 " (want 1)\n", rbx);

        asm volatile(
                "mov rcx, 100\n\t"
                "mov rdx, 50\n\t"
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, rbx\n\t"
                : "=r"(rax), "=r"(rbx) : : "rax","rbx","rcx","rdx","r8"
        );
        printf("  CF=0 case: carry=%" PRIu64 " (want 0)\n", rbx);


        /* Test: GENARITHFLAGS_RR(operandA, sum) → CMOVCC */
        ucode_t patch_b[] = {
                {
                        ADD_DSZ64_DRR(TMP0, RCX, RDX),
                        MOVE_DSZ64_DI(TMP2, 0),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        GENARITHFLAGS_RR(RCX, TMP0),      /* flags(A, sum) */
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        CMOVCC_DSZ64_CONDB_DRI(TMP2, TMP2, 1),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        MOVE_DSZ64_DR(RAX, TMP0),
                        MOVE_DSZ64_DR(RBX, TMP2),
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch_b, 4);

        printf("=== BONUS PROBE B: GENARITHFLAGS_RR(A, sum) → CMOVCC ===\n");

        asm volatile(
                "mov rcx, 0xFFFFFFFFFFFFFFFF\n\t"
                "mov rdx, 3\n\t"
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, rbx\n\t"
                : "=r"(rax), "=r"(rbx) : : "rax","rbx","rcx","rdx","r8"
        );
        printf("  CF=1 case: carry=%" PRIu64 " (want 1)\n", rbx);

        asm volatile(
                "mov rcx, 100\n\t"
                "mov rdx, 50\n\t"
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, rbx\n\t"
                : "=r"(rax), "=r"(rbx) : : "rax","rbx","rcx","rdx","r8"
        );
        printf("  CF=0 case: carry=%" PRIu64 " (want 0)\n\n", rbx);


        /* Also try with SETCC and UJMPCC since they read different flag domains */
        ucode_t patch_c[] = {
                {
                        ADD_DSZ64_DRR(TMP0, RCX, RDX),
                        MOVE_DSZ64_DI(TMP2, 0),
                        MOVE_DSZ64_DI(TMP4, 0),
                        NOP_SEQWORD
                },
                {
                        GENARITHFLAGS_RR(TMP0, RCX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        SETCC_CONDB_DR(TMP2, TMP2),        /* SETCC domain */
                        UJMPCC_DIRECT_NOTTAKEN_CONDB_RI(TMP0, 0x7c04), /* UJMPCC domain */
                        NOP,
                        NOP_SEQWORD
                },
                /* 0x7c03: no-jump path */
                {
                        MOVE_DSZ64_DI(TMP4, 0xBBBB),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* 0x7c04: output */
                {
                        MOVE_DSZ64_DR(RAX, TMP2),
                        MOVE_DSZ64_DR(RBX, TMP4),
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch_c, 5);

        printf("=== BONUS PROBE C: GENARITHFLAGS_RR(sum,A) → SETCC + UJMPCC ===\n");

        asm volatile(
                "mov rcx, 0xFFFFFFFFFFFFFFFF\n\t"
                "mov rdx, 3\n\t"
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, rbx\n\t"
                : "=r"(rax), "=r"(rbx) : : "rax","rbx","rcx","rdx","r8"
        );
        printf("  CF=1: SETCC=%" PRIu64 " (want 1), UJMPCC=%s (want jumped)\n",
               rax, (rbx == 0) ? "jumped" : "fell through");

        asm volatile(
                "mov rcx, 100\n\t"
                "mov rdx, 50\n\t"
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, rbx\n\t"
                : "=r"(rax), "=r"(rbx) : : "rax","rbx","rcx","rdx","r8"
        );
        printf("  CF=0: SETCC=%" PRIu64 " (want 0), UJMPCC=%s (want fell through)\n\n",
               rax, (rbx == 0xBBBB) ? "fell through" : "jumped");
}


/* ══════════════════════════════════════════════════════════════════
 *  FLAG-FREE MAC128 — Arithmetic carry detection
 *
 *  Operation: R8:RAX += RCX × RDX
 *
 *  Carry formula: for s = a + b, carry = SHR((a&b) | (~s & (a|b)), 63)
 *
 *  Layout: 6 triads, 10 active uops
 *
 *  Triad 0: MUL TMP0,RCX,RDX       — TMP0=hi, RDX=lo
 *           MOVE TMP3,RAX           — save old acc_lo
 *
 *  Triad 1: ADD RAX,TMP3,RDX       — acc_lo = old_acc + prod_lo
 *           AND TMP1,TMP3,RDX      — generate bits: a & b
 *           OR  TMP2,TMP3,RDX      — either bits:   a | b
 *                 [all 3 ops read TMP3,RDX from triad 0 — no hazard]
 *
 *  Triad 2: NOTAND TMP4,RAX,TMP2   — ~sum & (a|b) = propagated overflow
 *           ADD R8,R8,TMP0          — acc_hi += prod_hi (independent)
 *
 *  Triad 3: OR TMP5,TMP1,TMP4      — full carry chain bits
 *
 *  Triad 4: SHR TMP5,TMP5,63       — extract MSB = carry out (0 or 1)
 *
 *  Triad 5: ADD R8,R8,TMP5         — acc_hi += carry
 *           END
 * ══════════════════════════════════════════════════════════════════ */

void install_mac128(void) {
        ucode_t mac128_patch[] = {

                /* ── Triad 0: Multiply + save old accumulator ───── */
                {
                        MUL_DSZ64_DRR(TMP0, RCX, RDX),    /* TMP0=hi, RDX=lo  */
                        MOVE_DSZ64_DR(TMP3, RAX),          /* TMP3=old acc_lo  */
                        NOP,
                        NOP_SEQWORD
                },

                /* ── Triad 1: Sum + carry-detect operands ───────── *
                 *  All 3 ops read TMP3 and RDX (from triad 0).     *
                 *  All 3 write different registers. Perfect.        */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RDX),     /* RAX = a + b      */
                        AND_DSZ64_DRR(TMP1, TMP3, RDX),    /* TMP1 = a & b     */
                        OR_DSZ64_DRR(TMP2, TMP3, RDX),     /* TMP2 = a | b     */
                        NOP_SEQWORD
                },

                /* ── Triad 2: Propagated overflow + high product ── *
                 *  NOTAND(d, x, y) = ~x & y                        *
                 *  So NOTAND(TMP4, RAX, TMP2) = ~sum & (a|b)       *
                 *  ADD for acc_hi is independent — parallel.        */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2), /* TMP4 = ~s & (a|b)*/
                        ADD_DSZ64_DRR(R8, R8, TMP0),       /* acc_hi += prod_hi*/
                        NOP,
                        NOP_SEQWORD
                },

                /* ── Triad 3: Merge carry chain ────────────────── */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),    /* full carry bits  */
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },

                /* ── Triad 4: Extract carry bit ────────────────── */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),     /* carry = MSB      */
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },

                /* ── Triad 5: Fold carry into high, done ────────── */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),       /* acc_hi += carry  */
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        do_patch(mac128_patch, 6);
}


/* ══════════════════════════════════════════════════════════════════
 *  TEST SUITE — same tests as before
 * ══════════════════════════════════════════════════════════════════ */

typedef struct {
        const char *name;
        uint64_t acc_lo_in, acc_hi_in;
        uint64_t src1, src2;
        uint64_t exp_lo, exp_hi;
} mac_test_t;

static int run_one_test(const mac_test_t *t) {
        uint64_t rax_out, r8_out;

        asm volatile(
                "mov rax, %[alo]\n\t"
                "mov r8,  %[ahi]\n\t"
                "mov rcx, %[s1]\n\t"
                "mov rdx, %[s2]\n\t"
                "rdrand rax\n\t"
                "mov %[rax], rax\n\t"
                "mov %[r8],  r8\n\t"
                : [rax] "=r"(rax_out), [r8] "=r"(r8_out)
                : [alo] "r"(t->acc_lo_in), [ahi] "r"(t->acc_hi_in),
                  [s1]  "r"(t->src1),      [s2]  "r"(t->src2)
                : "rax", "rcx", "rdx", "r8"
        );

        int pass = (rax_out == t->exp_lo && r8_out == t->exp_hi);
        printf("%s:\n", t->name);
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")%s\n",
               rax_out, t->exp_lo, (rax_out == t->exp_lo) ? "" : " ✗");
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")%s\n",
               r8_out, t->exp_hi, (r8_out == t->exp_hi) ? "" : " ✗");
        printf("  %s\n\n", pass ? "✓ PASS" : "✗ FAIL");
        return pass;
}


int main(void) {
        printf("╔════════════════════════════════════════════════════╗\n");
        printf("║  MAC128 Flag-Free — Arithmetic Carry Detection     ║\n");
        printf("╚════════════════════════════════════════════════════╝\n\n");

        /* ── Run bonus probes first ─────────────────────── */
        probe_genarithflags_result_operand();

        /* ── Install and test MAC128 ────────────────────── */
        printf("═══════════════════════════════════════════\n");
        printf("Installing flag-free MAC128 (6 triads)...\n\n");

        install_mac128();

        mac_test_t tests[] = {
                {
                        .name = "Test 1: acc=100, acc += 7*13 (=191)",
                        .acc_lo_in = 100, .acc_hi_in = 0,
                        .src1 = 7, .src2 = 13,
                        .exp_lo = 191, .exp_hi = 0
                },
                {
                        .name = "Test 2: acc=0, acc += max*max",
                        .acc_lo_in = 0, .acc_hi_in = 0,
                        .src1 = 0xFFFFFFFFFFFFFFFFULL,
                        .src2 = 0xFFFFFFFFFFFFFFFFULL,
                        .exp_lo = 0x0000000000000001ULL,
                        .exp_hi = 0xFFFFFFFFFFFFFFFEULL
                },
                {
                        .name = "Test 3: acc=MAX128, acc += 1*2 (CARRY TEST)",
                        .acc_lo_in = 0xFFFFFFFFFFFFFFFFULL,
                        .acc_hi_in = 0xFFFFFFFFFFFFFFFFULL,
                        .src1 = 1, .src2 = 2,
                        .exp_lo = 0x0000000000000001ULL,
                        .exp_hi = 0x0000000000000000ULL
                },
                {
                        .name = "Test 4: Curve25519 limb product",
                        .acc_lo_in = 0, .acc_hi_in = 0,
                        .src1 = 0x0006000000000000ULL,
                        .src2 = 0x0004000000000000ULL,
                        .exp_lo = 0x0000000000000000ULL,
                        .exp_hi = 0x0000001800000000ULL
                },
                {
                        .name = "Test 5: acc has existing high, small MAC",
                        .acc_lo_in = 0, .acc_hi_in = 0xFF,
                        .src1 = 1, .src2 = 1,
                        .exp_lo = 1, .exp_hi = 0xFF
                },
                {
                        .name = "Test 6: Low overflow carry to hi (CARRY TEST)",
                        .acc_lo_in = 0xFFFFFFFFFFFFFFFFULL,
                        .acc_hi_in = 5,
                        .src1 = 1, .src2 = 3,
                        .exp_lo = 0x0000000000000002ULL,
                        .exp_hi = 6
                },
                {
                        .name = "Test 7: Sequential MAC (pre-accumulated)",
                        .acc_lo_in = 15, .acc_hi_in = 0,
                        .src1 = 7, .src2 = 11,
                        .exp_lo = 92, .exp_hi = 0
                },
                {
                        .name = "Test 8: Probe test vector (0x8..1 * 3)",
                        .acc_lo_in = 0, .acc_hi_in = 0,
                        .src1 = 0x8000000000000001ULL,
                        .src2 = 0x3ULL,
                        .exp_lo = 0x8000000000000003ULL,
                        .exp_hi = 0x0000000000000001ULL
                },
                {
                        .name = "Test 9: Double carry (product + acc both overflow)",
                        .acc_lo_in = 0xFFFFFFFFFFFFFFFFULL,
                        .acc_hi_in = 0,
                        .src1 = 0xFFFFFFFFFFFFFFFFULL,
                        .src2 = 0xFFFFFFFFFFFFFFFFULL,
                        /* max*max = FFFFFFFFFFFFFFFE:0000000000000001
                         * + acc_lo = FFFFFFFFFFFFFFFF
                         * new_lo = 0000000000000001 + FFFFFFFFFFFFFFFF = 0000000000000000 (carry!)
                         * new_hi = FFFFFFFFFFFFFFFE + carry(1) = FFFFFFFFFFFFFFFF */
                        .exp_lo = 0x0000000000000000ULL,
                        .exp_hi = 0xFFFFFFFFFFFFFFFFULL
                },
                {
                        .name = "Test 10: acc_lo = 1, product_lo = max (carry)",
                        .acc_lo_in = 1, .acc_hi_in = 0,
                        .src1 = 0xFFFFFFFFFFFFFFFFULL,
                        .src2 = 1,
                        /* 0xFFFFFFFFFFFFFFFF * 1 = 0:FFFFFFFFFFFFFFFF
                         * + acc_lo = 1
                         * new_lo = FFFFFFFFFFFFFFFF + 1 = 0 (carry!)
                         * new_hi = 0 + 0 + 1 = 1 */
                        .exp_lo = 0x0000000000000000ULL,
                        .exp_hi = 0x0000000000000001ULL
                },
        };

        int n = sizeof(tests) / sizeof(tests[0]);
        int pass_count = 0;
        for (int i = 0; i < n; i++)
                pass_count += run_one_test(&tests[i]);

        printf("════════════════════════════════════════\n");
        printf("  RESULTS: %d / %d passed\n", pass_count, n);
        printf("════════════════════════════════════════\n\n");

        if (pass_count == n) {
                printf("MAC128 is fully operational!\n\n");
                printf("Architecture: 6 triads, 10 active uops\n");
                printf("  Triad 0: MUL + save_acc        (2 uops)\n");
                printf("  Triad 1: ADD + AND + OR         (3 uops — full)\n");
                printf("  Triad 2: NOTAND + ADD_hi        (2 uops)\n");
                printf("  Triad 3: OR (carry chain)       (1 uop)\n");
                printf("  Triad 4: SHR (extract carry)    (1 uop)\n");
                printf("  Triad 5: ADD (fold carry) + END (1 uop)\n\n");
                printf("Flag bypass: carry = SHR((a&b)|(~s&(a|b)), 63)\n\n");
                printf("Next: integrate into curve25519_carry_square\n");
        }

        return 0;
}


/*
 * ═══════════════════════════════════════════════════════════════════
 *  COMPARISON: MAC128 APPROACHES
 *
 *                        │ Triads │ Notes
 *  ══════════════════════╪════════╪══════════════════════════════
 *  WMUL + x86 add/adc   │   2    │ Mix microcode + x86 (works!)
 *  MAC128 flag-free      │   6    │ Pure microcode, no flags
 *  MAC128 if flags worked│   4    │ Hypothetical (flags broken)
 *  x86 mov+mul+add+adc  │  ~6 insn│ Current N3350 code
 *
 *  For a single MAC, WMUL + x86 is clearly better (2 triads + 2 x86).
 *  But for the full carry_square with 15 MACs:
 *    - WMUL approach: 15 × (1 rdrand + 2 mov + 1 add + 1 adc) = 75 insns
 *    - MAC128 approach: 15 × 1 rdrand = 15 insns (6 triads each inside)
 *    - The MAC128 approach eliminates ALL register pressure from the
 *      outer x86 code — the accumulator lives in RAX:R8 across MACs.
 *
 *  The 6-triad MAC128 latency needs benchmarking against WMUL+x86.
 *  If triads execute at 1 cycle each, MAC128 = 6 cycles per MAC.
 *  WMUL (2 triads) + add + adc = ~4 cycles per MAC.
 *  But MAC128 uses fewer architectural registers and zero stack spills.
 *
 * ═══════════════════════════════════════════════════════════════════
 */
