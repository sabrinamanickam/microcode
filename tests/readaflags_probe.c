/*
 * readaflags_probe.c — What does READAFLAGS actually read?
 *
 * The micro-op READAFLAGS_DR(dst, src) (opcode 0x380, inst.h:120) reads
 * arch flags into `dst`. The `src` operand is unclear. Used in
 * `Sabrina/experiments/flag_probes.c` probe 6 but with limited probes.
 *
 * Key question: does READAFLAGS read…
 *   (a) the dispatch-time snapshot that ADC reads (frozen at patch entry)?
 *   (b) a live arch RFLAGS that ADD/ADC update as they go?
 *   (c) the same shadow that GENARITHFLAGS_R writes to (where it sometimes
 *       has the (TMP==0 AND TMP-CF==1) bit)?
 *   (d) yet another flag source we don't know about?
 *
 * If (b) or (c), READAFLAGS might let us *observe* a real carry, even if
 * we can't *push* one into ADC's port.
 *
 * Five probes, each running:
 *   <setup> ; READAFLAGS TMP1 ; ZEROEXT RBX = TMP1
 * and reading RBX back. RBX's bit 0 = CF that READAFLAGS observed.
 *
 *   P0  No setup — just READAFLAGS at patch start.
 *       Probe of dispatch snapshot vs entry flags.
 *
 *   P1  ADD TMP0 = R64DST + R64SRC (overflow) ; READAFLAGS
 *       Does READAFLAGS see ADD's CF-out?
 *
 *   P2  Plain MOVETOCREG_DSZ64_RI(TMP1=1, CORE_CR_EFLAGS) ; READAFLAGS
 *       Does the explicit EFLAGS write show up in READAFLAGS?
 *
 *   P3  MOVETOCREG_OR(TMP1=1, EFLAGS) ; READAFLAGS
 *       Does RMW write show up?
 *
 *   P4  ADD TMP0 = R64DST + R64SRC (overflow) ;
 *       GENARITHFLAGS_R(TMP0) ; READAFLAGS
 *       Does GENARITHFLAGS_R's write show up in arch flags
 *       (vs. some unrelated shadow)?
 *
 * Inputs (every probe): RBX=0xFFFFFFFFFFFFFFFF, RCX=1, entry arch CF=0.
 *
 * Build: make PROG=readaflags_probe
 * Run:   sudo taskset -c 0 ./readaflags_probe_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

#define _MOVETOCREG_OR_DSZ64  (0x902UL << 32)
#define MTC_OR(src, imm)      (_MOVETOCREG_OR_DSZ64 | INSTR_RI(src, imm) | MOD2)

/* P0 — just READAFLAGS at the very start, before any other ops. */
static void install_P0(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { READAFLAGS_DR(TMP1, TMP1), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RBX, TMP1), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* P1 — ADD overflow then READAFLAGS. */
static void install_P1(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R64DST, R64SRC), NOP, NOP, NOP_SEQWORD },
        { READAFLAGS_DR(TMP1, TMP1), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RBX, TMP1), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* P2 — plain MOVETOCREG TMP=1 to EFLAGS, then READAFLAGS. */
static void install_P2(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ZEROEXT_DSZ32_DI(TMP2, 1), NOP, NOP, NOP_SEQWORD },
        { MOVETOCREG_DSZ64_RI(TMP2, CORE_CR_EFLAGS), NOP, NOP, NOP_SEQWORD },
        { READAFLAGS_DR(TMP1, TMP1), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RBX, TMP1), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* P3 — MOVETOCREG_OR with TMP=1 (set CF), then READAFLAGS. */
static void install_P3(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ZEROEXT_DSZ32_DI(TMP2, 1), NOP, NOP, NOP_SEQWORD },
        { MTC_OR(TMP2, CORE_CR_EFLAGS), NOP, NOP, NOP_SEQWORD },
        { READAFLAGS_DR(TMP1, TMP1), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RBX, TMP1), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* P4 — ADD overflow ; GENARITHFLAGS_R(TMP0) ; READAFLAGS. */
static void install_P4(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R64DST, R64SRC), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_R(TMP0), NOP, NOP, NOP_SEQWORD },
        { READAFLAGS_DR(TMP1, TMP1), NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RBX, TMP1), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Fire: RBX=0xFFFFFFFFFFFFFFFF, RCX=1, entry CF=cf_in. Return RBX after. */
static uint64_t fire(int cf_in) {
    uint64_t res;
    uint64_t flags = cf_in ? 0x3ULL : 0x2ULL;
    asm volatile(
        "mov  rbx, 0xFFFFFFFFFFFFFFFF\n\t"
        "mov  rcx, 1\n\t"
        "push %[flg]\n\t"
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  %[res], rbx\n\t"
        : [res] "=r"(res)
        : [flg] "r"(flags)
        : "rax", "rbx", "rcx", "rdx", "cc", "memory"
    );
    return res;
}

static void run(const char *label, void (*install)(void), int cf_in) {
    install();
    uint64_t f = fire(cf_in);
    int cf_bit  = (int)(f >> 0)  & 1;
    int pf_bit  = (int)(f >> 2)  & 1;
    int zf_bit  = (int)(f >> 6)  & 1;
    int sf_bit  = (int)(f >> 7)  & 1;
    int of_bit  = (int)(f >> 11) & 1;
    printf("  %-60s\n", label);
    printf("    flags = 0x%016" PRIx64 "  (CF=%d  PF=%d  ZF=%d  SF=%d  OF=%d)  entry CF=%d\n\n",
           f, cf_bit, pf_bit, zf_bit, sf_bit, of_bit, cf_in);
}

int main(void) {
    printf("============================================================\n");
    printf("  READAFLAGS probe: what does the arch-flags reader see?\n");
    printf("============================================================\n\n");
    printf("  Inputs: RBX=0xFFFF…FFFF, RCX=1\n");
    printf("  Output flags interpreted: CF=bit0, PF=bit2, ZF=bit6, SF=bit7, OF=bit11\n\n");

    assign_to_core(0);

    run("P0  bare READAFLAGS (no setup)  entry CF=0",  install_P0, 0);
    run("P0  bare READAFLAGS (no setup)  entry CF=1",  install_P0, 1);

    run("P1  ADD overflow ; READAFLAGS                    entry CF=0",  install_P1, 0);
    run("P2  MOVETOCREG TMP=1, EFLAGS ; READAFLAGS         entry CF=0",  install_P2, 0);
    run("P3  MOVETOCREG_OR TMP=1, EFLAGS ; READAFLAGS      entry CF=0",  install_P3, 0);
    run("P4  ADD overflow ; GENARITHFLAGS_R ; READAFLAGS   entry CF=0",  install_P4, 0);

    printf("============================================================\n");
    printf("  Interpretation:\n");
    printf("    P0: should mirror entry flags (CF=0 then CF=1) — sanity baseline\n");
    printf("    P1: if CF=1, READAFLAGS sees live ADD carry → real bridge candidate\n");
    printf("    P2/P3: if CF=1, MOVETOCREG can update live arch flags (even if\n");
    printf("           ADC doesn't see it — meaning two separate flag registers)\n");
    printf("    P4: if CF=1, GENARITHFLAGS_R wrote arch flags (but recall ADC\n");
    printf("        doesn't see it for the TMP≠0 case)\n");
    printf("============================================================\n");
    return 0;
}
