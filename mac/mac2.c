/*
 * mac128_2mac.c — 2-MAC patch: grow from working 6-triad single MAC
 *
 * Patch layout (13 triads):
 *   Triads 0–5:  MAC1 using RCX × RDX  (identical to mac128_nomovs)
 *   Triad  6:    Setup — move R9→RCX, R10→RDX
 *   Triads 7–12: MAC2 using (new) RCX × RDX
 *
 * Hook: vmwrite rcx, rdx  (0x0cd8 → 0x7c00)
 *
 * INPUT:  RCX=op1a, RDX=op1b, R9=op2a, R10=op2b, RAX=acc_lo, R8=acc_hi
 * OUTPUT: RAX=acc_lo, R8=acc_hi
 * CLOBBERS: RCX, RDX
 *
 * BUILD:  make PROG=mac128_2mac
 * RUN:    sudo taskset -c 0 ./mac128_2mac_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

void install_2mac(void) {
        ucode_t patch[] = {
                /* ── MAC 1: RCX × RDX (triads 0–5, same as mac128_nomovs) ── */

                /* Triad 0: save acc_lo, multiply */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: sum + carry-detect */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* Triad 2: ~sum & (a|b), acc_hi += prod_hi */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 3: merge carry */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 4: extract carry bit */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 5: fold carry (NOT end — continue to MAC2) */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },

                /* ── Setup: move R9→RCX, R10→RDX (triad 6) ── */
                {
                        ZEROEXT_DSZ64_DR(RCX, R9),
                        ZEROEXT_DSZ64_DR(RDX, R10),
                        NOP,
                        NOP_SEQWORD
                },

                /* ── MAC 2: (new) RCX × RDX (triads 7–12) ── */

                /* Triad 7: save acc_lo, multiply */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 8 */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* Triad 9 */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 10 */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 11 */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 12: fold carry + END */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        printf("Installing 2-MAC patch (13 triads)...\n");
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
        printf("  Done.\n\n");
}

int main(void) {
        uint64_t rax_out, r8_out;

        install_2mac();

        /* ── Test 1: Single MAC still works (R9/R10 = 0 effectively) ── */
        /* acc=0, RCX=7, RDX=13, R9=0, R10=0 → should get 91 */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 7\n\t"
                "mov rdx, 13\n\t"
                "xor r9, r9\n\t"
                "xor r10, r10\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8", "r9", "r10"
        );
        printf("Test 1: acc=0, MAC1=7*13, MAC2=0*0\n");
        printf("  RAX = 0x%016" PRIx64 "  (expect 0x5b = 91)\n", rax_out);
        printf("  R8  = 0x%016" PRIx64 "  (expect 0x0)\n", r8_out);
        printf("  %s\n\n", (rax_out == 91 && r8_out == 0) ? "PASS" : "FAIL");

        /* ── Test 2: Both MACs active ── */
        /* acc=0, MAC1=7*13=91, MAC2=11*17=187 → 278 */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 7\n\t"
                "mov rdx, 13\n\t"
                "mov r9, 11\n\t"
                "mov r10, 17\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8", "r9", "r10"
        );
        printf("Test 2: acc=0, MAC1=7*13, MAC2=11*17\n");
        printf("  RAX = 0x%016" PRIx64 "  (expect 0x116 = 278)\n", rax_out);
        printf("  R8  = 0x%016" PRIx64 "  (expect 0x0)\n", r8_out);
        printf("  %s\n\n", (rax_out == 278 && r8_out == 0) ? "PASS" : "FAIL");

        /* ── Test 3: With initial accumulator ── */
        /* acc=100, MAC1=7*13=91, MAC2=11*17=187 → 378 */
        asm volatile(
                "mov rax, 100\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 7\n\t"
                "mov rdx, 13\n\t"
                "mov r9, 11\n\t"
                "mov r10, 17\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8", "r9", "r10"
        );
        printf("Test 3: acc=100, MAC1=7*13, MAC2=11*17\n");
        printf("  RAX = 0x%016" PRIx64 "  (expect 0x17a = 378)\n", rax_out);
        printf("  R8  = 0x%016" PRIx64 "  (expect 0x0)\n", r8_out);
        printf("  %s\n\n", (rax_out == 378 && r8_out == 0) ? "PASS" : "FAIL");

        /* ── Test 4: Large values — exercises 128-bit overflow ── */
        /* acc=0, MAC1=max*max, MAC2=1*3 */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 0xFFFFFFFFFFFFFFFF\n\t"
                "mov rdx, 0xFFFFFFFFFFFFFFFF\n\t"
                "mov r9, 1\n\t"
                "mov r10, 3\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8", "r9", "r10"
        );
        /* max*max = FFFFFFFFFFFFFFFE:0000000000000001
         * + 3   = FFFFFFFFFFFFFFFE:0000000000000004 */
        printf("Test 4: acc=0, MAC1=max*max, MAC2=1*3\n");
        printf("  RAX = 0x%016" PRIx64 "  (expect 0x0000000000000004)\n", rax_out);
        printf("  R8  = 0x%016" PRIx64 "  (expect 0xfffffffffffffffe)\n", r8_out);
        printf("  %s\n\n",
               (rax_out == 4 && r8_out == 0xFFFFFFFFFFFFFFFEULL) ? "PASS" : "FAIL");

        /* ── Test 5: Carry from MAC1 propagates through MAC2 ── */
        /* acc_lo=0xFF..FD, acc_hi=5, MAC1=1*2=2 (causes lo overflow), MAC2=1*1=1 */
        asm volatile(
                "mov rax, 0xFFFFFFFFFFFFFFFD\n\t"
                "mov r8, 5\n\t"
                "mov rcx, 1\n\t"
                "mov rdx, 2\n\t"
                "mov r9, 1\n\t"
                "mov r10, 1\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8", "r9", "r10"
        );
        /* acc=5:FFFFFFFFFFFFFFFD + 1*2 = 5:FFFFFFFFFFFFFFFF
         * + 1*1 = 6:0000000000000000 */
        printf("Test 5: acc=5:FF..FD, MAC1=1*2, MAC2=1*1 (cross-MAC carry)\n");
        printf("  RAX = 0x%016" PRIx64 "  (expect 0x0000000000000000)\n", rax_out);
        printf("  R8  = 0x%016" PRIx64 "  (expect 0x0000000000000006)\n", r8_out);
        printf("  %s\n\n", (rax_out == 0 && r8_out == 6) ? "PASS" : "FAIL");

        return 0;
}
