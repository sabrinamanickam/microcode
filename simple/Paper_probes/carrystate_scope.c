/*
 * carrystate_scope.c — WHERE does Goldmont's per-destination carry state live,
 *                      and how many chains can be live at once?
 *
 * Companion to `additionwithflags.c` (the master ADD/SETCC/ADC/GENARITHFLAGS
 * test). That file proves the two-domain model and the GENARITHFLAGS_RR bridge.
 * It leaves exactly two claims we make in the paper unprobed:
 *
 *   CLAIM 1 (register class).  "An arithmetic uop associates conditional state
 *   with its destination."  Every existing probe writes that destination into a
 *   TMP.  CLAUDE.md states the rule "SETCC_CONDB_DR only works on TMP
 *   registers, NOT arch registers", but the supporting evidence is indirect:
 *     - test_setcc_repeated.c did ADD->RAX then SETCC(TMP5, RAX) and failed,
 *       but was confounded (it was diagnosing END_SEQWORD EFLAGS restore, and
 *       used 51-bit limbs that cannot carry out of 64 bits).
 *     - experiments/flag_probes.c is cited in EXPERIMENTS.md as the ADD->arch
 *       failure, but it reads SETCC_CONDB_DR(TMP2, TMP2) -- the state of a
 *       register that was never an ADD destination. It never tested the arch
 *       destination at all.
 *     - gfl_rr_operand_matrix.c / arch_gfl_chain.c show arch-DEST *bridge*
 *       chains leak, consistent with "arch dests hold no queryable state",
 *       but they test GENARITHFLAGS, not SETCC.
 *   Groups A and B below test it head-on, and separate the two distinct
 *   sub-claims: (A) the flag SOURCE register class, (B) the SETCC DEST class.
 *
 *   CLAIM 2 (independent chains).  "Different additions can materialize their
 *   carries into different registers, so several accumulation chains can
 *   progress without sharing RFLAGS."  additionwithflags.c Section 8 only shows
 *   that an unrelated SHR to TMP8 does not disturb TMP0's latched carry (and
 *   marks it exploratory). No probe has ever held two or more carries live
 *   simultaneously and read them all back. Group C does, over all 2^3 carry
 *   patterns, so the claim is certified rather than assumed.
 *
 * Harness, register conventions and expected-value discipline are copied from
 * additionwithflags.c so the two files are directly comparable.
 *
 *   buf[0..5] = R8,R9,R10,R11,R12,R13   (patch inputs)
 *   buf[6]    = rflags image (0x2 -> CF=0, 0x3 -> CF=1)
 *   buf[7]    = RAX out,  buf[8] = RBX out
 *
 * Build: make PROG=carrystate_scope
 * Run:   sudo taskset -c 0 ./carrystate_scope_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* ─────────────────────────── tally ─────────────────────────── */
static int g_pass = 0, g_fail = 0;
static void check(const char *what, uint64_t got, uint64_t want) {
    int ok = (got == want);
    if (ok) g_pass++; else g_fail++;
    printf("    [%s] %-58s got=%-20" PRIu64 " want=%-20" PRIu64 "\n",
           ok ? "PASS" : "FAIL", what, got, want);
}

static int g_probe_match = 0, g_probe_seen = 0;
static int probe(const char *what, uint64_t got, uint64_t if_hyp, const char *hyp) {
    g_probe_seen++;
    int m = (got == if_hyp);
    if (m) g_probe_match++;
    printf("    [obs ] %-50s got=%-6" PRIu64 "(%s=%" PRIu64 ") %s\n",
           what, got, hyp, if_hyp, m ? "MATCH" : "differs");
    return m;
}

/* ───────────────────────── firing harness ───────────────────────── */
static void fire(const uint64_t in[6], int cf_in,
                 uint64_t *rax_out, uint64_t *rbx_out) {
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
        "push qword ptr [%[bp] + 48]\n\t"   /* rflags image — LAST flag op */
        "popfq\n\t"
        "vmwrite rcx, rdx\n\t"              /* trigger */
        "mov  qword ptr [%[bp] + 56], rax\n\t"
        "mov  qword ptr [%[bp] + 64], rbx\n\t"
        :
        : [bp] "r"(buf)
        : "rax", "rbx", "rcx", "rdx",
          "r8", "r9", "r10", "r11", "r12", "r13",
          "cc", "memory"
    );
    if (rax_out) *rax_out = buf[7];
    if (rbx_out) *rbx_out = buf[8];
}

