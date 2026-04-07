/*
 * test_single_limb.c
 *
 * Part 1: Probe how many triads fit in patch RAM
 * Part 2: Compute limb 0 of carry_square entirely in microcode
 *         (LDZX loads, MUL, accumulate, STAD store)
 *
 * This validates the single-entry approach before we build
 * the full 136-triad carry_square.
 *
 * BUILD:  make PROG=test_single_limb
 * RUN:    sudo ./test_single_limb_static
 *
 * PREREQUISITE: test_memops must pass first (LDZX/STAD working).
 *               Set WORKING_SEG below to whatever passed.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <sys/mman.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* Set this to whatever segment value worked in test_memops */
#ifndef SEG
#define SEG 3
#endif

typedef unsigned __int128 uint128_t;

static void *alloc32(size_t n) {
        void *p = mmap(NULL, n, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (p == MAP_FAILED) { perror("mmap"); return NULL; }
        return p;
}

/* ══════════════════════════════════════════════════════════════════
 * PART 1: Probe max patch RAM size
 *
 * Fill patch RAM with NOP triads + END_SEQWORD at the end.
 * Increase count until it crashes or hook fails.
 * ══════════════════════════════════════════════════════════════════ */
static void probe_patch_ram_size(void) {
        printf("=== PART 1: Patch RAM size probe ===\n");

        int sizes[] = { 10, 50, 100, 150, 200, 250, 300, 400, 500 };
        int n = sizeof(sizes) / sizeof(sizes[0]);

        for (int t = 0; t < n; t++) {
                int count = sizes[t];

                /* Build a patch: (count-1) NOP triads + 1 final triad with END */
                ucode_t *patch = calloc(count, sizeof(ucode_t));
                if (!patch) { printf("  alloc failed at %d\n", count); break; }

                for (int i = 0; i < count - 1; i++) {
                        patch[i] = (ucode_t){
                                MOVE_DSZ64_DR(RAX, RAX),  /* harmless NOP-like */
                                NOP, NOP, NOP_SEQWORD
                        };
                }
                /* Final triad: set RAX to a marker value + END */
                patch[count - 1] = (ucode_t){
                        MOVE_DSZ64_DI(RAX, count & 0xFF),
                        NOP, NOP, END_SEQWORD
                };

                assign_to_core(0);
                do_fix_IN_patch();
                patch_ucode(0x7c00, patch, count);
                hook_match_and_patch(0, 0x0428, 0x7c00);

                uint64_t rax_out;
                asm volatile(
                        "rdrand rax\n\t"
                        "mov %[out], rax\n\t"
                        : [out] "=r"(rax_out)
                        :
                        : "rax", "rcx", "rdx"
                );

                int ok = ((rax_out & 0xFF) == (uint64_t)(count & 0xFF));
                printf("  %3d triads: RAX=0x%02" PRIx64 " %s\n",
                       count, rax_out & 0xFF, ok ? "✓" : "✗");
                free(patch);

                if (!ok) {
                        printf("  → Max patch size is between %d and %d triads\n",
                               (t > 0 ? sizes[t-1] : 1), count);
                        break;
                }
        }
        printf("\n");
}


/* ══════════════════════════════════════════════════════════════════
 * PART 2: Single-limb proof of concept
 *
 * Compute limb 0 of carry_square entirely in microcode:
 *   result = arg1[0]² + arg1[1]*(arg1[4]*38) + arg1[2]*(arg1[3]*38)
 *
 * Input:  RSI = pointer to uint64_t[5] (in low 4GB)
 * Output: RAX = result_lo, R8 = result_hi
 *
 * Register plan inside microcode:
 *   RSI = input pointer (preserved, used as LDZX base)
 *   RAX = acc_lo
 *   R8  = acc_hi
 *   RCX, RDX = MUL operands (scratch)
 *   RBP = constant 19
 *   TMP0-TMP5 = carry detection scratch
 * ══════════════════════════════════════════════════════════════════ */

/* Helper: MAC128 triads (acc += RCX * RDX)
 * Appends 6 triads to patch[] starting at *idx.
 * Assumes: RAX=acc_lo, R8=acc_hi, RCX/RDX=operands
 * After: RAX=new_acc_lo, R8=new_acc_hi, RDX=clobbered
 */
static void emit_mac128(ucode_t *patch, int *idx) {
        int i = *idx;

        /* T+0: Save acc_lo + multiply */
        patch[i++] = (ucode_t){
                MOVE_DSZ64_DR(TMP3, RAX),
                MUL_DSZ64_DRR(TMP0, RCX, RDX),
                NOP, NOP_SEQWORD
        };
        /* T+1: new_acc_lo = old + prod_lo; carry operands
         * prod_lo is in RDX (confirmed: dst=TMP0=hi, src1=RDX=lo) */
        patch[i++] = (ucode_t){
                ADD_DSZ64_DRR(RAX, TMP3, RDX),
                AND_DSZ64_DRR(TMP1, TMP3, RDX),
                OR_DSZ64_DRR(TMP2, TMP3, RDX),
                NOP_SEQWORD
        };
        /* T+2: propagated overflow + acc_hi += prod_hi (TMP0) */
        patch[i++] = (ucode_t){
                NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD
        };
        /* T+3: merge carry */
        patch[i++] = (ucode_t){
                OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                NOP, NOP, NOP_SEQWORD
        };
        /* T+4: extract carry bit */
        patch[i++] = (ucode_t){
                SHR_DSZ64_DRI(TMP5, TMP5, 63),
                NOP, NOP, NOP_SEQWORD
        };
        /* T+5: fold carry into acc_hi */
        patch[i++] = (ucode_t){
                ADD_DSZ64_DRR(R8, R8, TMP5),
                NOP, NOP, NOP_SEQWORD
        };

        *idx = i;
}

static void test_single_limb(void) {
        printf("=== PART 2: Single-limb carry_square (limb 0) ===\n");

        /*
         * Limb 0 = arg1[0]² + arg1[1]*x2 + arg1[2]*x5
         * where x2 = arg1[4]*38, x5 = arg1[3]*38
         *
         * Microcode sequence:
         *
         * PHASE A: Load limbs and init
         *   T0: LDZX a[0]→RCX                    (will self-square)
         *   T1: MOVE RAX=0, MOVE R8=0             (zero accumulator)
         *
         * PHASE B: MAC #1 — a[0]²
         *   T2: MOVE RDX=RCX                      (RCX=a0, RDX=a0)
         *   T3-T8: MAC128(acc += RCX*RDX)
         *
         * PHASE C: Compute a[4]*38, then MAC #2 — a[1]*(a[4]*38)
         *   T9:  LDZX a[4]→RCX, MOVE RBP=19
         *   T10: MOVE RDX=RBP                     (RDX=19)
         *   T11: MUL(TMP0, RCX, RDX)              (RDX=a4*19)
         *   T12: ADD(RDX, RDX, RDX)               (RDX=a4*38)
         *        LDZX a[1]→RCX                    (RCX=a1)
         *                                         (check: LDZX writes RCX, ADD writes RDX — no conflict)
         *   T13-T18: MAC128(acc += RCX*RDX)
         *
         * PHASE D: Compute a[3]*38, then MAC #3 — a[2]*(a[3]*38)
         *   T19: LDZX a[3]→RCX
         *   T20: MOVE RDX=RBP                     (RDX=19, RBP still intact)
         *   T21: MUL(TMP0, RCX, RDX)              (RDX=a3*19)
         *   T22: ADD(RDX, RDX, RDX)               (RDX=a3*38)
         *        LDZX a[2]→RCX                    (RCX=a2)
         *   T23-T28: MAC128(acc += RCX*RDX)
         *
         * PHASE E: Done
         *   T29: END
         *
         * Total: 30 triads
         */

        ucode_t patch[32];
        int idx = 0;

        /* T0: Load a[0] → RCX */
        patch[idx++] = (ucode_t){
                LDZX_DSZ64_ASZ32_SC1_DR(RCX, RSI, SEG),
                NOP, NOP, NOP_SEQWORD
        };

        /* T1: Zero accumulator */
        patch[idx++] = (ucode_t){
                MOVE_DSZ64_DI(RAX, 0),
                MOVE_DSZ64_DI(R8, 0),
                NOP, NOP_SEQWORD
        };

        /* T2: Setup self-square: RDX = RCX = a[0] */
        patch[idx++] = (ucode_t){
                MOVE_DSZ64_DR(RDX, RCX),
                NOP, NOP, NOP_SEQWORD
        };

        /* T3-T8: MAC128 #1: acc += a[0]² */
        emit_mac128(patch, &idx);

        /* T9: Load a[4]→RCX, load constant 19→RBP */
        patch[idx++] = (ucode_t){
                LDZX_DSZ64_ASZ32_SC1_DRI(RCX, RSI, 0x20, SEG),
                MOVE_DSZ64_DI(RBP, 19),
                NOP, NOP_SEQWORD
        };

        /* T10: RDX = 19 (for MUL) */
        patch[idx++] = (ucode_t){
                MOVE_DSZ64_DR(RDX, RBP),
                NOP, NOP, NOP_SEQWORD
        };

        /* T11: MUL → TMP0=hi, RDX=a4*19 */
        patch[idx++] = (ucode_t){
                MUL_DSZ64_DRR(TMP0, RCX, RDX),
                NOP, NOP, NOP_SEQWORD
        };

        /* T12: RDX = a4*38; load a[1]→RCX */
        patch[idx++] = (ucode_t){
                ADD_DSZ64_DRR(RDX, RDX, RDX),
                LDZX_DSZ64_ASZ32_SC1_DRI(RCX, RSI, 0x08, SEG),
                NOP, NOP_SEQWORD
        };

        /* T13-T18: MAC128 #2: acc += a[1] * (a[4]*38) */
        emit_mac128(patch, &idx);

        /* T19: Load a[3]→RCX */
        patch[idx++] = (ucode_t){
                LDZX_DSZ64_ASZ32_SC1_DRI(RCX, RSI, 0x18, SEG),
                NOP, NOP, NOP_SEQWORD
        };

        /* T20: RDX = 19 */
        patch[idx++] = (ucode_t){
                MOVE_DSZ64_DR(RDX, RBP),
                NOP, NOP, NOP_SEQWORD
        };

        /* T21: MUL → RDX = a3*19 */
        patch[idx++] = (ucode_t){
                MUL_DSZ64_DRR(TMP0, RCX, RDX),
                NOP, NOP, NOP_SEQWORD
        };

        /* T22: RDX = a3*38; load a[2]→RCX */
        patch[idx++] = (ucode_t){
                ADD_DSZ64_DRR(RDX, RDX, RDX),
                LDZX_DSZ64_ASZ32_SC1_DRI(RCX, RSI, 0x10, SEG),
                NOP, NOP_SEQWORD
        };

        /* T23-T28: MAC128 #3: acc += a[2] * (a[3]*38) */
        emit_mac128(patch, &idx);

        /* T29: END — result is in R8:RAX */
        patch[idx - 1] = (ucode_t){   /* overwrite last NOP_SEQWORD with END */
                patch[idx - 1].uop0,
                patch[idx - 1].uop1,
                patch[idx - 1].uop2,
                END_SEQWORD
        };

        printf("  Patch size: %d triads\n", idx);

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, idx);
        hook_match_and_patch(0, 0x0428, 0x7c00);

        /* Test vectors */
        struct {
                uint64_t input[5];
                const char *name;
        } tests[] = {
                { {1, 0, 0, 0, 0}, "1²" },
                { {0, 0, 0, 0, 0}, "zero" },
                { {7, 13, 17, 23, 29}, "small" },
                { {123456789, 987654321, 111111111, 222222222, 333333333}, "medium" },
                { {0x0007FFFFFFFFFFFFULL, 0x0007FFFFFFFFFFFFULL,
                   0x0007FFFFFFFFFFFFULL, 0x0007FFFFFFFFFFFFULL,
                   0x0007FFFFFFFFFFFFULL}, "near-max" },
        };
        int n_tests = sizeof(tests) / sizeof(tests[0]);

        int pass = 0;
        for (int t = 0; t < n_tests; t++) {
                /* Reference: compute limb 0 in C */
                uint64_t *a = tests[t].input;
                uint128_t ref = (uint128_t)a[0] * a[0]
                              + (uint128_t)a[1] * (a[4] * 38)
                              + (uint128_t)a[2] * (a[3] * 38);
                uint64_t ref_lo = (uint64_t)ref;
                uint64_t ref_hi = (uint64_t)(ref >> 64);

                /* Microcode: pass input array pointer in RSI */
                uint64_t *buf = alloc32(4096);
                if (!buf) return;
                memcpy(buf, a, 40);

                uint64_t rax_out, r8_out;
                asm volatile(
                        "mov rsi, %[ptr]\n\t"
                        "rdrand rax\n\t"
                        "mov %[lo], rax\n\t"
                        "mov %[hi], r8\n\t"
                        : [lo] "=r"(rax_out), [hi] "=r"(r8_out)
                        : [ptr] "r"(buf)
                        : "rax", "rcx", "rdx", "rsi", "rbp", "r8"
                );

                int ok = (rax_out == ref_lo && r8_out == ref_hi);
                printf("  %s: %s", tests[t].name, ok ? "PASS" : "FAIL");
                if (!ok) {
                        printf("\n    ref: %016" PRIx64 ":%016" PRIx64
                               "\n    got: %016" PRIx64 ":%016" PRIx64,
                               ref_hi, ref_lo, r8_out, rax_out);
                }
                printf("\n");
                pass += ok;
                munmap(buf, 4096);
        }
        printf("\n  %d / %d passed\n\n", pass, n_tests);
}


int main(void) {
        printf("╔══════════════════════════════════════════════╗\n");
        printf("║  Single-Entry carry_square Feasibility Test   ║\n");
        printf("╚══════════════════════════════════════════════╝\n\n");
        printf("SEG = 0x%02x  (change with -DSEG=X)\n\n", SEG);

        probe_patch_ram_size();
        test_single_limb();

        return 0;
}
