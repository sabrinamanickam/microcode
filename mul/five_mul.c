/*
 * multest.c — 5× MUL_DSZ64 in one VMWRITE hook
 *
 * One vmwrite rcx, rdx performs 5 multiplies of the same inputs.
 * Result of last multiply in RDX:RAX (standard convention).
 *
 * 11 triads: save, (MUL, restore)×4, MUL, rearrange+END
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

void install_5xmul(void) {
        ucode_t patch[] = {
                /* Triad 0: save inputs */
                {
                        ZEROEXT_DSZ64_DR(TMP0, RCX),
                        ZEROEXT_DSZ64_DR(RBX, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: MUL 1 */
                {
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 2: restore */
                {
                        ZEROEXT_DSZ64_DR(RCX, TMP0),
                        ZEROEXT_DSZ64_DR(RDX, RBX),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 3: MUL 2 */
                {
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 4: restore */
                {
                        ZEROEXT_DSZ64_DR(RCX, TMP0),
                        ZEROEXT_DSZ64_DR(RDX, RBX),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 5: MUL 3 */
                {
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 6: restore */
                {
                        ZEROEXT_DSZ64_DR(RCX, TMP0),
                        ZEROEXT_DSZ64_DR(RDX, RBX),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 7: MUL 4 */
                {
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 8: restore */
                {
                        ZEROEXT_DSZ64_DR(RCX, TMP0),
                        ZEROEXT_DSZ64_DR(RDX, RBX),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 9: MUL 5 */
                {
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 10: RCX=lo → RAX, RDX=hi stays */
                {
                        ZEROEXT_DSZ64_DR(RAX, RCX),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

int main(void) {
        uint64_t rax, rdx;

        printf("Installing 5×MUL patch (11 triads)...\n");
        install_5xmul();

        /* Test 1: 5 × 5 = 25 */
        asm volatile(
                "mov rcx, 5\n\t"
                "mov rdx, 5\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, rdx\n\t"
                : "=r"(rax), "=r"(rdx)
                :
                : "rax", "rbx", "rcx", "rdx"
        );
        printf("\nTest 1: 5 × 5\n");
        printf("  RAX (lo) = 0x%016" PRIx64 " (%" PRIu64 ")\n", rax, rax);
        printf("  RDX (hi) = 0x%016" PRIx64 " (%" PRIu64 ")\n", rdx, rdx);
        printf("  %s\n", (rax == 25 && rdx == 0) ? "PASS" : "FAIL");

        /* Test 2: 0xFFFFFFFFFFFFFFFF × 0xFFFFFFFFFFFFFFFF */
        asm volatile(
                "mov rcx, 0xFFFFFFFFFFFFFFFF\n\t"
                "mov rdx, 0xFFFFFFFFFFFFFFFF\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, rdx\n\t"
                : "=r"(rax), "=r"(rdx)
                :
                : "rax", "rbx", "rcx", "rdx"
        );
        printf("\nTest 2: max × max\n");
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x0000000000000001)\n", rax);
        printf("  RDX (hi) = 0x%016" PRIx64 "  (expect 0xfffffffffffffffe)\n", rdx);
        printf("  %s\n", (rax == 1 && rdx == 0xFFFFFFFFFFFFFFFEULL) ? "PASS" : "FAIL");

        /* Test 3: Curve25519 limbs */
        uint64_t a = 0x0007FFFFFFFFFFFFULL;
        uint64_t b = 0x0006000000000000ULL;
        asm volatile(
                "mov rcx, %[a]\n\t"
                "mov rdx, %[b]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %0, rax\n\t"
                "mov %1, rdx\n\t"
                : "=r"(rax), "=r"(rdx)
                : [a] "r"(a), [b] "r"(b)
                : "rax", "rbx", "rcx", "rdx"
        );
        __uint128_t ref = (__uint128_t)a * b;
        printf("\nTest 3: 51-bit limbs\n");
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")\n", rax, (uint64_t)ref);
        printf("  RDX (hi) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")\n", rdx, (uint64_t)(ref >> 64));
        printf("  %s\n", (rax == (uint64_t)ref && rdx == (uint64_t)(ref >> 64)) ? "PASS" : "FAIL");

        return 0;
}
