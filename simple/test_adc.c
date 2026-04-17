/*
 * test_adc.c — Focused test of ADC+GENARITHFLAGS 5-word carry chain
 * Exactly the Phase A' pattern from p256_sq.
 *
 * Build: make PROG=test_adc
 * Run:   sudo taskset -c 0 ./test_adc_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define _ADC_DSZ64 (0x37eUL << 32)
#define ADC_DSZ64_DRR(dst, src0, src1) ( _ADC_DSZ64 | INSTR_DRR(dst, src0, src1) )

int main(void) {
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    printf("=== 5-word ADC chain tests ===\n\n");

    /* ── Test 1: Simple no-carry case ── */
    /* {1,2,3,4,5} + {10,20,30,40,50} = {11,22,33,44,55} */
    {
        ucode_t patch[] = {
            /* Load "b" values from arch regs to TMP product positions */
            { ZEROEXT_DSZ64_DR(TMP0, RDI), /* prod[0] */
              ZEROEXT_DSZ64_DR(TMP2, RSI), /* prod[1] */
              NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(TMP4, R12), /* prod[2] */
              ZEROEXT_DSZ64_DR(TMP5, R11), /* prod[3] */
              ZEROEXT_DSZ64_DR(TMP6, R14), /* prod[4] */
              NOP_SEQWORD },
            /* 5-word ADC chain: acc(R15,R9,R10,R13,RAX) += prod(TMP0,2,4,5,6) */
            { ADD_DSZ64_DRR(TMP0, R15, TMP0), GENARITHFLAGS_R(TMP0),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP2, R9, TMP2), GENARITHFLAGS_R(TMP2),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP4, R10, TMP4), GENARITHFLAGS_R(TMP4),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP5, R13, TMP5), GENARITHFLAGS_R(TMP5),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP6, RAX, TMP6), NOP,
              NOP, NOP_SEQWORD },
            /* Copy back */
            { ZEROEXT_DSZ64_DR(R15, TMP0), ZEROEXT_DSZ64_DR(R9, TMP2),
              ZEROEXT_DSZ64_DR(R10, TMP4), NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R13, TMP5), ZEROEXT_DSZ64_DR(RAX, TMP6),
              NOP, END_SEQWORD },
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);

        uint64_t res[5];
        register uint64_t *_p asm("r15") = res;
        asm volatile(
            "push r15\n\t"
            /* acc = {1,2,3,4,5} */
            "mov r15, 1\n\t"
            "mov r9, 2\n\t"
            "mov r10, 3\n\t"
            "mov r13, 4\n\t"
            "mov rax, 5\n\t"
            /* prod = {10,20,30,40,50} */
            "mov rdi, 10\n\t"
            "mov rsi, 20\n\t"
            "mov r12, 30\n\t"
            "mov r11, 40\n\t"
            "mov r14, 50\n\t"
            "vmwrite rcx, rdx\n\t"
            "pop rcx\n\t"
            "mov [rcx],    r15\n\t"
            "mov [rcx+8],  r9\n\t"
            "mov [rcx+16], r10\n\t"
            "mov [rcx+24], r13\n\t"
            "mov [rcx+32], rax\n\t"
            : "+r"(_p)
            :
            : "rax", "rcx", "rdx", "rsi", "rdi",
              "r9", "r10", "r11", "r12", "r13", "r14", "memory", "cc"
        );
        int ok = (res[0]==11 && res[1]==22 && res[2]==33 && res[3]==44 && res[4]==55);
        printf("Test 1 (no carry): {%lu,%lu,%lu,%lu,%lu} expect {11,22,33,44,55} → %s\n",
               res[0],res[1],res[2],res[3],res[4], ok?"PASS":"FAIL");
    }

    /* ── Test 2: Carry from word 0 only ── */
    /* {FFF..F, 0, 0, 0, 0} + {1, 0, 0, 0, 0} = {0, 1, 0, 0, 0} */
    {
        init_match_and_patch(); do_fix_IN_patch();
        ucode_t patch[] = {
            { ZEROEXT_DSZ64_DR(TMP0, RDI),
              ZEROEXT_DSZ64_DR(TMP2, RSI),
              NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(TMP4, R12),
              ZEROEXT_DSZ64_DR(TMP5, R11),
              ZEROEXT_DSZ64_DR(TMP6, R14),
              NOP_SEQWORD },
            { ADD_DSZ64_DRR(TMP0, R15, TMP0), GENARITHFLAGS_R(TMP0),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP2, R9, TMP2), GENARITHFLAGS_R(TMP2),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP4, R10, TMP4), GENARITHFLAGS_R(TMP4),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP5, R13, TMP5), GENARITHFLAGS_R(TMP5),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP6, RAX, TMP6), NOP,
              NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R15, TMP0), ZEROEXT_DSZ64_DR(R9, TMP2),
              ZEROEXT_DSZ64_DR(R10, TMP4), NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R13, TMP5), ZEROEXT_DSZ64_DR(RAX, TMP6),
              NOP, END_SEQWORD },
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);

        uint64_t res[5];
        register uint64_t *_p asm("r15") = res;
        asm volatile(
            "push r15\n\t"
            "mov r15, -1\n\t"
            "xor r9d, r9d\n\t"
            "xor r10d, r10d\n\t"
            "xor r13d, r13d\n\t"
            "xor eax, eax\n\t"
            "mov rdi, 1\n\t"
            "xor esi, esi\n\t"
            "xor r12d, r12d\n\t"
            "xor r11d, r11d\n\t"
            "xor r14d, r14d\n\t"
            "vmwrite rcx, rdx\n\t"
            "pop rcx\n\t"
            "mov [rcx],    r15\n\t"
            "mov [rcx+8],  r9\n\t"
            "mov [rcx+16], r10\n\t"
            "mov [rcx+24], r13\n\t"
            "mov [rcx+32], rax\n\t"
            : "+r"(_p)
            :
            : "rax", "rcx", "rdx", "rsi", "rdi",
              "r9", "r10", "r11", "r12", "r13", "r14", "memory", "cc"
        );
        int ok = (res[0]==0 && res[1]==1 && res[2]==0 && res[3]==0 && res[4]==0);
        printf("Test 2 (carry w0→w1): {%lx,%lx,%lx,%lx,%lx} expect {0,1,0,0,0} → %s\n",
               res[0],res[1],res[2],res[3],res[4], ok?"PASS":"FAIL");
    }

    /* ── Test 3: Carry cascades all the way ── */
    /* {FFF..F, FFF..F, FFF..F, FFF..F, 0} + {1,0,0,0,0} = {0,0,0,0,1} */
    {
        init_match_and_patch(); do_fix_IN_patch();
        ucode_t patch[] = {
            { ZEROEXT_DSZ64_DR(TMP0, RDI),
              ZEROEXT_DSZ64_DR(TMP2, RSI),
              NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(TMP4, R12),
              ZEROEXT_DSZ64_DR(TMP5, R11),
              ZEROEXT_DSZ64_DR(TMP6, R14),
              NOP_SEQWORD },
            { ADD_DSZ64_DRR(TMP0, R15, TMP0), GENARITHFLAGS_R(TMP0),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP2, R9, TMP2), GENARITHFLAGS_R(TMP2),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP4, R10, TMP4), GENARITHFLAGS_R(TMP4),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP5, R13, TMP5), GENARITHFLAGS_R(TMP5),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP6, RAX, TMP6), NOP,
              NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R15, TMP0), ZEROEXT_DSZ64_DR(R9, TMP2),
              ZEROEXT_DSZ64_DR(R10, TMP4), NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R13, TMP5), ZEROEXT_DSZ64_DR(RAX, TMP6),
              NOP, END_SEQWORD },
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);

        uint64_t res[5];
        register uint64_t *_p asm("r15") = res;
        asm volatile(
            "push r15\n\t"
            "mov r15, -1\n\t"
            "mov r9, -1\n\t"
            "mov r10, -1\n\t"
            "mov r13, -1\n\t"
            "xor eax, eax\n\t"
            "mov rdi, 1\n\t"
            "xor esi, esi\n\t"
            "xor r12d, r12d\n\t"
            "xor r11d, r11d\n\t"
            "xor r14d, r14d\n\t"
            "vmwrite rcx, rdx\n\t"
            "pop rcx\n\t"
            "mov [rcx],    r15\n\t"
            "mov [rcx+8],  r9\n\t"
            "mov [rcx+16], r10\n\t"
            "mov [rcx+24], r13\n\t"
            "mov [rcx+32], rax\n\t"
            : "+r"(_p)
            :
            : "rax", "rcx", "rdx", "rsi", "rdi",
              "r9", "r10", "r11", "r12", "r13", "r14", "memory", "cc"
        );
        int ok = (res[0]==0 && res[1]==0 && res[2]==0 && res[3]==0 && res[4]==1);
        printf("Test 3 (cascade all): {%lx,%lx,%lx,%lx,%lx} expect {0,0,0,0,1} → %s\n",
               res[0],res[1],res[2],res[3],res[4], ok?"PASS":"FAIL");
    }

    /* ── Test 4: Same as Test 3 but called TWICE (simulates 2 iterations) ── */
    /* First call: {FFF..F,FFF..F,FFF..F,FFF..F,0} + {1,0,0,0,0} = {0,0,0,0,1} */
    /* Second call: {0,0,0,0,1} + {0,0,0,0,0} = {0,0,0,0,1} (no change) */
    {
        init_match_and_patch(); do_fix_IN_patch();
        ucode_t patch[] = {
            { ZEROEXT_DSZ64_DR(TMP0, RDI),
              ZEROEXT_DSZ64_DR(TMP2, RSI),
              NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(TMP4, R12),
              ZEROEXT_DSZ64_DR(TMP5, R11),
              ZEROEXT_DSZ64_DR(TMP6, R14),
              NOP_SEQWORD },
            { ADD_DSZ64_DRR(TMP0, R15, TMP0), GENARITHFLAGS_R(TMP0),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP2, R9, TMP2), GENARITHFLAGS_R(TMP2),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP4, R10, TMP4), GENARITHFLAGS_R(TMP4),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP5, R13, TMP5), GENARITHFLAGS_R(TMP5),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP6, RAX, TMP6), NOP,
              NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R15, TMP0), ZEROEXT_DSZ64_DR(R9, TMP2),
              ZEROEXT_DSZ64_DR(R10, TMP4), NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R13, TMP5), ZEROEXT_DSZ64_DR(RAX, TMP6),
              NOP, END_SEQWORD },
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);

        uint64_t res[5];
        register uint64_t *_p asm("r15") = res;
        asm volatile(
            "push r15\n\t"
            /* First call: acc={FFF..F,FFF..F,FFF..F,FFF..F,0}, prod={1,0,0,0,0} */
            "mov r15, -1\n\t"
            "mov r9, -1\n\t"
            "mov r10, -1\n\t"
            "mov r13, -1\n\t"
            "xor eax, eax\n\t"
            "mov rdi, 1\n\t"
            "xor esi, esi\n\t"
            "xor r12d, r12d\n\t"
            "xor r11d, r11d\n\t"
            "xor r14d, r14d\n\t"
            "vmwrite rcx, rdx\n\t"
            /* Second call: acc should be {0,0,0,0,1}, prod={0,0,0,0,0} */
            /* prod regs already zero from above */
            "xor edi, edi\n\t"
            "vmwrite rcx, rdx\n\t"
            "pop rcx\n\t"
            "mov [rcx],    r15\n\t"
            "mov [rcx+8],  r9\n\t"
            "mov [rcx+16], r10\n\t"
            "mov [rcx+24], r13\n\t"
            "mov [rcx+32], rax\n\t"
            : "+r"(_p)
            :
            : "rax", "rcx", "rdx", "rsi", "rdi",
              "r9", "r10", "r11", "r12", "r13", "r14", "memory", "cc"
        );
        int ok = (res[0]==0 && res[1]==0 && res[2]==0 && res[3]==0 && res[4]==1);
        printf("Test 4 (2 calls): {%lx,%lx,%lx,%lx,%lx} expect {0,0,0,0,1} → %s\n",
               res[0],res[1],res[2],res[3],res[4], ok?"PASS":"FAIL");
    }

    /* ── Test 5: Two calls where second has carry ── */
    /* Call 1: {0,0,0,0,0} + {FFF..F,FFF..F,FFF..F,FFF..F,0} = {FFF..F,FFF..F,FFF..F,FFF..F,0} */
    /* Call 2: {FFF..F,FFF..F,FFF..F,FFF..F,0} + {1,0,0,0,0} = {0,0,0,0,1} */
    {
        init_match_and_patch(); do_fix_IN_patch();
        ucode_t patch[] = {
            { ZEROEXT_DSZ64_DR(TMP0, RDI),
              ZEROEXT_DSZ64_DR(TMP2, RSI),
              NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(TMP4, R12),
              ZEROEXT_DSZ64_DR(TMP5, R11),
              ZEROEXT_DSZ64_DR(TMP6, R14),
              NOP_SEQWORD },
            { ADD_DSZ64_DRR(TMP0, R15, TMP0), GENARITHFLAGS_R(TMP0),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP2, R9, TMP2), GENARITHFLAGS_R(TMP2),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP4, R10, TMP4), GENARITHFLAGS_R(TMP4),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP5, R13, TMP5), GENARITHFLAGS_R(TMP5),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(TMP6, RAX, TMP6), NOP,
              NOP, NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R15, TMP0), ZEROEXT_DSZ64_DR(R9, TMP2),
              ZEROEXT_DSZ64_DR(R10, TMP4), NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R13, TMP5), ZEROEXT_DSZ64_DR(RAX, TMP6),
              NOP, END_SEQWORD },
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);

        uint64_t res[5];
        register uint64_t *_p asm("r15") = res;
        asm volatile(
            "push r15\n\t"
            /* Call 1: acc=0, prod={FFF..F,...,0} */
            "xor r15d, r15d\n\t"
            "xor r9d, r9d\n\t"
            "xor r10d, r10d\n\t"
            "xor r13d, r13d\n\t"
            "xor eax, eax\n\t"
            "mov rdi, -1\n\t"
            "mov rsi, -1\n\t"
            "mov r12, -1\n\t"
            "mov r11, -1\n\t"
            "xor r14d, r14d\n\t"
            "vmwrite rcx, rdx\n\t"
            /* Call 2: prod={1,0,0,0,0} — acc from call 1 persists in arch regs */
            "mov rdi, 1\n\t"
            "xor esi, esi\n\t"
            "xor r12d, r12d\n\t"
            "xor r11d, r11d\n\t"
            "vmwrite rcx, rdx\n\t"
            "pop rcx\n\t"
            "mov [rcx],    r15\n\t"
            "mov [rcx+8],  r9\n\t"
            "mov [rcx+16], r10\n\t"
            "mov [rcx+24], r13\n\t"
            "mov [rcx+32], rax\n\t"
            : "+r"(_p)
            :
            : "rax", "rcx", "rdx", "rsi", "rdi",
              "r9", "r10", "r11", "r12", "r13", "r14", "memory", "cc"
        );
        int ok = (res[0]==0 && res[1]==0 && res[2]==0 && res[3]==0 && res[4]==1);
        printf("Test 5 (2 calls, carry on 2nd): {%lx,%lx,%lx,%lx,%lx} expect {0,0,0,0,1} → %s\n",
               res[0],res[1],res[2],res[3],res[4], ok?"PASS":"FAIL");
    }

    printf("\n=== Done ===\n");
    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
