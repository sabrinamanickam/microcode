/*
 * test_movetocreg_eflags.c — Can MOVETOCREG write arch EFLAGS directly?
 *
 * GENARITHFLAGS_R is broken as a CF bridge (it only writes arch CF=1
 * when ADD wraps to exactly 0). But there's another candidate:
 *   CORE_CR_EFLAGS = 0x7fe   (defined in opcode.h)
 *   MOVETOCREG_DSZ64_RI(src, imm)   could write src into arch EFLAGS
 *   if imm == CORE_CR_EFLAGS.
 *
 * If this works, the bridge is:
 *   ADD TMP0 = a+b
 *   SETCC_CONDB_DR(TMP_c, TMP0)        // TMP_c = 0 or 1, real CF
 *   MOVETOCREG_DSZ64_RI(TMP_c, CORE_CR_EFLAGS)   // arch EFLAGS = TMP_c
 *   ADC                                 // reads arch CF (bit 0 of EFLAGS)
 *
 * Even if this works, it's 3 ops per carry-propagation step — same cost
 * as the SETCC dance. But it lets us use ADC directly, which CAN merge
 * carry into the next limb's add in 1 micro-op instead of 2.
 *
 * Build: make PROG=test_movetocreg_eflags
 * Run:   sudo taskset -c 0 ./test_movetocreg_eflags_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"
#include "opcode.h"   /* for CORE_CR_EFLAGS */

/* Fire patch with R9=a, R10=b. ADC writes result to RAX.
 * arch CF forced to 0 on entry. */
static uint64_t fire(uint64_t a, uint64_t b) {
    uint64_t res;
    asm volatile(
        "push 2\n\t"           /* CF=0 */
        "popfq\n\t"
        "mov r9,  %[a]\n\t"
        "mov r10, %[b]\n\t"
        "xor eax, eax\n\t"
        "xor ecx, ecx\n\t"
        "xor edx, edx\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov %[res], rax\n\t"
        : [res] "=r"(res)
        : [a] "r"(a), [b] "r"(b)
        : "rax", "rcx", "rdx", "r9", "r10", "cc", "memory"
    );
    return res;
}

static void install_writeeflags(void) {
    /* Pattern (4 triads):
     *   T0: ADD TMP0 = R9 + R10            (sets TMP-CF for TMP0)
     *   T1: SETCC_CONDB_DR(TMP1, TMP0)     (TMP1 = real CF, 0 or 1)
     *   T2: MOVETOCREG_DSZ64_RI(TMP1, CORE_CR_EFLAGS)
     *       (writes TMP1 into arch EFLAGS — CF in bit 0)
     *   T3: ADC RAX = RAX + RCX            (RCX=0, RAX=0; result = arch CF)
     */
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R9, R10), NOP, NOP, NOP_SEQWORD },
        { SETCC_CONDB_DR(TMP1, TMP0),   NOP, NOP, NOP_SEQWORD },
        { MOVETOCREG_DSZ64_RI(TMP1, CORE_CR_EFLAGS), NOP, NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, RAX, RCX), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

typedef struct {
    uint64_t a, b;
    int true_cf;
    const char *label;
} probe_t;

static probe_t probes[] = {
    { 1, 1,                                          0, "no overflow" },
    { 0xFFFFFFFFFFFFFFFFULL, 1,                     1, "overflow, TMP=0" },
    { 0xFFFFFFFFFFFFFFFEULL, 3,                     1, "overflow, TMP=1 (DISCRIMINATOR)" },
    { 0xDEADBEEF12345678ULL, 0xCAFEFEEDABCDEF01ULL, 1, "overflow, TMP=random (DISCRIMINATOR)" },
    { 0,                     0,                     0, "zero+zero" },
};

int main(void) {
    printf("============================================================\n");
    printf("  MOVETOCREG → CORE_CR_EFLAGS as CF bridge\n");
    printf("============================================================\n\n");

    assign_to_core(0);
    install_writeeflags();

    printf("Pattern: ADD ; SETCC ; MOVETOCREG(EFLAGS) ; ADC rax = 0 + 0\n");
    printf("RAX_out = arch CF that ADC reads.\n\n");

    int all_correct = 1;
    for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
        uint64_t got = fire(probes[i].a, probes[i].b);
        int ok = (int)got == probes[i].true_cf;
        if (!ok) all_correct = 0;
        printf("  %-45s  expect CF=%d  got=%" PRIu64 "  %s\n",
               probes[i].label, probes[i].true_cf, got,
               ok ? "PASS" : "FAIL");
    }

    printf("\n");
    printf("============================================================\n");
    if (all_correct) {
        printf("  MOVETOCREG → EFLAGS IS a real CF bridge.\n");
        printf("  Bridge cost: ADD + SETCC + MOVETOCREG = 3 ops per limb.\n");
        printf("  Versus SETCC dance: ADD + SETCC + ADD-carry = 3 ops per limb.\n");
        printf("  Same cost — no win for switching to ADC chains.\n");
    } else {
        printf("  MOVETOCREG → EFLAGS does NOT work as a CF bridge either.\n");
        printf("  No known way to push TMP-CF into arch CF reliably.\n");
        printf("  → 4x64 microcode via ADC is NOT viable.\n");
    }
    printf("============================================================\n");
    return 0;
}
