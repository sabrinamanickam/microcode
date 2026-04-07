/*
 * test.c — Quick MAC128 carry_square test
 *
 * BUILD:
 *   make PROG=test   (after assembling carry_square_mac128.o)
 *   OR:
 *   nasm -f elf64 -o carry_square_mac128.o carry_square_mac128.asm
 *   gcc -static -O3 -masm=intel -march=x86-64-v2 -I include/ \
 *       test.c carry_square_mac128.o ../../build/libmicro.a -o test_static
 *
 * RUN:
 *   sudo ./test_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

typedef unsigned __int128 uint128_t;

extern void carry_square_mac128(uint64_t out1[5], const uint64_t arg1[5]);

static void carry_square_c(uint64_t out1[5], const uint64_t arg1[5]) {
        uint64_t x1 = arg1[4] * (uint64_t)0x13;
        uint64_t x2 = x1 * 0x2;
        uint64_t x3 = arg1[4] * 0x2;
        uint64_t x4 = arg1[3] * (uint64_t)0x13;
        uint64_t x5 = x4 * 0x2;
        uint64_t x6 = arg1[3] * 0x2;
        uint64_t x7 = arg1[2] * 0x2;
        uint64_t x8 = arg1[1] * 0x2;
        uint128_t x24 = (uint128_t)arg1[0]*arg1[0] + (uint128_t)arg1[1]*x2 + (uint128_t)arg1[2]*x5;
        uint64_t x25 = (uint64_t)(x24 >> 51);
        uint64_t x26 = (uint64_t)(x24 & 0x7FFFFFFFFFFFFULL);
        uint128_t x31 = x25 + (uint128_t)arg1[0]*x8 + (uint128_t)arg1[2]*x2 + (uint128_t)arg1[3]*x4;
        uint64_t x32 = (uint64_t)(x31 >> 51);
        uint64_t x33 = (uint64_t)(x31 & 0x7FFFFFFFFFFFFULL);
        uint128_t x34 = x32 + (uint128_t)arg1[0]*x7 + (uint128_t)arg1[1]*arg1[1] + (uint128_t)arg1[3]*x2;
        uint64_t x35 = (uint64_t)(x34 >> 51);
        uint64_t x36 = (uint64_t)(x34 & 0x7FFFFFFFFFFFFULL);
        uint128_t x37 = x35 + (uint128_t)arg1[0]*x6 + (uint128_t)arg1[1]*x7 + (uint128_t)arg1[4]*x1;
        uint64_t x38 = (uint64_t)(x37 >> 51);
        uint64_t x39 = (uint64_t)(x37 & 0x7FFFFFFFFFFFFULL);
        uint128_t x40 = x38 + (uint128_t)arg1[0]*x3 + (uint128_t)arg1[1]*x6 + (uint128_t)arg1[2]*arg1[2];
        uint64_t x41 = (uint64_t)(x40 >> 51);
        uint64_t x42 = (uint64_t)(x40 & 0x7FFFFFFFFFFFFULL);
        uint64_t x43 = x41 * 0x13;
        uint64_t x44 = x26 + x43;
        uint64_t x45 = x44 >> 51;
        uint64_t x46 = x44 & 0x7FFFFFFFFFFFFULL;
        uint64_t x47 = x45 + x33;
        uint64_t x48 = x47 >> 51;
        uint64_t x49 = x47 & 0x7FFFFFFFFFFFFULL;
        uint64_t x50 = x48 + x36;
        out1[0] = x46; out1[1] = x49; out1[2] = x50; out1[3] = x39; out1[4] = x42;
}

static void install_mac128(void) {
        ucode_t mac128_patch[] = {
                { MOVE_DSZ64_DR(TMP3, RAX), MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST), NOP, NOP_SEQWORD },
                { ADD_DSZ64_DRR(RAX, TMP3, RCX), AND_DSZ64_DRR(TMP1, TMP3, RCX), OR_DSZ64_DRR(TMP2, TMP3, RCX), NOP_SEQWORD },
                { NOTAND_DSZ64_DRR(TMP4, RAX, TMP2), ADD_DSZ64_DRR(R8, R8, RDX), NOP, NOP_SEQWORD },
                { OR_DSZ64_DRR(TMP5, TMP1, TMP4), NOP, NOP, NOP_SEQWORD },
                { SHR_DSZ64_DRI(TMP5, TMP5, 63), NOP, NOP, NOP_SEQWORD },
                { ADD_DSZ64_DRR(R8, R8, TMP5), NOP, NOP, END_SEQWORD }
        };
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac128_patch, ARRAY_SZ(mac128_patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

int main(void) {
        printf("Installing MAC128...\n");
        install_mac128();

        uint64_t tests[][5] = {
                {0, 0, 0, 0, 0},
                {1, 0, 0, 0, 0},
                {123, 456, 789, 1011, 1213},
                {0x0007FFFFFFFFFFFFULL, 0x0007FFFFFFFFFFFFULL,
                 0x0007FFFFFFFFFFFFULL, 0x0007FFFFFFFFFFFFULL,
                 0x0007FFFFFFFFFFFFULL},
                {0x34A2F1B8C7D00ULL, 0x5E91C3A287600ULL,
                 0x1F4D8B2E63A00ULL, 0x68C4F1D2A5B00ULL,
                 0x2B7E0A9C15D00ULL},
        };
        int n = sizeof(tests) / sizeof(tests[0]);
        int pass = 0;

        for (int i = 0; i < n; i++) {
                uint64_t c_out[5], mac_out[5];
                carry_square_c(c_out, tests[i]);
                carry_square_mac128(mac_out, tests[i]);
                int ok = (memcmp(c_out, mac_out, 40) == 0);
                printf("Test %d: %s\n", i+1, ok ? "PASS" : "FAIL");
                if (!ok) {
                        for (int j = 0; j < 5; j++)
                                printf("  [%d] C=%016" PRIx64 "  MAC=%016" PRIx64 "%s\n",
                                       j, c_out[j], mac_out[j],
                                       c_out[j] != mac_out[j] ? " ✗" : "");
                }
                pass += ok;
        }

        printf("\n%d / %d passed\n", pass, n);

        /* Quick single timing */
        if (pass == n) {
                uint64_t in[5] = {0x34A2F1B8C7D00ULL, 0x5E91C3A287600ULL,
                                   0x1F4D8B2E63A00ULL, 0x68C4F1D2A5B00ULL,
                                   0x2B7E0A9C15D00ULL};
                uint64_t out[5];
                uint32_t lo, hi;

                /* Warmup */
                for (int i = 0; i < 100; i++)
                        carry_square_mac128(out, in);

                asm volatile("cpuid\n\trdtsc" : "=a"(lo),"=d"(hi) : "a"(0) : "rbx","rcx");
                uint64_t t0 = ((uint64_t)hi << 32) | lo;

                for (int i = 0; i < 1000; i++) {
                        carry_square_mac128(out, in);
                        in[0]=out[0]; in[1]=out[1]; in[2]=out[2]; in[3]=out[3]; in[4]=out[4];
                }

                asm volatile("rdtscp" : "=a"(lo),"=d"(hi) : : "rcx");
                uint64_t t1 = ((uint64_t)hi << 32) | lo;

                printf("\nMAC128: ~%" PRIu64 " cycles/call (1000 calls)\n", (t1-t0)/1000);
        }

        return 0;
}
