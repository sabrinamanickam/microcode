/*
 * genflagsrr_exhaustive.c — Try every placement of GENARITHFLAGS
 * relative to ADD and ADC. If any arrangement bridges TMP-CF to
 * the ADC carry-in port, it shows up here.
 *
 * Every probe uses the DISCRIMINATOR input:
 *   RBX = 0xFFFFFFFFFFFFFFFE
 *   RCX = 3
 *   entry arch CF = 0
 * → RBX+RCX = 1 with CF=1 (so TMP=1, not 0 — the case
 *   GENARITHFLAGS_R(TMP) is known to fail on).
 *
 * Patch sequence under test produces a final `ADC RAX = R8 + R9 + arch_CF`
 * with R8=R9=0, so RAX_out IS the arch CF that ADC observed.
 * RAX=1 means GENARITHFLAGS managed to put the carry in ADC's port.
 *
 * Build: make PROG=genflagsrr_exhaustive
 * Run:   sudo taskset -c 0 ./genflagsrr_exhaustive_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* Discriminator inputs */
#define A_IN  0xFFFFFFFFFFFFFFFEULL
#define B_IN  3ULL
/* Expected: A+B = 1 with CF=1 (overflow, but TMP≠0) */

static uint64_t fire(uint64_t a, uint64_t b) {
    uint64_t res;
    asm volatile(
        "mov  rbx, %[a]\n\t"
        "mov  rcx, %[b]\n\t"
        "xor  r8,  r8\n\t"
        "xor  r9,  r9\n\t"
        "xor  rax, rax\n\t"
        "push 2\n\t"
        "popfq\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  %[res], rax\n\t"
        : [res] "=r"(res)
        : [a] "r"(a), [b] "r"(b)
        : "rax", "rbx", "rcx", "rdx", "r8", "r9", "cc", "memory"
    );
    return res;
}

#define FINAL_ADC      ADC_DSZ64_DRR(RAX, R8, R9)
#define INSTALL(pat)   patch_ucode(0x7c00, pat, ARRAY_SZ(pat));            \
                       hook_match_and_patch(0, 0x0cd8, 0x7c00)
#define HEADER         init_match_and_patch(); do_fix_IN_patch()

/* ─────────────────── group A: same-triad arrangements ─────────────────── */

/* A1: ADD slot0 ; GENARITHFLAGS_RR(RBX,RCX) slot1 ; ADC slot2 */
static void install_A1(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX),
          GENARITHFLAGS_RR(RBX, RCX),
          FINAL_ADC, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* A2: ADD slot0 ; GENARITHFLAGS_R(TMP0) slot1 ; ADC slot2 */
static void install_A2(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX),
          GENARITHFLAGS_R(TMP0),
          FINAL_ADC, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* A3: GENARITHFLAGS slot0 ; ADD slot1 ; ADC slot2 (gen reads originals before ADD) */
