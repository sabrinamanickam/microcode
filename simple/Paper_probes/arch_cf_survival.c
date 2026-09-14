/*
 * arch_cf_survival.c — which $\mu$ops are TRANSPARENT to the bridged arch CF?
 *
 * Gating experiment for restructuring the saturated 4x64 fe_mul.
 *
 * WHY THIS MATTERS
 * ----------------
 * asm_op_curve25519_solinas_mul_chained_v3.c is built as two separate blocks
 * per schoolbook row:
 *      MUL_BLOCK       5 triads — 4 MULs, all 8 lo/hi halves parked in TMP0..7
 *      COMBINED_CHAIN  6 triads — one 10-hop ADC/GENARITHFLAGS_RR carry chain
 * Both blocks are at 100% slot occupancy, so neither shrinks on its own.
 *
 * The production 5x51 patches do NOT have this shape. They use "progressive
 * accumulation": each MAC's lo/hi is folded into the accumulator in the SAME
 * triad as the next MUL, so the products are never parked in TMPs at all
 * (2 triads per MAC, no NOPs). Applying that to the 4x64 patch would merge
 * 5+6 = 11 triads per row into ~8-9, and would free TMP0..7 — which is what
 * currently forces the 2-triad-per-row TMP->arch->TMP writeback round trip,
 * since all 16 TMPs are committed.
 *
 * The reason v3 was not built that way is that its carry chain runs through
 * the architectural CF, which is a single global resource: every ADC must see
 * the CF published by the GENARITHFLAGS_RR belonging to its predecessor. To
 * interleave MULs into that chain, the intervening $\mu$ops must leave the
 * bridged arch CF alone.
 *
 * We know an intervening ADD is safe: v3's own COMBINED_CHAIN T3 places
 * ADD(TMP0, R15, TMP0) between an ADC and a GENARITHFLAGS_RR and verifies over
 * 10,007 random pairs, and carrystate_scope.c Group D shows an ADD+SETCC chain
 * leaves arch CF at its entry value. (v3's comment at that triad claims "slot 1
 * ADD overwrites arch CF" — that claim is wrong, though the code is correct.)
 *
 * What is NOT known is whether a MUL is transparent. EXPERIMENTS.md's "MUL
 * before ADD+SETCC — Safe: MUL does NOT poison the internal flag domain" is
 * about domain #1, not about arch CF, and carrystate_scope.c E2d only shows a
 * MUL replaces the condition state of a register it WRITES. Nothing tests MUL
 * against the bridged arch CF. If MUL is transparent, the interleaved 4x64
 * schedule is available and worth roughly 8-12 triads. If it is not, v3's
 * two-block shape is forced and the saturated patch is at its structural floor.
 *
 * METHOD
 * ------
 * For each candidate op X:
 *      T0: ADD TMP0 = R8 + R9          (sets TMP0's condition state)
 *      T1: GENARITHFLAGS_RR(TMP0,TMP0) (publishes it to arch CF)
 *      T2: X                            (the candidate)
 *      T3: ADC RAX = TMP9 + TMP9 + CF   (TMP9 = 0, so RAX == arch CF)
 *
 * Run twice per candidate, and BOTH directions must hold for transparency:
 *      bridge publishes 1, entry CF = 0  -> RAX must be 1  (X must not clear)
 *      bridge publishes 0, entry CF = 1  -> RAX must be 0  (X must not set,
 *                                          and must not restore the entry CF)
 * A NOP control pins both directions first.
 *
 * Build: make PROG=arch_cf_survival
 * Run:   sudo taskset -c 0 ./arch_cf_survival_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

static int g_pass = 0, g_fail = 0;

/*  buf[0..5] = R8,R9,R10,R11,R12,R13 ; buf[6] = rflags ; buf[7] = RAX  */
static uint64_t fire(const uint64_t in[6], int cf_in) {
    uint64_t buf[9];
    for (int i = 0; i < 6; i++) buf[i] = in[i];
    buf[6] = cf_in ? 0x3ULL : 0x2ULL;
    asm volatile(
        "mov  r8,  qword ptr [%[bp] + 0]\n\t"
        "mov  r9,  qword ptr [%[bp] + 8]\n\t"
        "mov  r10, qword ptr [%[bp] + 16]\n\t"
        "mov  r11, qword ptr [%[bp] + 24]\n\t"
        "mov  r12, qword ptr [%[bp] + 32]\n\t"
        "mov  r13, qword ptr [%[bp] + 40]\n\t"
        "xor  rax, rax\n\t"
        "xor  rbx, rbx\n\t"
        "xor  rcx, rcx\n\t"
        "xor  rdx, rdx\n\t"
        "push qword ptr [%[bp] + 48]\n\t"
        "popfq\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov  qword ptr [%[bp] + 56], rax\n\t"
        :
        : [bp] "r"(buf)
        : "rax", "rbx", "rcx", "rdx",
          "r8", "r9", "r10", "r11", "r12", "r13", "cc", "memory"
    );
    return buf[7];
}

