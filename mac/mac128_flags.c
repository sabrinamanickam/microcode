/*
 * mac128_flags.c — MAC128 via VMWRITE, flag-based carry detection
 *
 * vmwrite rcx, rdx  (hooked at 0x0cd8 → patch 0x7c00)
 * acc_hi:acc_lo += RCX × RDX
 *
 * CLOBBERS: RCX, RDX
 *
 * CHANGE vs mac128_nomovs: replaces the 4-op manual carry chain
 *   (AND/OR/NOTAND/SHR across triads 1-4) with:
 *   GENARITHFLAGS_RR  →  SETCC_CONDB_DR
 *
 * 6 triads → 4 triads,  9 real ops → 6 real ops
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

void install_mac128(void) {
        ucode_t mac128_patch[] = {
                /* Triad 0: save acc_lo, multiply
                 *   TMP3 = old RAX (acc_lo)
                 *   MUL: RDX:RCX = RCX × RDX  (prod_hi:prod_lo)
                 */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: accumulate + generate carry flags
                 *   RAX = TMP3 + RCX          (acc_lo += prod_lo)
                 *   R8  = R8  + RDX           (acc_hi += prod_hi)
                 *   flags ← arith(TMP3, RCX)  (CF from same addition)
                 *
                 * GENARITHFLAGS has MOD2 → slot 2
                 */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        GENARITHFLAGS_RR(TMP3, RCX),
                        NOP_SEQWORD
                },
                /* Triad 2: materialize carry bit
                 *   TMP0 = (CF ? 1 : 0)
                 */
                {
                        SETCC_CONDB_DR(TMP0, TMP0),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 3: fold carry into acc_hi, done */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP0),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac128_patch, ARRAY_SZ(mac128_patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

int main(void) {
        uint64_t rax_out, r8_out;

        printf("Installing MAC128-flags patch (4 triads, GENARITHFLAGS+SETCC)...\n");
        install_mac128();

        /* Test 1: acc=100, += 7×13 → 191 (no carry) */
        asm volatile(
                "mov rax, 100\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 7\n\t"
                "mov rdx, 13\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8"
        );
        printf("\nTest 1: acc=100, += 7*13\n");
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x00000000000000bf)\n", rax_out);
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0x0000000000000000)\n", r8_out);
        printf("  %s\n", (rax_out == 191 && r8_out == 0) ? "PASS" : "FAIL");

        /* Test 2: acc=0, += max×max (exercises carry + large prod_hi) */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 0xFFFFFFFFFFFFFFFF\n\t"
                "mov rdx, 0xFFFFFFFFFFFFFFFF\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8"
        );
        printf("\nTest 2: acc=0, += max*max\n");
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x0000000000000001)\n", rax_out);
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0xfffffffffffffffe)\n", r8_out);
        printf("  %s\n", (rax_out == 0x1ULL && r8_out == 0xFFFFFFFFFFFFFFFEULL) ? "PASS" : "FAIL");

        /* Test 3: low overflow → carry to hi (THE critical carry test) */
        asm volatile(
                "mov rax, 0xFFFFFFFFFFFFFFFF\n\t"
                "mov r8, 5\n\t"
                "mov rcx, 1\n\t"
                "mov rdx, 3\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8"
        );
        printf("\nTest 3: acc_lo=0xFF..F, acc_hi=5, += 1*3\n");
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x0000000000000002)\n", rax_out);
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0x0000000000000006)\n", r8_out);
        printf("  %s\n", (rax_out == 2 && r8_out == 6) ? "PASS" : "FAIL");

        /* Test 4: 3-MAC chain: 0 + 7*13 + 11*17 + 3*5 = 293 */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, 7\n\t"
                "mov rdx, 13\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov rcx, 11\n\t"
                "mov rdx, 17\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov rcx, 3\n\t"
                "mov rdx, 5\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(rax_out), "=r"(r8_out)
                :
                : "rax", "rcx", "rdx", "r8"
        );
        printf("\nTest 4: chain 0 + 7*13 + 11*17 + 3*5\n");
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x0000000000000125 = 293)\n", rax_out);
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0x0000000000000000)\n", r8_out);
        printf("  %s\n", (rax_out == 293 && r8_out == 0) ? "PASS" : "FAIL");

        return 0;
}

