/*
 * cr_cf_probe.c — Can any MOVETOCREG variant flip arch CF mid-patch?
 *
 * Plain MOVETOCREG_DSZ64_RI(reg, CORE_CR_EFLAGS) was already tested and
 * couldn't change what ADC reads. But opcode.h has RMW variants:
 *
 *   _MOVETOCREG_AND_DSZ64   (0x822 << 32)   cr[imm] &= reg
 *   _MOVETOCREG_OR_DSZ64    (0x902 << 32)   cr[imm] |= reg
 *   _MOVETOCREG_BTS_DSZ64   (0x962 << 32)   set bit (reg) in cr[imm]
 *   _MOVETOCREG_BTR_DSZ64   (0xa62 << 32)   reset bit (reg) in cr[imm]
 *   _MOVETOCREG_SHL_DSZ64   (0x8a2 << 32)   cr[imm] <<= reg
 *   _MOVETOCREG_SHR_DSZ64   (0x9a2 << 32)   cr[imm] >>= reg
 *
 * (Names inferred from opcode mnemonic; semantics guessed by analogy
 * with x86 instructions of the same name.)
 *
 * Hypothesis: a read-modify-write of CORE_CR_EFLAGS might bypass
 * whatever shadow ADC reads from. In particular:
 *   - BTS reg=0, EFLAGS → set bit 0 (CF) of EFLAGS
 *   - OR  reg=1, EFLAGS → bit 0 of EFLAGS goes to 1
 *
 * Patch (per probe):
 *   T0: ZEROEXT TMP1 = imm                 (where imm depends on op)
 *   T1: <MOVETOCREG variant>(TMP1, CORE_CR_EFLAGS)
 *   T2: ADC RAX = RAX + RCX  (RAX=RCX=0; RAX_out = arch CF that ADC saw)
 *   T3: END
 *
 * Build: make PROG=cr_cf_probe
 * Run:   sudo taskset -c 0 ./cr_cf_probe_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* Raw opcode constants — these are in opcode.h but not exposed as macros. */
#define _MOVETOCREG_AND_DSZ64 (0x822UL << 32)
#define _MOVETOCREG_SHL_DSZ64 (0x8a2UL << 32)
#define _MOVETOCREG_OR_DSZ64  (0x902UL << 32)
#define _MOVETOCREG_BTS_DSZ64 (0x962UL << 32)
#define _MOVETOCREG_SHR_DSZ64 (0x9a2UL << 32)
#define _MOVETOCREG_BTR_DSZ64 (0xa62UL << 32)

/* Build a generic MOVETOCREG variant: src reg, imm CR id. */
#define MTC_AND(src, imm)  (_MOVETOCREG_AND_DSZ64 | INSTR_RI(src, imm) | MOD2)
#define MTC_SHL(src, imm)  (_MOVETOCREG_SHL_DSZ64 | INSTR_RI(src, imm) | MOD2)
#define MTC_OR(src, imm)   (_MOVETOCREG_OR_DSZ64  | INSTR_RI(src, imm) | MOD2)
#define MTC_BTS(src, imm)  (_MOVETOCREG_BTS_DSZ64 | INSTR_RI(src, imm) | MOD2)
#define MTC_SHR(src, imm)  (_MOVETOCREG_SHR_DSZ64 | INSTR_RI(src, imm) | MOD2)
#define MTC_BTR(src, imm)  (_MOVETOCREG_BTR_DSZ64 | INSTR_RI(src, imm) | MOD2)

/* Install: ZEROEXT TMP1=tmp_val ; (variant)(TMP1, CORE_CR_EFLAGS) ; ADC RAX=0+0+CF. */
static void install(unsigned long variant_op, unsigned long tmp_val) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ZEROEXT_DSZ32_DI(TMP1, tmp_val), NOP, NOP, NOP_SEQWORD },
        { variant_op, NOP, NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, RAX, RCX), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Fire with arch CF=cf_in, RAX=0, RCX=0. */
static uint64_t fire(int cf_in) {
    uint64_t res;
    uint64_t flags = cf_in ? 0x3ULL : 0x2ULL;
    asm volatile(
        "xor  rax, rax\n\t"
        "xor  rcx, rcx\n\t"
        "mov  rbx, 1\n\t"
        "push %[flg]\n\t"
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  %[res], rax\n\t"
        : [res] "=r"(res)
        : [flg] "r"(flags)
        : "rax", "rbx", "rcx", "rdx", "cc", "memory"
    );
    return res;
}