/* Build the 4-triad probe around one candidate op. TMP9 is held at 0. */
static void install_with(uint64_t candidate) {
    ucode_t p[] = {
        { ZEROEXT_DSZ32_DI(TMP9, 0), ADD_DSZ64_DRR(TMP0, R8, R9),
          NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(TMP0, TMP0), NOP, NOP, NOP_SEQWORD },
        { candidate, NOP, NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, TMP9, TMP9), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* R8,R9 choose what the bridge publishes; R10..R13 feed the candidate op. */
#define CARRY_A 0xFFFFFFFFFFFFFFFEULL   /* FF..FE + 3 -> CF=1, sum 1 */
#define CARRY_B 3ULL
#define NOCAR_A 1ULL
#define NOCAR_B 1ULL                    /* 1 + 1      -> CF=0, sum 2 */

static void candidate(const char *name, uint64_t op) {
    install_with(op);
    /* R10=7, R11=9, R12=0x8000000000000000, R13=2 give the candidate ops
     * non-trivial operands (and a shift that pushes a bit out). */
    uint64_t hi[6] = { CARRY_A, CARRY_B, 7, 9, 0x8000000000000000ULL, 2 };
    uint64_t lo[6] = { NOCAR_A, NOCAR_B, 7, 9, 0x8000000000000000ULL, 2 };

    uint64_t got1 = fire(hi, 0);   /* bridged CF = 1, entry CF = 0 */
    uint64_t got0 = fire(lo, 1);   /* bridged CF = 0, entry CF = 1 */

    int ok = (got1 == 1) && (got0 == 0);
    if (ok) g_pass++; else g_fail++;
    printf("    [%s] %-46s  bridged1/entry0 -> %" PRIu64
           "   bridged0/entry1 -> %" PRIu64 "   %s\n",
           ok ? "PASS" : "FAIL", name, got1, got0,
           ok ? "transparent" : "DISTURBS arch CF");
}

int main(void) {
    printf("================================================================\n");
    printf("  arch_cf_survival.c — what may sit between GENARITHFLAGS_RR\n");
    printf("  and the ADC that consumes the bridged carry?\n");
    printf("  Goldmont (Celeron N3350), vmwrite (0x0cd8) hook -> U7c00\n");
    printf("================================================================\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    printf("\n  control — nothing between the bridge and the ADC:\n");
    candidate("NOP", NOP);

    printf("\n  known-safe reference (v3 relies on this one):\n");
    candidate("ADD_DSZ64_DRR(TMP1, R10, R11)",  ADD_DSZ64_DRR(TMP1, R10, R11));

    printf("\n  the question that gates an interleaved 4x64 schedule:\n");
    candidate("MUL_DSZ64_DRR(TMP1, R10, R11)",  MUL_DSZ64_DRR(TMP1, R10, R11));
    candidate("MUL_DSZ64_DIR(TMP1, 38, R10)",   MUL_DSZ64_DIR(TMP1, 38, R10));

    printf("\n  the rest of the ops an interleaved schedule would need:\n");
    candidate("ZEROEXT_DSZ64_DR(TMP1, R10)",    ZEROEXT_DSZ64_DR(TMP1, R10));
    candidate("ZEROEXT_DSZ64_DR(RCX, TMP0)",    ZEROEXT_DSZ64_DR(RCX, TMP0));
    candidate("SHL_DSZ64_DRI(TMP1, R12, 1)",    SHL_DSZ64_DRI(TMP1, R12, 1));
    candidate("SHR_DSZ64_DRI(TMP1, R10, 1)",    SHR_DSZ64_DRI(TMP1, R10, 1));
    candidate("OR_DSZ64_DRR(TMP1, R10, R11)",   OR_DSZ64_DRR(TMP1, R10, R11));
    candidate("SETCC_CONDB_DR(TMP1, TMP0)",     SETCC_CONDB_DR(TMP1, TMP0));
    candidate("ADD_DSZ64_DRR(R14, R10, R11)",   ADD_DSZ64_DRR(R14, R10, R11));
    candidate("ADC_DSZ64_DRR(TMP1, R10, R11)",  ADC_DSZ64_DRR(TMP1, R10, R11));

    init_match_and_patch();
    do_fix_IN_patch();

    printf("\n================================================================\n");
    printf("  %d transparent, %d disturb arch CF\n", g_pass, g_fail);
    printf("  A transparent MUL means the 4x64 patch can interleave its\n");
    printf("  multiplies into the ADC chain (progressive accumulation);\n");
    printf("  a disturbing MUL means v3's two-block shape is forced.\n");
    printf("================================================================\n");
    return 0;
}