static uint64_t fire1(uint64_t r8, uint64_t r9, uint64_t r10, uint64_t r11,
                      uint64_t r12, uint64_t r13, int cf_in) {
    uint64_t in[6] = { r8, r9, r10, r11, r12, r13 }, rax;
    fire(in, cf_in, &rax, NULL);
    return rax;
}

static void install(ucode_t *p, int n) {
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, p, n);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* carry-producing / non-carry-producing operand pairs */
#define OVF_A 0xFFFFFFFFFFFFFFFFULL
#define OVF_B 1ULL          /* FF..F + 1 -> carry out, sum 0        */
#define NOC_A 1ULL
#define NOC_B 1ULL          /* 1 + 1     -> no carry,  sum 2        */
/* a carry with a NON-ZERO sum, so "carry" cannot be confused with "sum==0"
 * (this is the discriminator that killed the old GENARITHFLAGS_R hypothesis) */
#define OVFNZ_A 0xFFFFFFFFFFFFFFFEULL
#define OVFNZ_B 3ULL        /* FF..FE + 3 -> carry out, sum 1       */

/* ════════════════════════════════════════════════════════════════════════
 * GROUP A — flag SOURCE register class.
 *   Identical patch shape, only the ADD destination changes: TMP0 vs R12.
 *   A1 is the known-good control. A2/A3 ask whether an ARCH destination
 *   carries state SETCC can read at all.
 * ════════════════════════════════════════════════════════════════════════ */

/* A1: ADD -> TMP dest, SETCC reads it. Known good (production shape). */
static void install_a1_tmp_src(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), SETCC_CONDB_DR(TMP1, TMP0),
          NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP1), ZEROEXT_DSZ64_DR(RBX, TMP0),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

/* A2: ADD -> ARCH dest (R12), SETCC reads R12's state. Intra-triad. */
static void install_a2_arch_src(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(R12, R8, R9), SETCC_CONDB_DR(TMP1, R12),
          NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP1), ZEROEXT_DSZ64_DR(RBX, R12),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

/* A3: same as A2 but cross-triad, to rule out an intra-triad timing artefact. */
static void install_a3_arch_src_crosstriad(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(R12, R8, R9), NOP, NOP, NOP_SEQWORD },
        { SETCC_CONDB_DR(TMP1, R12),  NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP1), ZEROEXT_DSZ64_DR(RBX, R12),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

/* A4: does an arch destination at least update the arch flag domain, so that
 *     GENARITHFLAGS is unnecessary there?  ADD -> R12, then ADC reads arch CF.
 *     Entry arch CF forced to 0, so a 1 could only come from the ADD. */
static void install_a4_arch_add_then_adc(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(R12, R8, R9), NOP, NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, R10, R11), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

static void group_a(void) {
    printf("\n=== GROUP A: which register class holds the per-destination carry? ===\n");
    uint64_t rax, rbx;

    printf("  A1 control — ADD -> TMP0, SETCC(TMP0)  [documented WORKING]\n");
    install_a1_tmp_src();
    { uint64_t in[6] = { OVF_A, OVF_B, 0,0,0,0 };
      fire(in, 0, &rax, &rbx);
      check("carry-out, sum 0   -> SETCC = 1", rax, 1);
      check("  (sum written back correctly)",  rbx, 0); }
    { uint64_t in[6] = { OVFNZ_A, OVFNZ_B, 0,0,0,0 };
      fire(in, 0, &rax, &rbx);
      check("carry-out, sum 1   -> SETCC = 1", rax, 1);
      check("  (sum written back correctly)",  rbx, 1); }
    { uint64_t in[6] = { NOC_A, NOC_B, 0,0,0,0 };
      fire(in, 0, &rax, &rbx);
      check("no carry, sum 2    -> SETCC = 0", rax, 0);
      check("  (sum written back correctly)",  rbx, 2); }

    printf("  A2 — ADD -> R12 (ARCH), SETCC(R12), intra-triad\n");
    printf("       CLAUDE.md rule predicts 0 for every input (arch dests hold no\n");
    printf("       queryable state); the arithmetic result must still be correct.\n");
    install_a2_arch_src();
    { uint64_t in[6] = { OVF_A, OVF_B, 0,0,0,0 };
      fire(in, 0, &rax, &rbx);
      probe("ADD->R12 carry-out sum 0, SETCC(R12)", rax, 0, "arch-holds-nothing");
      check("  arithmetic still correct (R12 = 0)", rbx, 0); }
    { uint64_t in[6] = { OVFNZ_A, OVFNZ_B, 0,0,0,0 };
      fire(in, 0, &rax, &rbx);
      probe("ADD->R12 carry-out sum 1, SETCC(R12)", rax, 0, "arch-holds-nothing");
      check("  arithmetic still correct (R12 = 1)", rbx, 1); }
    { uint64_t in[6] = { NOC_A, NOC_B, 0,0,0,0 };
      fire(in, 0, &rax, &rbx);
      probe("ADD->R12 no carry,     SETCC(R12)",   rax, 0, "arch-holds-nothing");
      check("  arithmetic still correct (R12 = 2)", rbx, 2); }

    printf("  A3 — same, cross-triad (rules out an intra-triad forwarding artefact)\n");
    install_a3_arch_src_crosstriad();
    probe("ADD->R12 carry-out sum 0, SETCC next triad",
          fire1(OVF_A, OVF_B, 0,0,0,0, 0), 0, "arch-holds-nothing");
    probe("ADD->R12 carry-out sum 1, SETCC next triad",
          fire1(OVFNZ_A, OVFNZ_B, 0,0,0,0, 0), 0, "arch-holds-nothing");

    printf("  A4 — does an ARCH-dest ADD update the architectural CF that ADC reads?\n");
    printf("       (entry arch CF forced 0, so a 1 could only come from the ADD)\n");
    install_a4_arch_add_then_adc();
    probe("ADD->R12 carry-out, then ADC RAX=0+0+CF",
          fire1(OVF_A, OVF_B, 0, 0, 0, 0, 0), 0, "arch-CF-frozen");
}

