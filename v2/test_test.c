/*
 * test_single_limb.c — Compute limb 0 of carry_square entirely in microcode
 *
 * One rdrand hook triggers a 30-triad sequence that:
 *   1. Loads limbs from memory via LDZX
 *   2. Computes 3 multiply-accumulate operations
 *   3. Returns 128-bit result in R8:RAX
 *
 * limb0 = arg1[0]² + arg1[1]*(arg1[4]*38) + arg1[2]*(arg1[3]*38)
 *
 * BUILD:  make PROG=test_single_limb
 * RUN:    sudo ./test_single_limb_static
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

/* Append 6 MAC128 triads: acc(R8:RAX) += RCX * RDX */
static void emit_mac128(ucode_t *patch, int *idx) {
        int i = *idx;

        patch[i++] = (ucode_t){
                MOVE_DSZ64_DR(TMP3, RAX),
                MUL_DSZ64_DRR(TMP0, RCX, RDX),
                NOP, NOP_SEQWORD
        };
        patch[i++] = (ucode_t){
                ADD_DSZ64_DRR(RAX, TMP3, RDX),
                AND_DSZ64_DRR(TMP1, TMP3, RDX),
                OR_DSZ64_DRR(TMP2, TMP3, RDX),
                NOP_SEQWORD
        };
        patch[i++] = (ucode_t){
                NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                ADD_DSZ64_DRR(R8, R8, TMP0),
                NOP, NOP_SEQWORD
        };
        patch[i++] = (ucode_t){
                OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                NOP, NOP, NOP_SEQWORD
        };
        patch[i++] = (ucode_t){
                SHR_DSZ64_DRI(TMP5, TMP5, 63),
                NOP, NOP, NOP_SEQWORD
        };
        patch[i++] = (ucode_t){
                ADD_DSZ64_DRR(R8, R8, TMP5),
                NOP, NOP, NOP_SEQWORD
        };

        *idx = i;
}

static void test_single_limb(void) {
        printf("=== Single-limb carry_square (limb 0) ===\n");
        printf("SEG = 0x%02x\n\n", SEG);

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

        /* T9: Load a[4]→RCX, constant 19→RBP */
        patch[idx++] = (ucode_t){
                LDZX_DSZ64_ASZ32_SC1_DRI(RCX, RSI, 0x20, SEG),
                MOVE_DSZ64_DI(RBP, 19),
                NOP, NOP_SEQWORD
        };

        /* T10: RDX = 19 */
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

        /* Overwrite last triad's seqword with END */
        patch[idx - 1].seqw = END_SEQWORD;

        printf("Patch size: %d triads\n\n", idx);

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, idx);
        hook_match_and_patch(0, 0x0428, 0x7c00);

        /* Test vectors */
        struct {
                uint64_t input[5];
                const char *name;
        } tests[] = {
                { {1, 0, 0, 0, 0}, "1^2" },
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
                uint64_t *a = tests[t].input;
                uint128_t ref = (uint128_t)a[0] * a[0]
                              + (uint128_t)a[1] * (a[4] * 38)
                              + (uint128_t)a[2] * (a[3] * 38);
                uint64_t ref_lo = (uint64_t)ref;
                uint64_t ref_hi = (uint64_t)(ref >> 64);

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
                printf("  %-10s: %s", tests[t].name, ok ? "PASS" : "FAIL");
                if (!ok) {
                        printf("\n    ref: %016" PRIx64 ":%016" PRIx64
                               "\n    got: %016" PRIx64 ":%016" PRIx64,
                               ref_hi, ref_lo, r8_out, rax_out);
                }
                printf("\n");
                pass += ok;
                munmap(buf, 4096);
        }
        printf("\n%d / %d passed\n", pass, n_tests);
}

int main(void) {
        test_single_limb();
        return 0;
}
