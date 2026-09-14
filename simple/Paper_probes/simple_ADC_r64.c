/*
 * simple_ADC_r64.c — ADC test using R64SRC/R64DST operand encodings.
 *
 * R64DST=0x01 and R64SRC=0x02 (opcode.h:69-70) are not literal register
 * IDs — they are placeholders that the hardware substitutes with the
 * operand registers of the triggering macroinstruction. For our hook
 *   vmwrite %rbx, %rcx
 * R64SRC resolves to RCX, R64DST resolves to RBX (or vice-versa
 * depending on convention).
 *
 * Per `experiments/multest.c`, ops written against R64SRC/R64DST do
 * function — that file uses MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST)
 * successfully. So the encoding is plumbed through dispatch.
 *
 * Hypothesis to test: maybe ADC's flag-domain read depends on operand
 * encoding type — perhaps when operands are R64SRC/R64DST (the
 * "incoming macroinstruction operand" path), ADC reads a different
 * flag source than when operands are explicit arch reg IDs like
 * RBX (0x23) or TMP IDs.
 *
 * Test setup:
 *   asm: push CF=1, mov rbx=5, mov rcx=3, vmwrite rbx, rcx
 *   so R64SRC, R64DST resolve to two of {RBX=5, RCX=3}
 *   patch runs one ADC variant with CF=1 incoming
 *   read rbx, rcx, rax back, identify whether +1 was added
 *
 * Build: make PROG=simple_ADC_r64
 * Run:   sudo taskset -c 0 ./simple_ADC_r64_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* Each install_X variants the ADC operand encoding. */