int main(void) {
    printf("============================================================\n");
    printf("  MOVETOCREG RMW variants on CORE_CR_EFLAGS (0x7fe)\n");
    printf("============================================================\n\n");
    printf("  Pattern: TMP1=val ; <op>(TMP1, EFLAGS) ; ADC RAX = 0 + 0 + arch_CF\n");
    printf("  RAX_out = whatever arch CF ADC saw.\n\n");

    assign_to_core(0);

    struct {
        const char *name;
        unsigned long op;
        unsigned long tmp_val;
        int cf_in;
        int expect_high_means;   /* 1 if RAX=1 means the op set arch CF */
        const char *note;
    } probes[] = {
        /* Baseline rerun: plain MOVETOCREG, CF=0 entry, expect 0 (no bridge) */
        { "MOVETOCREG (plain) TMP1=1",  MOVETOCREG_DSZ64_RI(TMP1, CORE_CR_EFLAGS),
          1, 0, 1, "baseline: known to not bridge" },

        /* OR with 1 — sets bit 0 if it works */
        { "MOVETOCREG_OR  TMP1=1, EFLAGS",  MTC_OR(TMP1, CORE_CR_EFLAGS),
          1, 0, 1, "should set CF=1 if op writes EFLAGS" },

        /* OR with 0 — should be a no-op */
        { "MOVETOCREG_OR  TMP1=0, EFLAGS",  MTC_OR(TMP1, CORE_CR_EFLAGS),
          0, 0, 1, "no-op sanity check; entry CF=0 → expect RAX=0" },

        /* OR with 1, entry CF=1 — should keep CF=1 */
        { "MOVETOCREG_OR  TMP1=1, EFLAGS (entry CF=1)", MTC_OR(TMP1, CORE_CR_EFLAGS),
          1, 1, 0, "sanity: entry CF=1 already" },

        /* AND with 0 — should clear EFLAGS if it works */
        { "MOVETOCREG_AND TMP1=0, EFLAGS (entry CF=1)",  MTC_AND(TMP1, CORE_CR_EFLAGS),
          0, 1, 0, "if AND works, CF gets cleared → RAX=0" },

        /* AND with ~0 — should preserve EFLAGS */
        { "MOVETOCREG_AND TMP1=~0, EFLAGS (entry CF=1)", MTC_AND(TMP1, CORE_CR_EFLAGS),
          ~0UL, 1, 0, "AND preserves: expect RAX=1" },

        /* BTS bit 0 — set CF specifically */
        { "MOVETOCREG_BTS TMP1=0, EFLAGS",  MTC_BTS(TMP1, CORE_CR_EFLAGS),
          0, 0, 1, "BTS bit 0 = set CF" },

        /* BTS bit 0 with TMP1=0 means set bit 0 — that's CF */
        /* (already covered above) */

        /* BTR bit 0 — clear CF (entry CF=1) */
        { "MOVETOCREG_BTR TMP1=0, EFLAGS (entry CF=1)",  MTC_BTR(TMP1, CORE_CR_EFLAGS),
          0, 1, 0, "BTR bit 0 = clear CF; expect RAX=0 if it worked" },
    };

    for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
        install(probes[i].op, probes[i].tmp_val);
        uint64_t got = fire(probes[i].cf_in);
        printf("  %-50s  entry CF=%d  RAX=%" PRIu64 "  (%s)\n",
               probes[i].name, probes[i].cf_in, got, probes[i].note);
    }

    printf("\n");
    printf("============================================================\n");
    printf("  Key rows to watch:\n");
    printf("    MOVETOCREG_OR  TMP1=1, EFLAGS    : RAX should be 1 if it works\n");
    printf("    MOVETOCREG_BTS TMP1=0, EFLAGS    : RAX should be 1 if it works\n");
    printf("    MOVETOCREG_AND TMP1=0  (CF=1 in) : RAX should be 0 if it works\n");
    printf("    MOVETOCREG_BTR TMP1=0  (CF=1 in) : RAX should be 0 if it works\n");
    printf("============================================================\n");
    return 0;
}