/* ════════════════════════════════════════════════════════════════════════
 * GROUP B — SETCC DESTINATION register class.
 *   Can the materialised 0/1 be written straight into an arch register, or
 *   must it land in a TMP and be moved with ZEROEXT (as production does)?
 *   B2 carries an independent liveness witness in RBX so that a 0 in RAX
 *   cannot be mistaken for "the patch never ran".
 * ════════════════════════════════════════════════════════════════════════ */

/* B1: SETCC dest = arch RAX directly. */
static void install_b1_setcc_to_arch(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), SETCC_CONDB_DR(RAX, TMP0),
          NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RBX, TMP0), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

/* B2: both at once — SETCC to arch RAX and (the same carry) to TMP1->RBX.
 *     RBX is the known-good reading, so the pair is self-controlling. */
static void install_b2_setcc_both(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), SETCC_CONDB_DR(RAX, TMP0),
          SETCC_CONDB_DR(TMP1, TMP0), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RBX, TMP1), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

static void group_b(void) {
    printf("\n=== GROUP B: can SETCC's destination be an arch register? ===\n");
    uint64_t rax, rbx;

    printf("  B1 — SETCC_CONDB_DR(RAX, TMP0): materialise straight into arch\n");
    install_b1_setcc_to_arch();
    { uint64_t in[6] = { OVF_A, OVF_B, 0,0,0,0 };
      fire(in, 0, &rax, &rbx);
      probe("carry-out -> SETCC into RAX", rax, 1, "arch-dest-works");
      check("  patch really ran (RBX = TMP0 = 0)", rbx, 0); }
    { uint64_t in[6] = { OVFNZ_A, OVFNZ_B, 0,0,0,0 };
      fire(in, 0, &rax, &rbx);
      probe("carry-out (sum 1) -> SETCC into RAX", rax, 1, "arch-dest-works");
      check("  patch really ran (RBX = TMP0 = 1)", rbx, 1); }
    { uint64_t in[6] = { NOC_A, NOC_B, 0,0,0,0 };
      fire(in, 0, &rax, &rbx);
      probe("no carry  -> SETCC into RAX", rax, 0, "arch-dest-works");
      check("  patch really ran (RBX = TMP0 = 2)", rbx, 2); }

    printf("  B2 — same carry read twice in one triad: once into RAX, once into TMP1\n");
    printf("       RBX (via TMP1) is the known-good path, so it pins the truth.\n");
    install_b2_setcc_both();
    { uint64_t in[6] = { OVFNZ_A, OVFNZ_B, 0,0,0,0 };
      fire(in, 0, &rax, &rbx);
      check("TMP1 path  reads carry = 1", rbx, 1);
      probe("RAX  path  reads carry = 1", rax, 1, "arch-dest-works"); }
    { uint64_t in[6] = { NOC_A, NOC_B, 0,0,0,0 };
      fire(in, 0, &rax, &rbx);
      check("TMP1 path  reads carry = 0", rbx, 0);
      probe("RAX  path  reads carry = 0", rax, 0, "arch-dest-works"); }
}

