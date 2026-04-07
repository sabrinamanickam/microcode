/*
 * mac3.c — Install MAC3 microcode hook for CryptOpt vmwrite MAC
 *
 * vmwrite rcx, rdx  (hooked at 0x0cd8 → patch @ 0x7c00)
 * RAX:R8 += RCX×RDX + R9×R10 + R11×R12   (3 fused MACs)
 *
 * INPUT:  RCX=op1a, RDX=op1b, R9=op2a, R10=op2b,
 *         R11=op3a, R12=op3b, RAX=acc_lo, R8=acc_hi
 * OUTPUT: RAX=acc_lo, R8=acc_hi
 * CLOBBERS: RCX, RDX
 *
 * Build:
 *   gcc -static -O3 -masm=intel \
 *       -I ../code/lib-micro/include \
 *       mac3.c ../code/lib-micro/build/libmicro.a \
 *       -o mac3
 *
 * Run (installs hook, then exec's the CryptOpt benchmark):
 *   sudo taskset -c 0 ./mac3
 *
 * Or just install and exit:
 *   sudo taskset -c 0 ./mac3 --install-only
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

void install_mac3(void) {
    ucode_t mac3_patch[] = {
        /* ── MAC 1: RCX × RDX (triads 0–5) ── */
        /* Triad 0: save acc_lo, multiply */
        { ZEROEXT_DSZ64_DR(TMP3, RAX),
          MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
          NOP, NOP_SEQWORD },
        /* Triad 1: sum + carry-detect operands */
        { ADD_DSZ64_DRR(RAX, TMP3, RCX),
          AND_DSZ64_DRR(TMP1, TMP3, RCX),
          OR_DSZ64_DRR(TMP2, TMP3, RCX),
          NOP_SEQWORD },
        /* Triad 2: ~sum & (a|b), acc_hi += prod_hi */
        { NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
          ADD_DSZ64_DRR(R8, R8, RDX),
          NOP, NOP_SEQWORD },
        /* Triad 3: merge carry chain */
        { OR_DSZ64_DRR(TMP5, TMP1, TMP4),
          NOP, NOP, NOP_SEQWORD },
        /* Triad 4: extract carry bit 63 */
        { SHR_DSZ64_DRI(TMP5, TMP5, 63),
          NOP, NOP, NOP_SEQWORD },
        /* Triad 5: fold carry */
        { ADD_DSZ64_DRR(R8, R8, TMP5),
          NOP, NOP, NOP_SEQWORD },

        /* ── Setup MAC 2: R9→RCX, R10→RDX (triad 6) ── */
        { ZEROEXT_DSZ64_DR(RCX, R9),
          ZEROEXT_DSZ64_DR(RDX, R10),
          NOP, NOP_SEQWORD },

        /* ── MAC 2: (new) RCX × RDX (triads 7–12) ── */
        { ZEROEXT_DSZ64_DR(TMP3, RAX),
          MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
          NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRR(RAX, TMP3, RCX),
          AND_DSZ64_DRR(TMP1, TMP3, RCX),
          OR_DSZ64_DRR(TMP2, TMP3, RCX),
          NOP_SEQWORD },
        { NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
          ADD_DSZ64_DRR(R8, R8, RDX),
          NOP, NOP_SEQWORD },
        { OR_DSZ64_DRR(TMP5, TMP1, TMP4),
          NOP, NOP, NOP_SEQWORD },
        { SHR_DSZ64_DRI(TMP5, TMP5, 63),
          NOP, NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRR(R8, R8, TMP5),
          NOP, NOP, NOP_SEQWORD },

        /* ── Setup MAC 3: R11→RCX, R12→RDX (triad 13) ── */
        { ZEROEXT_DSZ64_DR(RCX, R11),
          ZEROEXT_DSZ64_DR(RDX, R12),
          NOP, NOP_SEQWORD },

        /* ── MAC 3: (new) RCX × RDX (triads 14–19) ── */
        { ZEROEXT_DSZ64_DR(TMP3, RAX),
          MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
          NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRR(RAX, TMP3, RCX),
          AND_DSZ64_DRR(TMP1, TMP3, RCX),
          OR_DSZ64_DRR(TMP2, TMP3, RCX),
          NOP_SEQWORD },
        { NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
          ADD_DSZ64_DRR(R8, R8, RDX),
          NOP, NOP_SEQWORD },
        { OR_DSZ64_DRR(TMP5, TMP1, TMP4),
          NOP, NOP, NOP_SEQWORD },
        { SHR_DSZ64_DRI(TMP5, TMP5, 63),
          NOP, NOP, NOP_SEQWORD },
        /* Triad 19: fold carry + END */
        { ADD_DSZ64_DRR(R8, R8, TMP5),
          NOP, NOP, END_SEQWORD }
    };

    printf("Installing MAC3 hook (20 triads @ 0x7c00, CAM 0x0cd8)...\n");
    assign_to_core(0);
    do_fix_IN_patch();
    patch_ucode(0x7d7c, mac3_patch, ARRAY_SZ(mac3_patch));
    hook_match_and_patch(0, 0x0cd8, 0x7d7c);
    printf("Hook installed.\n");
}