static void install_A3(void) { HEADER;
    ucode_t p[] = {
        { GENARITHFLAGS_RR(RBX, RCX),
          ADD_DSZ64_DRR(TMP0, RBX, RCX),
          FINAL_ADC, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* A4: GENARITHFLAGS slot0 ; ADC slot1 ; ADD slot2 (gen first, no ADD needed for the bridge) */
static void install_A4(void) { HEADER;
    ucode_t p[] = {
        { GENARITHFLAGS_RR(RBX, RCX),
          FINAL_ADC,
          ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* A5: ADC slot0 ; GENARITHFLAGS slot1 ; ADD slot2 (reverse — sanity check) */
static void install_A5(void) { HEADER;
    ucode_t p[] = {
        { FINAL_ADC,
          GENARITHFLAGS_RR(RBX, RCX),
          ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* ─────────────────── group B: cross-triad arrangements ───────────────── */

/* B1: T0 ADD ; T1 GENARITHFLAGS_RR ; T2 ADC */
static void install_B1(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* B2: T0 ADD ; T1 GENARITHFLAGS_R(TMP0) ; T2 ADC */
static void install_B2(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_R(TMP0), NOP, NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* B3: T0 ADD+GENARITHFLAGS_RR(RBX,RCX) ; T1 ADC */
static void install_B3(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX),
          GENARITHFLAGS_RR(RBX, RCX),
          NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* B4: T0 ADD+GENARITHFLAGS_R(TMP0) ; T1 ADC */
static void install_B4(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX),
          GENARITHFLAGS_R(TMP0),
          NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* B5: T0 ADD ; T1 GENARITHFLAGS+ADC same triad */
static void install_B5(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(RBX, RCX), FINAL_ADC, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* B6: T0 GENARITHFLAGS_RR ; T1 ADD ; T2 ADC (gen first — bridge doesn't need ADD?) */
static void install_B6(void) { HEADER;
    ucode_t p[] = {
        { GENARITHFLAGS_RR(RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* B7: T0 GENARITHFLAGS_RR ; T1 ADC — no ADD at all */
static void install_B7(void) { HEADER;
    ucode_t p[] = {
        { GENARITHFLAGS_RR(RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* ─────────────────── group C: operand variants ───────────────────────── */

/* C1: TMP-only GENARITHFLAGS_RR(TMP0, TMP0) after duplicating the ADD result */
static void install_C1(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(TMP0, TMP0), NOP, NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* C2: GENARITHFLAGS_RR(TMP0, RCX) — mix TMP and arch reg */
static void install_C2(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(TMP0, RCX), NOP, NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* C3: GENARITHFLAGS_RR(R64DST, R64SRC) — macroinstruction-operand encoding */
static void install_C3(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(R64DST, R64SRC), NOP, NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* C4: GENARITHFLAGS_R(RBX) — single arch reg, not TMP */
static void install_C4(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_R(RBX), NOP, NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* C5: GENARITHFLAGS_IR(0, TMP0) — immediate-register form, "0 + TMP0" flags */
static void install_C5(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_IR(0, TMP0), NOP, NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* ─────────────────── group D: ADD destination variants ───────────────── */

/* D1: ADD writes to RBX (arch reg) instead of TMP, then GENARITHFLAGS */
static void install_D1(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(RBX, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_R(RBX), NOP, NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* D2: ADD writes to R64DST, then GENARITHFLAGS_R(R64DST) */
static void install_D2(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(R64DST, R64DST, R64SRC), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_R(R64DST), NOP, NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* ─────────────────── group E: multiple GENARITHFLAGS ─────────────────── */

/* E1: ADD ; GENARITHFLAGS_RR ; GENARITHFLAGS_R(TMP0) ; ADC — double bridge */
static void install_E1(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_R(TMP0), NOP, NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* E2: Two GENARITHFLAGS_RR same triad before ADC */
static void install_E2(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(RBX, RCX),
          GENARITHFLAGS_RR(RBX, RCX),
          NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* ─────────────────── group F: ADC immediately after gen (same triad) ─── */

/* F1: same triad: GENARITHFLAGS_RR + ADC, NO add (gen alone) */
static void install_F1(void) { HEADER;
    ucode_t p[] = {
        { GENARITHFLAGS_RR(RBX, RCX), FINAL_ADC, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* F2: T0 ADD ; T1 (GENARITHFLAGS_RR slot0 + ADC slot1) */
static void install_F2(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(RBX, RCX), FINAL_ADC, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

/* control: just ADC with no GENARITHFLAGS — expect RAX=0 (entry CF) */
static void install_ctrl(void) { HEADER;
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, RBX, RCX), NOP, NOP, NOP_SEQWORD },
        { FINAL_ADC, NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    }; INSTALL(p);
}

typedef struct { const char *name; void (*install)(void); } variant_t;

static variant_t variants[] = {
    /* baseline */
    { "ctrl  ADD ; ADC (no GENARITHFLAGS)",                                          install_ctrl },
    /* group A: same-triad arrangements */
    { "A1    same-triad: ADD ; GFL_RR(RBX,RCX) ; ADC",                               install_A1 },
    { "A2    same-triad: ADD ; GFL_R(TMP0) ; ADC",                                   install_A2 },
    { "A3    same-triad: GFL_RR(RBX,RCX) ; ADD ; ADC",                               install_A3 },
    { "A4    same-triad: GFL_RR(RBX,RCX) ; ADC ; ADD",                               install_A4 },
    { "A5    same-triad: ADC ; GFL_RR(RBX,RCX) ; ADD (reverse sanity)",              install_A5 },
    /* group B: cross-triad arrangements */
    { "B1    T0:ADD  T1:GFL_RR(RBX,RCX)  T2:ADC",                                    install_B1 },
    { "B2    T0:ADD  T1:GFL_R(TMP0)      T2:ADC",                                    install_B2 },
    { "B3    T0:ADD+GFL_RR(RBX,RCX)  T1:ADC",                                        install_B3 },
    { "B4    T0:ADD+GFL_R(TMP0)      T1:ADC",                                        install_B4 },
    { "B5    T0:ADD  T1:GFL_RR+ADC (same triad)",                                    install_B5 },
    { "B6    T0:GFL_RR  T1:ADD  T2:ADC (gen FIRST)",                                 install_B6 },
    { "B7    T0:GFL_RR  T1:ADC (no ADD at all)",                                     install_B7 },
    /* group C: operand variants */
    { "C1    T0:ADD  T1:GFL_RR(TMP0,TMP0)  T2:ADC",                                  install_C1 },
    { "C2    T0:ADD  T1:GFL_RR(TMP0,RCX)   T2:ADC",                                  install_C2 },
    { "C3    T0:ADD  T1:GFL_RR(R64DST,R64SRC)  T2:ADC",                              install_C3 },
    { "C4    T0:ADD  T1:GFL_R(RBX)         T2:ADC",                                  install_C4 },
    { "C5    T0:ADD  T1:GFL_IR(0, TMP0)    T2:ADC",                                  install_C5 },
    /* group D: ADD writes to arch reg */
    { "D1    T0:ADD→RBX  T1:GFL_R(RBX)      T2:ADC",                                 install_D1 },
    { "D2    T0:ADD→R64DST  T1:GFL_R(R64DST) T2:ADC",                                install_D2 },
    /* group E: multiple GENARITHFLAGS */
    { "E1    T0:ADD T1:GFL_RR T2:GFL_R(TMP0) T3:ADC",                                install_E1 },
    { "E2    T0:ADD T1:two GFL_RR same triad T2:ADC",                                install_E2 },
    /* group F: GENARITHFLAGS+ADC same triad */
    { "F1    one triad: GFL_RR(RBX,RCX) ; ADC (no ADD)",                             install_F1 },
    { "F2    T0:ADD T1:GFL_RR+ADC same triad",                                       install_F2 },
};

int main(void) {
    printf("============================================================\n");
    printf("  Exhaustive GENARITHFLAGS placement probe\n");
    printf("============================================================\n\n");
    printf("  Input (every probe): RBX=0xFFFFFFFFFFFFFFFE, RCX=3, entry CF=0\n");
    printf("    sum = 1, true CF = 1, TMP=1 (≠0 — discriminator condition)\n\n");
    printf("  ADC RAX = R8 + R9 + arch_CF   (R8=R9=0, so RAX_out = arch CF observed)\n");
    printf("  RAX=1 means an arrangement DID bridge the carry to ADC.\n");
    printf("  RAX=0 means it did not.\n\n");

    assign_to_core(0);

    int hits = 0;
    printf("  %-65s  RAX  result\n", "variant");
    printf("  -----------------------------------------------------------------  ---  ------------------\n");
    for (size_t i = 0; i < sizeof(variants)/sizeof(variants[0]); i++) {
        variants[i].install();
        uint64_t got = fire(A_IN, B_IN);
        const char *verdict;
        if      (got == 1) { verdict = "★ BRIDGED CARRY!"; hits++; }
        else if (got == 0) { verdict = "  no bridge";              }
        else               { verdict = "  unexpected";             }
        printf("  %-65s  %3" PRIu64 "  %s\n", variants[i].name, got, verdict);
    }

    printf("\n");
    printf("============================================================\n");
    if (hits > 0) {
        printf("  ★ %d arrangement(s) bridged the carry to ADC.\n", hits);
        printf("    Use one of those patterns for 4×64 microcode!\n");
    } else {
        printf("  ZERO arrangements bridged. The ADC carry-in port is\n");
        printf("  structurally separate from anything GENARITHFLAGS writes.\n");
        printf("  This is now confirmed across all probed placements,\n");
        printf("  operand encodings, triad positions, and slot orderings.\n");
    }
    printf("============================================================\n");
    return 0;
}
