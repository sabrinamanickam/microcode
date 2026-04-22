/*
 * test_adc.c — Narrow down GENARITHFLAGS carry leak from previous triad
 *
 * Hypothesis: ADD in slot 1 of triad N leaks its carry into
 * GENARITHFLAGS_R in triad N+1, overriding slot 0's ADD carry.
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

    printf("=== GENARITHFLAGS carry-leak from previous triad ===\n\n");

    /* ── Test 1: Baseline — ADD(slot 0) + GENARITHFLAGS, no prior ADD ── */
    /* ADD 3+4=7, no overflow → CF should be 0 → ADC adds 0 */
    {
        ucode_t patch[] = {
            { ADD_DSZ64_DRR(TMP0, R9, R10), GENARITHFLAGS_R(TMP0),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(RAX, RAX, RAX), NOP, NOP, END_SEQWORD }
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
        uint64_t res;
        asm volatile("xor eax,eax\n\t" "mov r9,3\n\t" "mov r10,4\n\t"
                     "vmwrite rcx,rdx\n\t" : "=a"(res) : : "rcx","rdx","r9","r10","memory","cc");
        printf("Test 1 (baseline, no prior ADD): ADC got %lu, expect 0 → %s\n",
               res, res==0?"PASS":"FAIL");
    }

    /* ── Test 2: Prior triad has ADD(slot 0) that OVERFLOWS, then our ADD(slot 0) does NOT overflow ── */
    /* Prior: ADD(FFF..F + 1) → CF=1 internally.  Current: ADD(3+4) → CF=0.  ADC should get 0. */
    {
        init_match_and_patch(); do_fix_IN_patch();
        ucode_t patch[] = {
            /* Prior triad: ADD in SLOT 0 overflows */
            { ADD_DSZ64_DRR(TMP2, R13, R14), NOP, NOP, NOP_SEQWORD },
            /* Current: ADD in slot 0 (no overflow) + GENARITHFLAGS */
            { ADD_DSZ64_DRR(TMP0, R9, R10), GENARITHFLAGS_R(TMP0),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(RAX, RAX, RAX), NOP, NOP, END_SEQWORD }
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
        uint64_t res;
        asm volatile("xor eax,eax\n\t" "mov r9,3\n\t" "mov r10,4\n\t"
                     "mov r13,-1\n\t" "mov r14,1\n\t"
                     "vmwrite rcx,rdx\n\t" : "=a"(res) : : "rcx","rdx","r9","r10","r13","r14","memory","cc");
        printf("Test 2 (prior slot-0 ADD overflows): ADC got %lu, expect 0 → %s\n",
               res, res==0?"PASS":"FAIL");
    }

    /* ── Test 3: Prior triad has ADD in SLOT 1 that overflows ── */
    /* This is the EXACT pattern from MONT_ITER: ZEROEXT + ADD(slot 1) */
    {
        init_match_and_patch(); do_fix_IN_patch();
        ucode_t patch[] = {
            /* Prior triad: ZEROEXT in slot 0, ADD in SLOT 1 overflows */
            { ZEROEXT_DSZ64_DR(TMP3, R13), ADD_DSZ64_DRR(TMP2, R13, R14),
              NOP, NOP_SEQWORD },
            /* Current: ADD in slot 0 (no overflow) + GENARITHFLAGS */
            { ADD_DSZ64_DRR(TMP0, R9, R10), GENARITHFLAGS_R(TMP0),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(RAX, RAX, RAX), NOP, NOP, END_SEQWORD }
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
        uint64_t res;
        asm volatile("xor eax,eax\n\t" "mov r9,3\n\t" "mov r10,4\n\t"
                     "mov r13,-1\n\t" "mov r14,1\n\t"
                     "vmwrite rcx,rdx\n\t" : "=a"(res) : : "rcx","rdx","r9","r10","r13","r14","memory","cc");
        printf("Test 3 (prior SLOT-1 ADD overflows): ADC got %lu, expect 0 → %s\n",
               res, res==0?"PASS":"FAIL");
    }

    /* ── Test 4: Prior triad has ADD in SLOT 2 that overflows ── */
    {
        init_match_and_patch(); do_fix_IN_patch();
        ucode_t patch[] = {
            { NOP, NOP, ADD_DSZ64_DRR(TMP2, R13, R14), NOP_SEQWORD },
            { ADD_DSZ64_DRR(TMP0, R9, R10), GENARITHFLAGS_R(TMP0),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(RAX, RAX, RAX), NOP, NOP, END_SEQWORD }
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
        uint64_t res;
        asm volatile("xor eax,eax\n\t" "mov r9,3\n\t" "mov r10,4\n\t"
                     "mov r13,-1\n\t" "mov r14,1\n\t"
                     "vmwrite rcx,rdx\n\t" : "=a"(res) : : "rcx","rdx","r9","r10","r13","r14","memory","cc");
        printf("Test 4 (prior SLOT-2 ADD overflows): ADC got %lu, expect 0 → %s\n",
               res, res==0?"PASS":"FAIL");
    }

    /* ── Test 5: Prior triad ADD(slot 1) overflows, current ADD(slot 0) also overflows ── */
    /* GENARITHFLAGS should read current (slot 0) carry = 1, not be confused */
    {
        init_match_and_patch(); do_fix_IN_patch();
        ucode_t patch[] = {
            { ZEROEXT_DSZ64_DR(TMP3, R9), ADD_DSZ64_DRR(TMP2, R13, R14),
              NOP, NOP_SEQWORD },
            { ADD_DSZ64_DRR(TMP0, R13, R14), GENARITHFLAGS_R(TMP0),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(RAX, RAX, RAX), NOP, NOP, END_SEQWORD }
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
        uint64_t res;
        asm volatile("xor eax,eax\n\t" "mov r9,3\n\t" "mov r10,4\n\t"
                     "mov r13,-1\n\t" "mov r14,1\n\t"
                     "vmwrite rcx,rdx\n\t" : "=a"(res) : : "rcx","rdx","r9","r10","r13","r14","memory","cc");
        printf("Test 5 (both prior slot-1 AND current slot-0 overflow): ADC got %lu, expect 1 → %s\n",
               res, res==1?"PASS":"FAIL");
    }

    /* ── Test 6: Prior triad ADD(slot 1) NO overflow, current ADD(slot 0) overflows ── */
    {
        init_match_and_patch(); do_fix_IN_patch();
        ucode_t patch[] = {
            { ZEROEXT_DSZ64_DR(TMP3, R9), ADD_DSZ64_DRR(TMP2, R9, R10),
              NOP, NOP_SEQWORD },
            { ADD_DSZ64_DRR(TMP0, R13, R14), GENARITHFLAGS_R(TMP0),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(RAX, RAX, RAX), NOP, NOP, END_SEQWORD }
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
        uint64_t res;
        asm volatile("xor eax,eax\n\t" "mov r9,3\n\t" "mov r10,4\n\t"
                     "mov r13,-1\n\t" "mov r14,1\n\t"
                     "vmwrite rcx,rdx\n\t" : "=a"(res) : : "rcx","rdx","r9","r10","r13","r14","memory","cc");
        printf("Test 6 (prior slot-1 NO overflow, current slot-0 overflows): ADC got %lu, expect 1 → %s\n",
               res, res==1?"PASS":"FAIL");
    }

    /* ── Test 7: ADD+SETCC in prior triad (slot 0+1), then ADD+GENARITHFLAGS ── */
    /* This is the Phase A pattern: ADD+SETCC, then later ADD+GENARITHFLAGS */
    {
        init_match_and_patch(); do_fix_IN_patch();
        ucode_t patch[] = {
            /* ADD overflows + SETCC captures (like Phase A) */
            { ADD_DSZ64_DRR(TMP2, R13, R14), SETCC_CONDB_DR(TMP3, TMP2),
              NOP, NOP_SEQWORD },
            /* ADD no overflow + GENARITHFLAGS (like Phase A') */
            { ADD_DSZ64_DRR(TMP0, R9, R10), GENARITHFLAGS_R(TMP0),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(RAX, RAX, RAX), NOP, NOP, END_SEQWORD }
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
        uint64_t res;
        asm volatile("xor eax,eax\n\t" "mov r9,3\n\t" "mov r10,4\n\t"
                     "mov r13,-1\n\t" "mov r14,1\n\t"
                     "vmwrite rcx,rdx\n\t" : "=a"(res) : : "rcx","rdx","r9","r10","r13","r14","memory","cc");
        printf("Test 7 (prior ADD+SETCC overflows, current ADD+GENARITHFLAGS no overflow): ADC got %lu, expect 0 → %s\n",
               res, res==0?"PASS":"FAIL");
    }

    /* ── Test 8: The EXACT T15→T16 pattern from MONT_ITER ── */
    /* T15: ZEROEXT(R15,TMP0), ADD(TMP0,R9,TMP2), SETCC(TMP1,TMP0) */
    /* T16: ADD(TMP1,TMP0,TMP3), GENARITHFLAGS_R(TMP1) */
    /* Setup: R9+TMP2 overflows, TMP0+TMP3 does NOT overflow */
    {
        init_match_and_patch(); do_fix_IN_patch();
        ucode_t patch[] = {
            /* Load TMP0, TMP2, TMP3 with known values */
            { ZEROEXT_DSZ64_DR(TMP0, RDI), ZEROEXT_DSZ64_DR(TMP2, RSI),
              ZEROEXT_DSZ64_DR(TMP3, R12), NOP_SEQWORD },
            /* T15 pattern: ZEROEXT + ADD(overflows) + SETCC */
            { ZEROEXT_DSZ64_DR(R15, TMP0), ADD_DSZ64_DRR(TMP0, R9, TMP2),
              SETCC_CONDB_DR(TMP1, TMP0), NOP_SEQWORD },
            /* T16 pattern: ADD(no overflow) + GENARITHFLAGS */
            { ADD_DSZ64_DRR(TMP1, TMP0, TMP3), GENARITHFLAGS_R(TMP1),
              NOP, NOP_SEQWORD },
            /* ADC reads arch CF — should be from T16's ADD, not T15's */
            { ADC_DSZ64_DRR(RAX, RAX, RAX), NOP, NOP, END_SEQWORD }
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);

        uint64_t res;
        asm volatile(
            "xor eax, eax\n\t"
            /* TMP0 = 42, TMP2 = 0 (no overflow), TMP3 = 0 */
            "mov rdi, 42\n\t" "xor esi, esi\n\t" "xor r12d, r12d\n\t"
            /* R9 = FFF..F, will overflow when added to TMP2 in slot 1... wait, TMP2=0 */
            /* Let me make R9+TMP2 overflow: R9=FFF..F, TMP2(rsi)=1 */
            "mov r9, -1\n\t" "mov rsi, 1\n\t"
            "vmwrite rcx, rdx\n\t"
            : "=a"(res) : : "rcx","rdx","rsi","rdi","r9","r12","memory","cc"
        );
        /* T15: ADD(TMP0, R9=FFF..F, TMP2=1) → TMP0=0, overflows (CF=1 internally)
         * T16: ADD(TMP1, TMP0=0, TMP3=0) → TMP1=0, no overflow (CF=0)
         * GENARITHFLAGS should promote CF=0 → ADC adds 0 */
        printf("Test 8 (EXACT T15→T16 pattern, slot-1 overflows but slot-0 doesn't):\n");
        printf("  ADC got %lu, expect 0 → %s\n", res, res==0?"PASS":"FAIL");
    }

    /* ── Test 9: Same as 8 but flip: T15 slot-1 NO overflow, T16 slot-0 DOES overflow ── */
    {
        init_match_and_patch(); do_fix_IN_patch();
        ucode_t patch[] = {
            { ZEROEXT_DSZ64_DR(TMP0, RDI), ZEROEXT_DSZ64_DR(TMP2, RSI),
              ZEROEXT_DSZ64_DR(TMP3, R12), NOP_SEQWORD },
            { ZEROEXT_DSZ64_DR(R15, TMP0), ADD_DSZ64_DRR(TMP0, R9, TMP2),
              SETCC_CONDB_DR(TMP1, TMP0), NOP_SEQWORD },
            { ADD_DSZ64_DRR(TMP1, TMP0, TMP3), GENARITHFLAGS_R(TMP1),
              NOP, NOP_SEQWORD },
            { ADC_DSZ64_DRR(RAX, RAX, RAX), NOP, NOP, END_SEQWORD }
        };
        patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
        hook_match_and_patch(0, 0x0cd8, 0x7c00);

        uint64_t res;
        asm volatile(
            "xor eax, eax\n\t"
            "mov rdi, 42\n\t"
            /* R9+TMP2 = 3+4 = 7, NO overflow */
            "mov r9, 3\n\t" "mov rsi, 4\n\t"
            /* TMP0+TMP3: TMP0 will be 7, TMP3=FFF..F → 7+FFF..F overflows */
            "mov r12, -1\n\t"
            "vmwrite rcx, rdx\n\t"
            : "=a"(res) : : "rcx","rdx","rsi","rdi","r9","r12","memory","cc"
        );
        /* T15: ADD(TMP0, 3, 4) = 7, no overflow
         * T16: ADD(TMP1, 7, FFF..F) = 6, overflows (CF=1)
         * GENARITHFLAGS should promote CF=1 → ADC adds 1 */
        printf("Test 9 (T15 slot-1 no overflow, T16 slot-0 DOES overflow):\n");
        printf("  ADC got %lu, expect 1 → %s\n", res, res==1?"PASS":"FAIL");
    }

    printf("\n=== Done ===\n");
    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
