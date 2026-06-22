/*
 * simple_ADC.c — Simplest possible ADC test on Goldmont microcode.
 *
 * Setup arch CF=1 before vmwrite, fire a patch whose only instruction
 * is ADC r, a, b. Does ADC see CF=1?
 *
 * Build: make PROG=simple_ADC
 * Run:   sudo taskset -c 0 ./simple_ADC_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* Patch:
 *   T0: RAX = R9 + R10 + arch_CF
 *   T1: END
 * The 'rcx, rdx' operands of vmwrite are arbitrary — we just need
 * the vmwrite to fire the hook. */
static void install(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADC_DSZ64_DRR(RAX, R9, R10), NOP, NOP, NOP_SEQWORD },
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
    printf("  Simplest possible ADC test\n");
    printf("  Patch is one micro-op: ADC RAX = R9 + R10 + arch_CF\n");
    printf("============================================================\n\n");

    assign_to_core(0);
    install();

    /* Three probes — clearly distinguish "ADC sees CF" from "ADC ignores CF". */
    struct { uint64_t a, b; int cf; uint64_t expect; const char *label; } cases[] = {
        { 5, 3, 0, 8, "CF=0:  5+3       = 8" },
        { 5, 3, 1, 9, "CF=1:  5+3+1     = 9" },
        { 0, 0, 1, 1, "CF=1:  0+0+1     = 1  (proves CF is added, not lost)" },
        { 0, 0, 0, 0, "CF=0:  0+0+0     = 0  (sanity)" },
        { 0xFFFFFFFFFFFFFFFFULL, 0, 1, 0,
                                  "CF=1:  -1+0+1    = 0  (wraps; lo result only)" },
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
        printf("  All %d/%d passed → ADC correctly reads arch CF set at patch entry.\n",
               pass, total);
    } else {
        printf("  %d/%d failed → ADC does not read arch CF as expected.\n",
               total - pass, total);
    }
    printf("============================================================\n");
    return 0;
}
