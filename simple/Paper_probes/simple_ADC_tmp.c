/*
 * simple_ADC_tmp.c — Same as simple_ADC.c but ADC sources are TMP regs.
 *
 * The prior `test_adc_carry_route.c` (2026-04-17, Strategy B) tested
 * whether ADC behaves differently when its sources are TMPs vs arch
 * regs. That test only used a SUM (ADD-then-ADC pattern) — this one
 * is the cleanest standalone test: arch CF in, ADC on TMP sources,
 * arch CF reading checked.
 *
 * Setup arch CF=1 before vmwrite, load a/b into TMP1/TMP2 inside the
 * patch, ADC into TMP0, write back to RAX.
 *
 * Build: make PROG=simple_ADC_tmp
 * Run:   sudo taskset -c 0 ./simple_ADC_tmp_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* Patch:
 *   T0: TMP1 = R9 ;  TMP2 = R10   (load operands into TMPs)
 *   T1: TMP0 = TMP1 + TMP2 + arch_CF
 *   T2: RAX  = TMP0               (writeback)
 *   T3: END
 */
static void install(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ZEROEXT_DSZ64_DR(R64SRC, R9),
          ZEROEXT_DSZ64_DR(R64SRC, R10),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP0, R64SRC, R64SRC),
          NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP0),
          NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Fire with arch CF set to `cf_in`. R9=a, R10=b. RAX comes back as result. */
static uint64_t fire(uint64_t a, uint64_t b, int cf_in) {
    uint64_t res;
    uint64_t flags = cf_in ? 0x3ULL : 0x2ULL;   /* bit1 always set, bit0=CF */
    asm volatile(
        "push %[flg]\n\t"
        "popfq\n\t"
        "mov r9,  %[a]\n\t"
        "mov r10, %[b]\n\t"
        "xor eax, eax\n\t"
        "xor ecx, ecx\n\t"
        "xor edx, edx\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %[res], rax\n\t"
        : [res] "=r"(res)
        : [a] "r"(a), [b] "r"(b), [flg] "r"(flags)
        : "rax", "rcx", "rdx", "r9", "r10", "cc", "memory"
    );
    return res;
}

int main(void) {
    printf("============================================================\n");
    printf("  Simplest ADC test, TMP-register sources\n");
    printf("  Patch: TMP1=R9; TMP2=R10; ADC TMP0=TMP1+TMP2+arch_CF; RAX=TMP0\n");
    printf("============================================================\n\n");

    assign_to_core(0);
    install();

    struct { uint64_t a, b; int cf; uint64_t expect; const char *label; } cases[] = {
        { 5, 3, 0, 8, "CF=0:  5+3       = 8" },
        { 5, 3, 1, 9, "CF=1:  5+3+1     = 9" },
        { 0, 0, 1, 1, "CF=1:  0+0+1     = 1  (proves CF is added)" },
        { 0, 0, 0, 0, "CF=0:  0+0+0     = 0  (sanity)" },
        { 0xFFFFFFFFFFFFFFFFULL, 0, 1, 0,
                                  "CF=1:  -1+0+1    = 0  (wraps; lo only)" },
    };

    int pass = 0, total = 0;
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint64_t got = fire(cases[i].a, cases[i].b, cases[i].cf);
        int ok = got == cases[i].expect;
        printf("  %-55s  got=%" PRIu64 "  %s\n",
               cases[i].label, got, ok ? "PASS" : "FAIL");
        pass += ok; total++;
    }

    printf("\n");
    printf("============================================================\n");
    if (pass == total) {
        printf("  All %d/%d passed → ADC on TMP sources also reads arch CF\n",
               pass, total);
        printf("  set at patch entry. (Source register type does not gate\n");
        printf("  ADC's flag-domain read.)\n");
    } else {
        printf("  %d/%d failed → ADC behaves differently on TMP sources.\n",
               total - pass, total);
        printf("  Compare each row with simple_ADC's output to localize.\n");
    }
    printf("============================================================\n");
    return 0;
}
