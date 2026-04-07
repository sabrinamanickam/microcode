/*
 * hook.c — Install MAC128 microcode hook for CryptOpt vmwrite MAC
 *
 * vmwrite rcx, rdx  (hooked at 0x0cd8 → patch @ 0x7c00)
 * RAX:R8 += RCX × RDX
 *
 * INPUT:  RCX=multiplicand, RDX=multiplier, RAX=acc_lo, R8=acc_hi
 * OUTPUT: RAX=acc_lo, R8=acc_hi
 * CLOBBERS: RCX, RDX
 *
 * Build:
 *   gcc -static -O3 -masm=intel \
 *       -I ../code/lib-micro/include \
 *       hook.c ../code/lib-micro/build/libmicro.a \
 *       -o hook
 *
 * Run (installs hook, then exec's the CryptOpt benchmark):
 *   sudo taskset -c 0 ./hook
 *
 * Or just install and exit:
 *   sudo taskset -c 0 ./hook --install-only
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

void install_mac128(void) {
    ucode_t mac128_patch[] = {
        /* Triad 0: save acc_lo, multiply */
        {
            ZEROEXT_DSZ64_DR(TMP3, RAX),
            MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
            NOP,
            NOP_SEQWORD
        },
        /* Triad 1: sum + carry-detect operands */
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
        /* Triad 3: merge carry chain */
        {
            OR_DSZ64_DRR(TMP5, TMP1, TMP4),
            NOP,
            NOP,
            NOP_SEQWORD
        },
        /* Triad 4: extract carry bit 63 */
        {
            SHR_DSZ64_DRI(TMP5, TMP5, 63),
            NOP,
            NOP,
            NOP_SEQWORD
        },
        /* Triad 5: fold carry, done */
        {
            ADD_DSZ64_DRR(R8, R8, TMP5),
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

/* Quick smoke test: 0 + 7×13 = 91 */
int verify_hook(void) {
    uint64_t rax_out, r8_out;

    asm volatile(
        "xor rax, rax\n\t"
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

    printf("  vmwrite smoke test: 0 + 7*13 = %lu (expect 91) ... %s\n",
           rax_out, (rax_out == 91 && r8_out == 0) ? "PASS" : "FAIL");
    return (rax_out == 91 && r8_out == 0) ? 0 : 1;
}

int main(int argc, char *argv[]) {
    printf("Installing MAC128 hook (6 triads @ 0x7c00, CAM 0x0cd8)...\n");
    install_mac128();
    printf("Hook installed.\n");

    if (verify_hook() != 0) {
        fprintf(stderr, "Hook verification FAILED — aborting.\n");
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "--install-only") == 0) {
        printf("Hook active on core 0. Exiting.\n");
        return 0;
    }

    /* If extra args given, exec them with the hook already installed.
     * e.g.: sudo taskset -c 0 ./hook node dist/CryptOpt.js --curve curve25519 --method square
     */
    if (argc > 1) {
        printf("Exec: %s ...\n", argv[1]);
        execvp(argv[1], &argv[1]);
        perror("execvp");
        return 1;
    }

    printf("\nUsage:\n");
    printf("  sudo taskset -c 0 ./hook --install-only\n");
    printf("  sudo taskset -c 0 ./hook node dist/CryptOpt.js --curve curve25519 --method square\n");
    return 0;
}