/* ════════════════════════════════════════════════════════════════════════
 * GROUP C — several carry chains live at the same time.
 *   Three independent ADDs into TMP0 / TMP2 / TMP4, then all three carries
 *   read back in one firing and packed as c0 | c1<<1 | c2<<2 in RAX.
 *   All 8 carry patterns are exercised.  This is the claim that path #1
 *   supports multiple simultaneous accumulation chains, which a single
 *   architectural RFLAGS could not.
 * ════════════════════════════════════════════════════════════════════════ */

/* C1: three ADDs, then three SETCCs, then pack. */
static void install_c1_three_chains(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8,  R9),  NOP, NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRR(TMP2, R10, R11), NOP, NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRR(TMP4, R12, R13), NOP, NOP, NOP_SEQWORD },
        { SETCC_CONDB_DR(TMP1, TMP0), SETCC_CONDB_DR(TMP3, TMP2),
          SETCC_CONDB_DR(TMP5, TMP4), NOP_SEQWORD },
        { SHL_DSZ64_DRI(TMP3, TMP3, 1), SHL_DSZ64_DRI(TMP5, TMP5, 2),
          OR_DSZ64_DRR(TMP1, TMP1, TMP3), NOP_SEQWORD },
        { OR_DSZ64_DRR(TMP1, TMP1, TMP5), ZEROEXT_DSZ64_DR(RAX, TMP1),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

/* C2: interleaved — chain 0's carry is read AFTER chain 1's ADD has run, so a
 *     shared single flag register would have been overwritten by then. */
static void install_c2_interleaved(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8,  R9), ADD_DSZ64_DRR(TMP2, R10, R11),
          SETCC_CONDB_DR(TMP1, TMP0), NOP_SEQWORD },
        { SETCC_CONDB_DR(TMP3, TMP2), SHL_DSZ64_DRI(TMP3, TMP3, 1),
          OR_DSZ64_DRR(TMP1, TMP1, TMP3), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP1), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