/* Replace the 20-triad MAC3 patch with a 1-triad NOP passthrough
 * so the hook becomes a harmless no-op before we exit.
 * Prevents the kernel from hitting our MAC3 logic with garbage regs. */
/*void cleanup_hook(void) {
    ucode_t passthrough[] = {
        { NOP, NOP, NOP, END_SEQWORD }
    };
    patch_ucode(0x7c47, passthrough, ARRAY_SZ(passthrough));
    printf("Hook neutralized (NOP passthrough installed).\n");
}*/

/* Quick smoke test: 7*13 + 11*17 + 3*5 = 91 + 187 + 15 = 293 */
int verify_hook(void) {
    uint64_t rax_out, r8_out;
    int pass = 1;

    /* Test 1: MAC1 only (MAC2/MAC3 operands = 0) */
    asm volatile(
        "xor rax, rax\n\t"
        "xor r8, r8\n\t"
        "mov rcx, 7\n\t"
        "mov rdx, 13\n\t"
        "xor r9, r9\n\t"
        "xor r10, r10\n\t"
        "xor r11, r11\n\t"
        "xor r12, r12\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %0, rax\n\t"
        "mov %1, r8\n\t"
        : "=r"(rax_out), "=r"(r8_out)
        :
        : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12"
    );
    printf("  Test 1 (MAC1 only): 0 + 7*13 = %lu (expect 91) ... %s\n",
           rax_out, (rax_out == 91 && r8_out == 0) ? "PASS" : "FAIL");
    if (rax_out != 91 || r8_out != 0) pass = 0;

    /* Test 2: All 3 MACs active */
    asm volatile(
        "xor rax, rax\n\t"
        "xor r8, r8\n\t"
        "mov rcx, 7\n\t"
        "mov rdx, 13\n\t"
        "mov r9, 11\n\t"
        "mov r10, 17\n\t"
        "mov r11, 3\n\t"
        "mov r12, 5\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %0, rax\n\t"
        "mov %1, r8\n\t"
        : "=r"(rax_out), "=r"(r8_out)
        :
        : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12"
    );
    printf("  Test 2 (all 3):     0 + 7*13 + 11*17 + 3*5 = %lu (expect 293) ... %s\n",
           rax_out, (rax_out == 293 && r8_out == 0) ? "PASS" : "FAIL");
    if (rax_out != 293 || r8_out != 0) pass = 0;

    /* Test 3: With initial accumulator */
    asm volatile(
        "mov rax, 100\n\t"
        "xor r8, r8\n\t"
        "mov rcx, 7\n\t"
        "mov rdx, 13\n\t"
        "mov r9, 11\n\t"
        "mov r10, 17\n\t"
        "mov r11, 3\n\t"
        "mov r12, 5\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %0, rax\n\t"
        "mov %1, r8\n\t"
        : "=r"(rax_out), "=r"(r8_out)
        :
        : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12"
    );
    printf("  Test 3 (with acc):  100 + 7*13 + 11*17 + 3*5 = %lu (expect 393) ... %s\n",
           rax_out, (rax_out == 393 && r8_out == 0) ? "PASS" : "FAIL");
    if (rax_out != 393 || r8_out != 0) pass = 0;

    /* Test 4: Carry propagation across all 3 MACs */
    asm volatile(
        "mov rax, 0xFFFFFFFFFFFFFFFC\n\t"
        "mov r8, 5\n\t"
        "mov rcx, 1\n\t"
        "mov rdx, 2\n\t"
        "mov r9, 1\n\t"
        "mov r10, 1\n\t"
        "mov r11, 1\n\t"
        "mov r12, 1\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %0, rax\n\t"
        "mov %1, r8\n\t"
        : "=r"(rax_out), "=r"(r8_out)
        :
        : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12"
    );
    printf("  Test 4 (carry):     5:FF..FC + 1*2 + 1*1 + 1*1 → %lu:%016lx (expect 6:0) ... %s\n",
           r8_out, rax_out, (rax_out == 0 && r8_out == 6) ? "PASS" : "FAIL");
    if (rax_out != 0 || r8_out != 6) pass = 0;

    return pass ? 0 : 1;
}

int main(int argc, char *argv[]) {
    install_mac3();

    if (verify_hook() != 0) {
        fprintf(stderr, "Hook verification FAILED — aborting.\n");
        //cleanup_hook();
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "--install-only") == 0) {
        printf("Hook active on core 0. Cleaning up before exit.\n");
        //cleanup_hook();
        return 0;
    }

    /* If extra args given, exec them with the hook already installed.
     * e.g.: sudo taskset -c 0 ./mac3 node dist/CryptOpt.js --curve curve25519 --method square
     */
    if (argc > 1) {
        printf("Exec: %s ...\n", argv[1]);
        execvp(argv[1], &argv[1]);
        /* execvp only returns on failure */
        perror("execvp");
        //cleanup_hook();
        return 1;
    }

    printf("\nUsage:\n");
    printf("  sudo taskset -c 0 ./mac3 --install-only\n");
    printf("  sudo taskset -c 0 ./mac3 node dist/CryptOpt.js --curve curve25519 --method square\n");
    //cleanup_hook();
    return 0;
}