static void install_dst_src_dst(void) {
    /* ADC R64DST = R64SRC + R64DST  */
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADC_DSZ64_DRR(R64DST, R64SRC, R64DST), 
          NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_src_src_dst(void) {
    /* ADC R64SRC = R64SRC + R64DST  */
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADC_DSZ64_DRR(R64SRC, R64SRC, R64DST), 
          NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_dst_dst_src(void) {
    /* ADC R64DST = R64DST + R64SRC */
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADC_DSZ64_DRR(R64DST, R64DST, R64SRC), 
          NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_src_dst_src(void) {
    /* ADC R64SRC = R64DST + R64SRC */
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADC_DSZ64_DRR(R64DST, R64SRC, R64SRC), 
          NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_tmp_src_dst(void) {
    /* ADC TMP0 = R64SRC + R64DST ; write TMP0 → RAX so we can read it */
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADC_DSZ64_DRR(TMP0, R64SRC, R64DST), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_tmp_dst_src(void) {
    /* ADC TMP0 = R64DST + R64SRC ; write TMP0 → RAX */
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADC_DSZ64_DRR(TMP0, R64DST, R64SRC), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP0), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Baselines using explicit RBX/RCX (arch reg IDs 0x23/0x21). */
static void install_rbx_rbx_rcx(void) {
    /* ADC RBX = RBX + RCX  */
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADC_DSZ64_DRR(RBX, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

static void install_rcx_rbx_rcx(void) {
    /* ADC RCX = RBX + RCX  */
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADC_DSZ64_DRR(RCX, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Fire with RBX=a, RCX=b, arch CF=cf_in.  Returns (RBX, RCX, RAX) so each
 * variant can report whichever register the ADC wrote to.
 *
 * Sequence: load operand registers FIRST (mov doesn't touch flags),
 * then popfq sets CF as the LAST thing before vmwrite. The patch's
 * own instructions overwrite RAX, so no pre-zeroing needed. */
static void fire(uint64_t a, uint64_t b, int cf_in,
                 uint64_t *out_rbx, uint64_t *out_rcx, uint64_t *out_rax) {
    uint64_t flags = cf_in ? 0x3ULL : 0x2ULL;
    asm volatile(
        "mov rbx, %[a]\n\t"          /* operand loads — flag-neutral */
        "mov rcx, %[b]\n\t"
        "push %[flg]\n\t"            /* set arch flags JUST before vmwrite */
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"       /* hook fires with arch CF = cf_in */
        "mov %[rbx], rbx\n\t"
        "mov %[rcx], rcx\n\t"
        "mov %[rax], rax\n\t"
        : [rbx] "=&r"(*out_rbx),
          [rcx] "=&r"(*out_rcx),
          [rax] "=&r"(*out_rax)
        : [a] "r"(a), [b] "r"(b), [flg] "r"(flags)
        : "rax", "rbx", "rcx", "rdx", "cc", "memory"
    );
}

/* CONTROL: pure x86 hardware adc, NO microcode hook involved.
 *
 * Validates that our test harness actually sets CF=1 before the
 * arithmetic. If this control returns 9, the harness is correct and
 * any microcode-side failure is genuinely a microcode behaviour
 * problem. If this control also fails to see CF, the harness (popfq,
 * register loads, calling-convention clobbers) is broken and all
 * microcode results are suspect. */
static uint64_t fire_x86_control(uint64_t a, uint64_t b, int cf_in) {
    uint64_t result;
    uint64_t flags = cf_in ? 0x3ULL : 0x2ULL;
    asm volatile(
        "push %[flg]\n\t"
        "popfq\n\t"
        "mov  rbx, %[a]\n\t"
        "mov  rcx, %[b]\n\t"
        "adc  rbx, rcx\n\t"        /* HARDWARE ADC — no vmwrite, no hook */
        "mov  %[res], rbx\n\t"
        : [res] "=r"(result)
        : [a] "r"(a), [b] "r"(b), [flg] "r"(flags)
        : "rbx", "rcx", "cc", "memory"
    );
    return result;
}

typedef void (*install_fn)(void);

typedef struct {
    const char *name;
    install_fn install;
    /* Where the ADC's destination ends up: 'B'=RBX, 'C'=RCX, 'A'=RAX (from TMP). */
    char dst_reg;
} variant_t;

static variant_t variants[] = {
    { "ADC R64DST = R64SRC + R64DST",     install_dst_src_dst, 'B' },
    { "ADC R64SRC = R64SRC + R64DST",     install_src_src_dst, 'C' },
    { "ADC R64DST = R64DST + R64SRC",     install_dst_dst_src, 'B' },
    { "ADC R64SRC = R64DST + R64SRC",     install_src_dst_src, 'C' },
    { "ADC TMP0   = R64SRC + R64DST  (→RAX)", install_tmp_src_dst, 'A' },
    { "ADC TMP0   = R64DST + R64SRC  (→RAX)", install_tmp_dst_src, 'A' },
    { "ADC RBX    = RBX    + RCX     (baseline)", install_rbx_rbx_rcx, 'B' },
    { "ADC RCX    = RBX    + RCX     (baseline)", install_rcx_rbx_rcx, 'C' },
};

int main(void) {
    printf("============================================================\n");
    printf("  ADC with R64SRC/R64DST operand encodings\n");
    printf("  vmwrite rbx, rcx → R64DST ↔ rbx, R64SRC ↔ rcx (or vice versa)\n");
    printf("============================================================\n\n");
    printf("  Test: RBX=5, RCX=3, arch CF=1.  ADC computes 5+3+CF.\n");
    printf("        If CF was read, result = 9.  If CF was lost, result = 8.\n");
    printf("        If operand mapping reversed, also fine — same math.\n\n");

    assign_to_core(0);

    /* CONTROL ROW: hardware x86 adc with CF=1.
     * If this prints 9, the test harness is correct (popfq does set CF). */
    {
        uint64_t ctrl1 = fire_x86_control(5, 3, 1);
        uint64_t ctrl0 = fire_x86_control(5, 3, 0);
        printf("  CONTROL  hardware x86 ADC  RBX=5,RCX=3,CF=1 → %" PRIu64
               "   (expect 9 if harness sets CF)\n", ctrl1);
        printf("  CONTROL  hardware x86 ADC  RBX=5,RCX=3,CF=0 → %" PRIu64
               "   (expect 8 baseline)\n", ctrl0);
        if (ctrl1 != 9 || ctrl0 != 8) {
            printf("\n  ⚠  Harness is BROKEN — hardware ADC didn't see the CF we set.\n");
            printf("     All microcode rows below are meaningless. Fix popfq/clobbers first.\n\n");
        } else {
            printf("  → Harness confirmed correct. Microcode results below are real.\n\n");
        }
    }

    /* Print all registers in decimal and hex. Don't try to be clever
     * about a "verdict" — just show what every register contains so the
     * caller can read the actual ADC semantics from the data. */
    printf("  Inputs to each row:  RBX=5 (0x05)  RCX=3 (0x03)  arch CF=1\n");
    printf("  Reading: 8=no-CF, 9=CF-seen, 6=R64SRC+R64SRC, 10=R64SRC+R64SRC+CF, etc.\n\n");

    printf("  %-46s  %-26s  %-26s  %-26s\n",
           "variant",
           "RBX after",
           "RCX after",
           "RAX after");
    printf("  %-46s  %-26s  %-26s  %-26s\n",
           "------------------------------------------",
           "--------------------------",
           "--------------------------",
           "--------------------------");

    for (size_t i = 0; i < sizeof(variants)/sizeof(variants[0]); i++) {
        variants[i].install();
        uint64_t rbx = 0, rcx = 0, rax = 0;
        fire(5, 3, 1, &rbx, &rcx, &rax);
        printf("  %-46s  %4" PRIu64 " (0x%016" PRIx64 ")  %4" PRIu64
               " (0x%016" PRIx64 ")  %4" PRIu64 " (0x%016" PRIx64 ")\n",
               variants[i].name,
               rbx, rbx, rcx, rcx, rax, rax);
    }

    printf("\n");
    printf("============================================================\n");
    printf("  If ANY variant returns 9, that encoding reads arch CF.\n");
    printf("  If all return 8, ADC ignores CF regardless of operand encoding\n");
    printf("    AND the prior simple_ADC.c test was wrong/lying.\n");
    printf("  If all return 8 but simple_ADC.c got 9, then ADC's CF-read\n");
    printf("    is conditional on something we haven't isolated yet.\n");
    printf("============================================================\n");
    return 0;
}