static void group_c(void) {
    printf("\n=== GROUP C: multiple carry chains live simultaneously ===\n");

    /* operand pairs selected so each chain's carry is independently chosen,
     * and carrying sums are non-zero where possible. */
    struct { uint64_t a, b; } CARRY   = { OVFNZ_A, OVFNZ_B };  /* carry, sum 1 */
    struct { uint64_t a, b; } NOCARRY = { NOC_A,  NOC_B    };  /* no carry     */

    printf("  C1 — three ADDs (TMP0, TMP2, TMP4), all three carries read back\n");
    install_c1_three_chains();
    for (int m = 0; m < 8; m++) {
        int c0 = m & 1, c1 = (m >> 1) & 1, c2 = (m >> 2) & 1;
        uint64_t in[6] = {
            c0 ? CARRY.a : NOCARRY.a, c0 ? CARRY.b : NOCARRY.b,
            c1 ? CARRY.a : NOCARRY.a, c1 ? CARRY.b : NOCARRY.b,
            c2 ? CARRY.a : NOCARRY.a, c2 ? CARRY.b : NOCARRY.b,
        };
        uint64_t rax;
        fire(in, 0, &rax, NULL);
        char what[80];
        snprintf(what, sizeof what, "carries (c0,c1,c2) = (%d,%d,%d)", c0, c1, c2);
        check(what, rax, (uint64_t)m);
    }

    printf("  C2 — interleaved: chain 0's carry read after chain 1's ADD\n");
    install_c2_interleaved();
    for (int m = 0; m < 4; m++) {
        int c0 = m & 1, c1 = (m >> 1) & 1;
        uint64_t in[6] = {
            c0 ? CARRY.a : NOCARRY.a, c0 ? CARRY.b : NOCARRY.b,
            c1 ? CARRY.a : NOCARRY.a, c1 ? CARRY.b : NOCARRY.b,
            0, 0,
        };
        uint64_t rax;
        fire(in, 0, &rax, NULL);
        char what[80];
        snprintf(what, sizeof what, "interleaved (c0,c1) = (%d,%d)", c0, c1);
        check(what, rax, (uint64_t)m);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 * GROUP D — does path #1 leave the architectural carry flag untouched?
 *   The paper claims the SETCC path lets chains "progress without sharing
 *   RFLAGS". does_add_update_eflags.c shows ADD does not write arch flags;
 *   here we additionally show a full ADD+SETCC+fold chain does not either,
 *   by reading arch CF with an ADC afterwards under both entry polarities.
 * ════════════════════════════════════════════════════════════════════════ */
static void install_d1_setcc_chain_then_adc(void) {
    ucode_t p[] = {
        /* a full production-shape carry step on TMP0/TMP1 ... */
        { ADD_DSZ64_DRR(TMP0, R8, R9), SETCC_CONDB_DR(TMP1, TMP0),
          NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRR(TMP2, R10, R11), ADD_DSZ64_DRR(TMP2, TMP2, TMP1),
          NOP, NOP_SEQWORD },
        /* ... then ask what arch CF is: RAX = 0 + 0 + arch_CF */
        { ADC_DSZ64_DRR(RAX, TMP9, TMP9), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

static void group_d(void) {
    printf("\n=== GROUP D: the SETCC path does not disturb architectural CF ===\n");
    install_d1_setcc_chain_then_adc();
    /* TMP9 is never written by this patch; ADC adds it to itself, so RAX is
     * 2*TMP9 + CF. TMP9's residual value is unknown, so we only compare the
     * low bit's behaviour across the two entry polarities. */
    uint64_t r_cf0 = fire1(OVF_A, OVF_B, 5, 7, 0, 0, 0);
    uint64_t r_cf1 = fire1(OVF_A, OVF_B, 5, 7, 0, 0, 1);
    printf("    entry CF=0 -> RAX=%" PRIu64 "   entry CF=1 -> RAX=%" PRIu64 "\n",
           r_cf0, r_cf1);
    check("carrying ADD+SETCC chain leaves arch CF = entry CF (delta is exactly 1)",
          r_cf1 - r_cf0, 1);
}

/* ════════════════════════════════════════════════════════════════════════
 * GROUP E — two questions the first run of this file opened.
 *
 *   E1. Group B showed SETCC_CONDB_DR(RAX, TMP0) reads the carry correctly,
 *       contradicting the CLAUDE.md rule "SETCC only works on TMP registers".
 *       But the harness zeroes RAX before firing, so a narrow (8-bit) write
 *       is indistinguishable from a full 64-bit one. Pre-fill the destination
 *       with all-ones inside the patch and look at what survives.
 *
 *   E2. additionwithflags.c S8 #5 observed that ZEROEXT to TMP0 CLEARS the
 *       carry the preceding ADD latched (got 0, the file predicted 1), while
 *       SHL "preserved" it. But that SHL case shifted a bit out, so
 *       "preserved the ADD's 1" and "wrote its own 1" both predict 1.
 *       E2b/E2c discriminate; E2d asks the same of MUL, which flag_test.c
 *       G2 found innocent when it writes a DIFFERENT register.
 *       This decides whether the state's lifetime is "until the next ADD/SUB
 *       to that register" (as CLAUDE.md and microcode_findings.md §4 say) or
 *       "until the next write of any kind" — a stricter scheduling rule.
 * ════════════════════════════════════════════════════════════════════════ */

/* E1: RAX = all-ones, then SETCC_CONDB_DR(RAX, TMP0). */
static void install_e1_setcc_arch_width(void) {
    ucode_t p[] = {
        { ZEROEXT_DSZ64_DR(RAX, R10), ADD_DSZ64_DRR(TMP0, R8, R9),
          SETCC_CONDB_DR(RAX, TMP0), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RBX, TMP0), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

/* E2b: carry latched, then SHL that shifts NOTHING out. */
static void install_e2b_shl_noshiftout(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), NOP, NOP, NOP_SEQWORD },  /* carry = 1 */
        { SHL_DSZ64_DRI(TMP0, R10, 1), NOP, NOP, NOP_SEQWORD },  /* R10 = 1   */
        { SETCC_CONDB_DR(TMP1, TMP0), ZEROEXT_DSZ64_DR(RAX, TMP1),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

/* E2c: NO carry latched, then SHL that DOES shift a bit out. */
static void install_e2c_shl_shiftout(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), NOP, NOP, NOP_SEQWORD },  /* carry = 0 */
        { SHL_DSZ64_DRI(TMP0, R10, 1), NOP, NOP, NOP_SEQWORD },  /* R10 = 2^63 */
        { SETCC_CONDB_DR(TMP1, TMP0), ZEROEXT_DSZ64_DR(RAX, TMP1),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

/* E2d: carry latched, then a MUL writes the SAME register (hi half). */
static void install_e2d_mul_same_reg(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), NOP, NOP, NOP_SEQWORD },  /* carry = 1 */
        { MUL_DSZ64_DRR(TMP0, R10, R11), NOP, NOP, NOP_SEQWORD }, /* TMP0 = hi */
        { SETCC_CONDB_DR(TMP1, TMP0), ZEROEXT_DSZ64_DR(RAX, TMP1),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

/* E2e: control — carry latched, MUL writes a DIFFERENT register.
 *      flag_test.c G2 predicts the carry survives. */
static void install_e2e_mul_other_reg(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), NOP, NOP, NOP_SEQWORD },  /* carry = 1 */
        { MUL_DSZ64_DRR(TMP8, R10, R11), NOP, NOP, NOP_SEQWORD }, /* TMP8 = hi */
        { SETCC_CONDB_DR(TMP1, TMP0), ZEROEXT_DSZ64_DR(RAX, TMP1),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

static void group_e(void) {
    printf("\n=== GROUP E: SETCC arch-dest width, and what clears a latched carry ===\n");
    const uint64_t ONES = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t rax, rbx;

    printf("  E1 — SETCC into an arch register PRE-FILLED with all ones:\n");
    printf("       0 or 1        => full 64-bit destination write\n");
    printf("       0xFF..FF00/01 => narrow (8-bit) write, upper bits preserved\n");
    install_e1_setcc_arch_width();
    { uint64_t in[6] = { NOC_A, NOC_B, ONES, 0, 0, 0 };   /* no carry */
      fire(in, 0, &rax, &rbx);
      printf("    [obs ] no carry, RAX pre-filled FF..FF -> RAX = 0x%016" PRIx64 "\n", rax);
      check("  patch really ran (RBX = TMP0 = 2)", rbx, 2); }
    { uint64_t in[6] = { OVFNZ_A, OVFNZ_B, ONES, 0, 0, 0 };  /* carry, sum 1 */
      fire(in, 0, &rax, &rbx);
      printf("    [obs ] carry,    RAX pre-filled FF..FF -> RAX = 0x%016" PRIx64 "\n", rax);
      check("  patch really ran (RBX = TMP0 = 1)", rbx, 1); }

    printf("  E2 — does any write to a register replace its latched condition state,\n");
    printf("       or only an ADD/SUB?  (b) and (c) discriminate for SHL:\n");
    printf("       (0,1) => every write replaces it   (1,0) => only ADD/SUB writes it\n");
    install_e2b_shl_noshiftout();
    probe("E2b carry=1, then SHL 1<<1 (nothing out)",
          fire1(OVF_A, OVF_B, 1, 0, 0, 0, 0), 0, "SHL-replaces");
    install_e2c_shl_shiftout();
    probe("E2c carry=0, then SHL 2^63<<1 (bit out)",
          fire1(NOC_A, NOC_B, 0x8000000000000000ULL, 0, 0, 0, 0), 1, "SHL-replaces");

    printf("       (d) and (e) ask the same of MUL, same-register vs other-register:\n");
    install_e2d_mul_same_reg();
    probe("E2d carry=1, then MUL writes TMP0 (hi=0)",
          fire1(OVF_A, OVF_B, 0, 0, 0, 0, 0), 0, "MUL-replaces");
    install_e2e_mul_other_reg();
    probe("E2e carry=1, then MUL writes TMP8 (control)",
          fire1(OVF_A, OVF_B, 0, 0, 0, 0, 0), 1, "carry-survives");
}

/* ════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("================================================================\n");
    printf("  carrystate_scope.c — register-class scope of the per-destination\n");
    printf("  carry state, and simultaneity of independent carry chains.\n");
    printf("  Goldmont (Celeron N3350), vmwrite (0x0cd8) hook -> U7c00\n");
    printf("================================================================\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    group_a();   /* flag SOURCE register class  */
    group_b();   /* SETCC DEST register class   */
    group_c();   /* simultaneous chains         */
    group_d();   /* arch CF untouched by path 1 */
    group_e();   /* SETCC arch-dest width; carry lifetime */

    init_match_and_patch();
    do_fix_IN_patch();

    printf("\n================================================================\n");
    printf("  CERTIFYING TOTAL: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("  ALL PASS\n");
    else
        printf("  *** %d FAILURE(S) ***\n", g_fail);
    printf("  Hypothesis probes: %d/%d matched (informational — read the lines)\n",
           g_probe_match, g_probe_seen);
    printf("================================================================\n");
    return g_fail ? 1 : 0;
}
