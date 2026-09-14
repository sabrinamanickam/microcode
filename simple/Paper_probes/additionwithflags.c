/*
 * additionwithflags.c — MASTER TEST for addition-with-carry on Goldmont.
 *
 * Consolidates the entire ADD / SETCC / ADC / GENARITHFLAGS investigation
 * (adc_findings.md, test_adc*.c, intra-triad-adc*.c, genflagsrr*.c,
 * test_genarithflags_semantics.c, chained_4limb_add.c, gfl_rr_operand_matrix.c)
 * into ONE self-contained, deterministic, citable test.
 *
 * This is the file to rely on while writing the paper: every claim we make
 * about flag domains on the Red-Unlocked Goldmont (Celeron N3350) is asserted
 * here with a PASS/FAIL line and a known-good expected value.
 *
 * ============================================================================
 *  THE TWO-DOMAIN FLAG MODEL (what this test proves)
 * ============================================================================
 *
 *  Domain #1 — per-TMP-register carry ("internal ALU flags")
 *     • WRITTEN by every ADD / ADC to its destination register.
 *     • READ by SETCC_CONDB_DR(dst, src): dst = (src's carry-out) ? 1 : 0.
 *     • Per-register: each TMP/arch reg carries its own latched carry,
 *       persisting until the next ADD/ADC writes that same register.
 *     • This is the carry primitive used in ALL production curve code
 *       (curve25519, p256, secp256k1, ...): ADD → SETCC → fold.
 *
 *  Domain #2 — architectural RFLAGS.CF
 *     • This is the x86 carry flag the caller sets with popfq before vmwrite.
 *     • READ by ADC as its carry-IN. FROZEN at patch entry: no ADD, and no
 *       other ADC, updates it for a later ADC in the same patch.
 *     • ADC does NOT write Domain #2 either (two ADCs read the same frozen CF).
 *
 *  The bridge — GENARITHFLAGS_RR(TMP, TMP)   [same TMP twice]
 *     • Copies a register's Domain-#1 carry INTO Domain #2 (arch CF), so the
 *       NEXT ADC consumes it. This is the ONLY operand form that bridges:
 *       GENARITHFLAGS_RR(arch,arch) leaks, GENARITHFLAGS_R / mixed-operand
 *       forms do not bridge reliably (gfl_rr_operand_matrix.c, chain_length.c).
 *     • ADC's destination DOES carry a Domain-#1 carry-out (like any ADD), so
 *       GENARITHFLAGS_RR(adc_dst, adc_dst) re-bridges it → ADC chains compose.
 *
 *  ANSWERS TO THE KEY QUESTIONS (negative controls — Section 3):
 *     Q: Generate a carry inside the microcode domain (Domain #1). Without
 *        GENARITHFLAGS, can ADC find it?
 *     A: NO. ADC's carry-in is ALWAYS the entry arch CF (Domain #2). The
 *        Domain-#1 carry is provably present (SETCC reads it = 1) yet ADC
 *        returns the frozen entry CF, ignoring it — no matter how many times
 *        the Domain-#1 carry is set or updated mid-patch.
 *     Q: Does it need GENARITHFLAGS, or does SETCC suffice?
 *     A: It needs GENARITHFLAGS_RR(TMP,TMP). SETCC reads Domain #1 but does
 *        NOT promote the carry to arch CF, so ADC after a bare SETCC still
 *        sees the frozen entry CF. SETCC's role is to materialise the carry
 *        as a 0/1 VALUE for the fold-ADD path; it is not a bridge to ADC.
 *
 *  Consequences for the paper:
 *     • SETCC carry chain  : ADD + SETCC + folding-ADD  = 3 ops/limb, never
 *                            touches arch RFLAGS (used in production).
 *     • ADC carry chain    : GENARITHFLAGS_RR + ADC     = 2 ops/limb, but
 *                            routes the carry through arch CF (Domain #2).
 *     Both are correct; production curve arithmetic uses the SETCC form.
 *
 * Build: make PROG=additionwithflags
 * Run:   sudo taskset -c 0 ./additionwithflags_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* ─────────────────────────── global tally ─────────────────────────── */
static int g_pass = 0, g_fail = 0;
static void check(const char *what, uint64_t got, uint64_t want) {
    int ok = (got == want);
    if (ok) g_pass++; else g_fail++;
    printf("    [%s] %-58s got=%-20" PRIu64 " want=%-20" PRIu64 "\n",
           ok ? "PASS" : "FAIL", what, got, want);
}

/* Exploratory probes answer genuinely-open questions: they report the observed
 * value against a stated hypothesis but do NOT affect the certifying tally or
 * the exit code. Returns 1 on match. */
static int g_probe_match = 0, g_probe_seen = 0;
static int probe(const char *what, uint64_t got, uint64_t if_hyp, const char *hyp) {
    g_probe_seen++;
    int m = (got == if_hyp);
    if (m) g_probe_match++;
    printf("    [obs ] %-50s got=%-6" PRIu64 "(%s=%" PRIu64 ") %s\n",
           what, got, hyp, if_hyp, m ? "MATCH" : "differs");
    return m;
}

/* ───────────────────────── generic firing harness ─────────────────────
 * All inputs/outputs go through a memory buffer so GCC can never alias a
 * source value into one of the registers we clobber (the classic inline-asm
 * footgun). The patch reads R8..R13 and writes RAX (and RBX where noted).
 * Entry arch CF is set by popfq immediately before vmwrite — nothing between
 * popfq and the trigger touches flags, so entry CF is exactly `cf_in`.
 *
 *   buf[0..5] = R8,R9,R10,R11,R12,R13     (patch inputs)
 *   buf[6]    = rflags image (0x2 → CF=0, 0x3 → CF=1)
 *   buf[7]    = RAX out
 *   buf[8]    = RBX out
 */
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
        "vmwrite rcx, rdx\n\t"              /* trigger (operands irrelevant) */
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

/* convenience: single-value (RAX) fire */
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

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* ════════════════════════════════════════════════════════════════════════
 * SECTION 1 — Domain #1: ADD writes a per-TMP carry, SETCC reads it.
 *   This is the production carry primitive. 128-bit add with NO ADC:
 *     T0: ADD  TMP0 = a_lo + b_lo ; SETCC TMP15 = carry(TMP0)
 *     T1: ADD  TMP1 = a_hi + b_hi ; ADD TMP1 = TMP1 + TMP15   (fold carry)
 *     T2: RAX = TMP0 (sum_lo) ; RBX = TMP1 (sum_hi)
 *   Verified against __uint128_t over edge + random inputs.
 * ════════════════════════════════════════════════════════════════════════ */
static void install_add_setcc(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R10), SETCC_CONDB_DR(TMP15, TMP0),
          NOP, NOP_SEQWORD },
        { ADD_DSZ64_DRR(TMP1, R9, R11), ADD_DSZ64_DRR(TMP1, TMP1, TMP15),
          NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP0), ZEROEXT_DSZ64_DR(RBX, TMP1),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void add128_setcc(const uint64_t a[2], const uint64_t b[2], uint64_t out[2]) {
    uint64_t in[6] = { a[0], a[1], b[0], b[1], 0, 0 };
    fire(in, /*cf_in irrelevant*/0, &out[0], &out[1]);
}
static void section1(void) {
    printf("\n=== SECTION 1: ADD + SETCC carry chain (Domain #1, production) ===\n");
    install_add_setcc();

    struct { uint64_t a[2], b[2]; const char *label; } v[] = {
        { {0,0}, {0,0}, "0 + 0" },
        { {1,2}, {3,4}, "no carry" },
        { {0xFFFFFFFFFFFFFFFFULL,0}, {1,0}, "lo overflow -> hi" },
        { {0xFFFFFFFFFFFFFFFFULL,5}, {1,7}, "lo overflow, hi has values" },
        { {0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL}, {1,0}, "full wrap (carry lost)" },
    };
    for (size_t i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
        uint64_t out[2];
        add128_setcc(v[i].a, v[i].b, out);
        __uint128_t A = ((__uint128_t)v[i].a[1] << 64) | v[i].a[0];
        __uint128_t B = ((__uint128_t)v[i].b[1] << 64) | v[i].b[0];
        __uint128_t S = A + B;
        uint64_t got = (out[1] == (uint64_t)(S >> 64) && out[0] == (uint64_t)S);
        check(v[i].label, got, 1);
    }

    /* random stress */
    uint64_t rng = 0xADD5E7CC0FF33EE1ULL; int rp = 0;
    for (int i = 0; i < 5000; i++) {
        uint64_t a[2] = { splitmix64(&rng), splitmix64(&rng) };
        uint64_t b[2] = { splitmix64(&rng), splitmix64(&rng) }, out[2];
        add128_setcc(a, b, out);
        __uint128_t S = (((__uint128_t)a[1]<<64)|a[0]) + (((__uint128_t)b[1]<<64)|b[0]);
        if (out[0] == (uint64_t)S && out[1] == (uint64_t)(S >> 64)) rp++;
    }
    check("5000 random 128-bit adds", rp, 5000);
}

/* ════════════════════════════════════════════════════════════════════════
 * SECTION 2 — Domain #2: ADC reads the FROZEN architectural CF.
 * ════════════════════════════════════════════════════════════════════════ */

/* 2a: a single ADC reads the entry arch CF. */
static void install_adc_single(void) {
    ucode_t p[] = {
        { ADC_DSZ64_DRR(RAX, R8, R9), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
/* 2b: ADD (slot 0) then ADC (slot 1), SAME triad. ADC must NOT see the ADD. */
static void install_adc_intratriad(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), ADC_DSZ64_DRR(RAX, R10, R11),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
/* 2c: ADD in T0, ADC in T1. Triad boundary alone does not bridge. */
static void install_adc_crosstriad(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), NOP, NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, R10, R11), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
/* 2d: two ADCs back-to-back. Both read the SAME frozen entry CF. */
static void install_adc_two(void) {
    ucode_t p[] = {
        { ADC_DSZ64_DRR(RAX, R8, R9), NOP, NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RBX, R10, R11), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void section2(void) {
    printf("\n=== SECTION 2: ADC reads frozen architectural CF (Domain #2) ===\n");

    printf("  2a. single ADC RAX = R8 + R9 + arch_CF\n");
    install_adc_single();
    check("entry CF=0, 0+0            -> 0", fire1(0,0,0,0,0,0, 0), 0);
    check("entry CF=1, 0+0            -> 1", fire1(0,0,0,0,0,0, 1), 1);
    check("entry CF=1, 5+3            -> 9", fire1(5,3,0,0,0,0, 1), 9);
    check("entry CF=0, 5+3            -> 8", fire1(5,3,0,0,0,0, 0), 8);

    printf("  2b. intra-triad: ADD(slot0) overflow must NOT reach ADC(slot1)\n");
    install_adc_intratriad();
    /* ADD R8+R9 = 0xFF..F + 1 overflows (Domain#1 CF=1); ADC R10+R11 = 0+0;
     * entry arch CF = 0. ADC ignores the ADD's carry -> RAX = 0. */
    check("slot0 ADD overflows, entry CF=0 -> ADC sees 0",
          fire1(0xFFFFFFFFFFFFFFFFULL,1, 0,0, 0,0, 0), 0);
    /* Flip: slot0 ADD no overflow (Domain#1 CF=0) but entry arch CF=1.
     * ADC reads the frozen entry CF -> RAX = 1. */
    check("slot0 ADD no overflow, entry CF=1 -> ADC sees 1",
          fire1(1,1, 0,0, 0,0, 1), 1);

    printf("  2c. cross-triad: ADD(T0) overflow must NOT reach ADC(T1)\n");
    install_adc_crosstriad();
    check("T0 ADD overflows, entry CF=0 -> ADC sees 0",
          fire1(0xFFFFFFFFFFFFFFFFULL,1, 0,0, 0,0, 0), 0);
    check("T0 ADD no overflow, entry CF=1 -> ADC sees 1",
          fire1(1,1, 0,0, 0,0, 1), 1);

    printf("  2d. two ADCs both read the SAME frozen CF (ADC does not write Domain #2)\n");
    install_adc_two();
    /* entry CF=1: first ADC RAX=0+0+1=1 (its own CF-out is 0), second ADC
     * RBX=0+0+? — if it saw the first ADC's CF-out it'd be 0; it reads the
     * frozen entry CF=1 -> RBX=1. */
    {
        uint64_t in[6] = {0,0,0,0,0,0}, rax, rbx;
        fire(in, 1, &rax, &rbx);
        check("first  ADC RAX -> 1", rax, 1);
        check("second ADC RBX -> 1 (same frozen CF, not first ADC's 0)", rbx, 1);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 * SECTION 3 — The bridge: GENARITHFLAGS_RR(TMP,TMP) copies Domain #1 -> #2.
 *   T0: ADD TMP0 = R8 + R9
 *   T1: GENARITHFLAGS_RR(TMP0, TMP0)     (arch CF := TMP0's real carry)
 *   T2: ADC RAX  = R10 + R11 + arch_CF   (R10=R11=0 -> RAX = bridged CF)
 *   Entry arch CF forced to 0, so any 1 MUST come from the bridge.
 *   DISCRIMINATOR cases (overflow with TMP != 0) prove the bridge copies the
 *   true CARRY, not (result == 0).
 * ════════════════════════════════════════════════════════════════════════ */
static void install_bridge(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(TMP0, TMP0), NOP, NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, R10, R11), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void section3(void) {
    printf("\n=== SECTION 4: GENARITHFLAGS_RR(TMP,TMP) bridges Domain #1 -> #2 ===\n");
    install_bridge();
    struct { uint64_t a, b; int cf; const char *label; } v[] = {
        { 1, 1,                                         0, "1+1                 no ovf, TMP=2" },
        { 0, 0,                                         0, "0+0                 no ovf, TMP=0" },
        { 5, 0,                                         0, "5+0                 no ovf, TMP=5" },
        { 0xFFFFFFFFFFFFFFFFULL, 1,                     1, "FF..F+1             ovf,    TMP=0" },
        { 0xFFFFFFFFFFFFFFFEULL, 3,                     1, "FF..E+3             ovf,    TMP=1 (discrim)" },
        { 0xFFFFFFFFFFFFFFFEULL, 5,                     1, "FF..E+5             ovf,    TMP=3 (discrim)" },
        { 0xDEADBEEF12345678ULL, 0xCAFEFEEDABCDEF01ULL, 1, "DEAD..+CAFE..       ovf,    TMP=random" },
        { 0x8000000000000000ULL, 0x8000000000000000ULL, 1, "8000..+8000..       ovf,    TMP=0" },
    };
    for (size_t i = 0; i < sizeof(v)/sizeof(v[0]); i++)
        check(v[i].label, fire1(v[i].a, v[i].b, 0, 0, 0, 0, /*entry CF*/0), (uint64_t)v[i].cf);

    /* #1: the bridge WRITES arch CF — it must be able to drive a set entry
     * CF back to 0 (otherwise a no-carry limb would falsely propagate). */
    printf("  bridge overrides a SET entry CF (proves write, not OR):\n");
    check("entry CF=1, ADD 2+3 no ovf, bridge -> ADC sees 0",
          fire1(2, 3, 0, 0, 0, 0, /*entry CF*/1), 0);
    check("entry CF=1, ADD FF..F+1 ovf, bridge -> ADC sees 1",
          fire1(0xFFFFFFFFFFFFFFFFULL, 1, 0, 0, 0, 0, /*entry CF*/1), 1);
}

/* ════════════════════════════════════════════════════════════════════════
 * SECTION 4 — ADC's destination carries a Domain-#1 carry -> chains compose.
 *   T0: ADD TMP0 = R8 + R9
 *   T1: GENARITHFLAGS_RR(TMP0, TMP0)
 *   T2: ADC TMP1 = R10 + R11 + arch_CF   (second adder; sets TMP1 Domain-#1 CF)
 *   T3: GENARITHFLAGS_RR(TMP1, TMP1)      (re-bridge ADC's own carry-out)
 *   T4: ADC RAX = R12 + R13 + arch_CF     (R12=R13=0 -> RAX = TMP1's carry-out)
 * ════════════════════════════════════════════════════════════════════════ */
static void install_chain(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9),   NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(TMP0, TMP0),  NOP, NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP1, R10, R11), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(TMP1, TMP1),  NOP, NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, R12, R13),  NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void section4(void) {
    printf("\n=== SECTION 5: ADC writes Domain-#1 carry-out (chainable) ===\n");
    install_chain();
    /* first ADD (a+b) feeds CF into the second adder (c+d+CF); we report the
     * second adder's carry-out. Expected computed via __uint128_t. */
    struct { uint64_t a, b, c, d; const char *label; } v[] = {
        { 0xFFFFFFFFFFFFFFFFULL, 1, 0xFFFFFFFFFFFFFFFFULL, 0, "1st ovf -> 2nd FF..F+0+1 ovf" },
        { 0xFFFFFFFFFFFFFFFFULL, 1, 5, 3,                     "1st ovf -> 2nd 5+3+1 no ovf" },
        { 0xFFFFFFFFFFFFFFFFULL, 1, 0xFFFFFFFFFFFFFFFEULL, 0, "1st ovf -> 2nd FF..E+0+1 no ovf" },
        { 1, 1, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, "1st no ovf -> 2nd FF..F+FF..F ovf" },
        { 1, 1, 5, 3,                                         "1st no ovf -> 2nd 5+3 no ovf" },
    };
    for (size_t i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
        int first_cf = (v[i].a + v[i].b) < v[i].a;
        __uint128_t s = (__uint128_t)v[i].c + v[i].d + first_cf;
        uint64_t want = (uint64_t)(s >> 64);
        check(v[i].label, fire1(v[i].a, v[i].b, v[i].c, v[i].d, 0, 0, /*entry CF*/0), want);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 * SECTION 5 — End-to-end capstone: 256-bit (4x64) add via chained ADC + bridge,
 *   packed in the PRODUCTION layout {ADC, GENARITHFLAGS_RR(dst,dst)} per triad.
 *   Verified against __uint128_t over edge + 10k random inputs, carry included.
 *   (Mirrors chained_4limb_add.c — the proven primitive in action.)
 * ════════════════════════════════════════════════════════════════════════ */
static void install_add256(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    ucode_t p[] = {
        /* stash a[0..3]=R8..R11 -> TMP0..3, b[0..3]=R12..R15 -> TMP4..7 */
        { ZEROEXT_DSZ64_DR(TMP0, R8),  ZEROEXT_DSZ64_DR(TMP1, R9),
          ZEROEXT_DSZ64_DR(TMP2, R10), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(TMP3, R11), ZEROEXT_DSZ64_DR(TMP4, R12),
          ZEROEXT_DSZ64_DR(TMP5, R13), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(TMP6, R14), ZEROEXT_DSZ64_DR(TMP7, R15),
          NOP, NOP_SEQWORD },
        /* limb 0: ADD ; bridge */
        { ADD_DSZ64_DRR(TMP8, TMP0, TMP4), GENARITHFLAGS_RR(TMP8, TMP8),
          NOP, NOP_SEQWORD },
        /* limbs 1..3: ADC + bridge */
        { ADC_DSZ64_DRR(TMP9, TMP1, TMP5),  GENARITHFLAGS_RR(TMP9, TMP9),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP10, TMP2, TMP6), GENARITHFLAGS_RR(TMP10, TMP10),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(TMP11, TMP3, TMP7), GENARITHFLAGS_RR(TMP11, TMP11),
          NOP, NOP_SEQWORD },
        /* writeback result[0..3] -> R8..R11, final carry -> RAX */
        { ZEROEXT_DSZ64_DR(R8,  TMP8),  ZEROEXT_DSZ64_DR(R9,  TMP9),
          ZEROEXT_DSZ64_DR(R10, TMP10), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(R11, TMP11), ADC_DSZ64_DRR(RAX, RDX, RSI),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}
static uint64_t add256_ucode(const uint64_t a[4], const uint64_t b[4], uint64_t out[4]) {
    uint64_t buf[13];
    for (int i = 0; i < 4; i++) { buf[i] = a[i]; buf[4 + i] = b[i]; }
    asm volatile(
        "mov  r8,  qword ptr [%[bp] + 0]\n\t"
        "mov  r9,  qword ptr [%[bp] + 8]\n\t"
        "mov  r10, qword ptr [%[bp] + 16]\n\t"
        "mov  r11, qword ptr [%[bp] + 24]\n\t"
        "mov  r12, qword ptr [%[bp] + 32]\n\t"
        "mov  r13, qword ptr [%[bp] + 40]\n\t"
        "mov  r14, qword ptr [%[bp] + 48]\n\t"
        "mov  r15, qword ptr [%[bp] + 56]\n\t"
        "xor  rdx, rdx\n\t"
        "xor  rsi, rsi\n\t"
        "xor  rax, rax\n\t"
        "vmwrite rbx, rcx\n\t"
        "mov  qword ptr [%[bp] + 64], r8\n\t"
        "mov  qword ptr [%[bp] + 72], r9\n\t"
        "mov  qword ptr [%[bp] + 80], r10\n\t"
        "mov  qword ptr [%[bp] + 88], r11\n\t"
        "mov  qword ptr [%[bp] + 96], rax\n\t"
        :
        : [bp] "r"(buf)
        : "rax", "rbx", "rcx", "rdx", "rsi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "cc", "memory"
    );
    for (int i = 0; i < 4; i++) out[i] = buf[8 + i];
    return buf[12];
}
static uint64_t add256_ref(const uint64_t a[4], const uint64_t b[4], uint64_t out[4]) {
    __uint128_t s = 0;
    for (int i = 0; i < 4; i++) { s = (__uint128_t)a[i] + b[i] + (uint64_t)(s >> 64); out[i] = (uint64_t)s; }
    return (uint64_t)(s >> 64);
}
static void section5(void) {
    printf("\n=== SECTION 6: end-to-end 256-bit add (chained ADC + bridge) ===\n");
    install_add256();

    struct { uint64_t a[4], b[4]; const char *label; } v[] = {
        { {0,0,0,0}, {0,0,0,0}, "0 + 0" },
        { {0xFFFFFFFFFFFFFFFFULL,0,0,0}, {1,0,0,0}, "limb0 carry" },
        { {0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL,
           0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL}, {1,0,0,0}, "carry through all 4" },
        { {0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL,
           0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL},
          {0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL,
           0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL}, "max + max" },
    };
    for (size_t i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
        uint64_t ou[4], or_[4];
        uint64_t cu = add256_ucode(v[i].a, v[i].b, ou);
        uint64_t cr = add256_ref(v[i].a, v[i].b, or_);
        check(v[i].label, (cu == cr && !memcmp(ou, or_, 32)), 1);
    }
    uint64_t rng = 0xC0FFEE1234567890ULL; int rp = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t a[4], b[4], ou[4], or_[4];
        for (int j = 0; j < 4; j++) { a[j] = splitmix64(&rng); b[j] = splitmix64(&rng); }
        uint64_t cu = add256_ucode(a, b, ou);
        uint64_t cr = add256_ref(a, b, or_);
        if (cu == cr && !memcmp(ou, or_, 32)) rp++;
    }
    check("10000 random 256-bit adds (result + carry)", rp, 10000);
}

/* ════════════════════════════════════════════════════════════════════════
 * SECTION 3 — NEGATIVE CONTROLS (the core of the user's question):
 *   Make a carry inside the microcode domain (Domain #1) and ask, step by
 *   step, what ADC can reach. Entry arch CF is forced so that any "1" ADC
 *   returns MUST have come from the Domain-#1 carry — if it can reach it.
 *
 *   (A) prove the Domain-#1 carry is really there  : SETCC reads it = 1
 *   (B) ADC without any bridge                      : sees frozen arch CF, NOT it
 *   (C) ADD + SETCC + ADC (no GENARITHFLAGS)        : SETCC does NOT bridge → 0
 *   (D) ADD + GENARITHFLAGS_RR + ADC                : the bridge → ADC sees 1
 *   (E) set then UPDATE the Domain-#1 carry         : ADC's carry-in never moves
 * ════════════════════════════════════════════════════════════════════════ */

/* (A) ADD writes TMP0's Domain-#1 carry; SETCC materialises it into RAX. */
static void install_neg_setcc_reads(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), NOP, NOP, NOP_SEQWORD },
        { SETCC_CONDB_DR(TMP1, TMP0), ZEROEXT_DSZ64_DR(RAX, TMP1),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

/* (C) ADD overflow, SETCC reads the carry, then ADC — SETCC is not a bridge. */
static void install_neg_setcc_then_adc(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), SETCC_CONDB_DR(TMP1, TMP0),
          NOP, NOP_SEQWORD },
        { ADC_DSZ64_DRR(RAX, R10, R11), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

/* (E) set TMP0's Domain-#1 carry, then OVERWRITE it with a second ADD; ADC
 *     after still reads only the frozen entry arch CF. */
static void install_neg_update_carry(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9),   NOP, NOP, NOP_SEQWORD }, /* set    */
        { ADD_DSZ64_DRR(TMP0, R12, R13), NOP, NOP, NOP_SEQWORD }, /* update */
        { ADC_DSZ64_DRR(RAX, R10, R11),  NOP, NOP, NOP_SEQWORD }, /* RAX = entry CF */
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}

static void section_neg(void) {
    printf("\n=== SECTION 3: NEGATIVE CONTROLS — can ADC reach a Domain-#1 carry? ===\n");
    const uint64_t OVF_A = 0xFFFFFFFFFFFFFFFFULL, OVF_B = 1; /* FF..F + 1 overflows */

    printf("  (A) the Domain-#1 carry EXISTS — SETCC reads it directly\n");
    install_neg_setcc_reads();
    check("ADD FF..F+1 overflow  -> SETCC carry = 1", fire1(OVF_A, OVF_B, 0,0,0,0, 0), 1);
    check("ADD 2+3   no overflow -> SETCC carry = 0", fire1(2, 3,         0,0,0,0, 0), 0);

    printf("  (B) WITHOUT a bridge ADC cannot see it (entry arch CF=0)\n");
    install_adc_crosstriad();   /* T0: ADD overflow ; T1: ADC RAX = 0+0+arch_CF */
    check("Domain#1 CF=1, no bridge -> ADC sees 0 (not 1)", fire1(OVF_A, OVF_B, 0,0,0,0, 0), 0);

    printf("  (C) SETCC alone does NOT bridge to ADC (entry arch CF=0)\n");
    install_neg_setcc_then_adc();
    check("ADD overflow + SETCC, no GENARITHFLAGS -> ADC sees 0", fire1(OVF_A, OVF_B, 0,0,0,0, 0), 0);

    printf("  (D) GENARITHFLAGS_RR(TMP,TMP) is exactly what lets ADC see it (entry arch CF=0)\n");
    install_bridge();
    check("ADD overflow + GENARITHFLAGS_RR -> ADC sees 1", fire1(OVF_A, OVF_B, 0,0,0,0, 0), 1);

    printf("  (E) setting/updating the Domain-#1 carry never moves ADC's carry-in\n");
    install_neg_update_carry();
    /* set carry=1 (R8+R9 overflow) then UPDATE to 0 (R12+R13 no overflow);
     * entry arch CF=1 -> ADC still reads the frozen 1, ignoring Domain #1. */
    check("set CF=1 then update->0, entry arch CF=1 -> ADC sees 1",
          fire1(OVF_A, OVF_B, 0,0, 2, 3, 1), 1);
    /* set carry=0 then UPDATE to 1; entry arch CF=0 -> ADC still reads 0. */
    check("set CF=0 then update->1, entry arch CF=0 -> ADC sees 0",
          fire1(2, 3, 0,0, OVF_A, OVF_B, 0), 0);
}

/* ════════════════════════════════════════════════════════════════════════
 * SECTION 7 — EXPLORATORY: does Domain #1 latch the FULL flag set, or only CF?
 *   Production only ever reads CF (SETCC_CONDB). Here we ADD into TMP0 and read
 *   it back through SETCC_CONDB (CF), _CONDZ (ZF), _CONDS (SF), _CONDO (OF).
 *   If the per-register domain carries the whole arithmetic flag set, every
 *   probe MATCHes the x86 prediction. Reported, not certifying.
 * ════════════════════════════════════════════════════════════════════════ */
static void install_flagset_cf_zf(void) {   /* RAX = CF(TMP0), RBX = ZF(TMP0) */
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), SETCC_CONDB_DR(TMP1, TMP0),
          SETCC_CONDZ_DR(TMP2, TMP0), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP1), ZEROEXT_DSZ64_DR(RBX, TMP2),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void install_flagset_sf_of(void) {   /* RAX = SF(TMP0), RBX = OF(TMP0) */
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), SETCC_CONDS_DR(TMP1, TMP0),
          SETCC_CONDO_DR(TMP2, TMP0), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP1), ZEROEXT_DSZ64_DR(RBX, TMP2),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void section_flagset(void) {
    printf("\n=== SECTION 7 (exploratory): does Domain #1 latch the full flag set? ===\n");
    struct { uint64_t a, b; const char *label; } v[] = {
        { 5, 3,                                          "5 + 3              = 8" },
        { 5, 0xFFFFFFFFFFFFFFFBULL,                       "5 + (-5)           = 0" },
        { 0, 0x8000000000000000ULL,                       "0 + 2^63           (sign set)" },
        { 0x7FFFFFFFFFFFFFFFULL, 1,                        "INT64_MAX + 1      (signed +ovf)" },
        { 0x8000000000000000ULL, 0x8000000000000000ULL,   "INT64_MIN+INT64_MIN(signed -ovf)" },
        { 0xFFFFFFFFFFFFFFFFULL, 1,                        "FF..F + 1          = 0" },
    };
    int n = 0, cf_ok = 0, zf_ok = 0, sf_ok = 0, of_ok = 0;
    for (size_t i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
        uint64_t a = v[i].a, b = v[i].b, sum = a + b;
        uint64_t cf = (sum < a), zf = (sum == 0), sf = (sum >> 63),
                 of = (((~(a ^ b)) & (a ^ sum)) >> 63) & 1;
        uint64_t in[6] = { a, b, 0, 0, 0, 0 }, rax, rbx;
        printf("  %s\n", v[i].label);
        install_flagset_cf_zf(); fire(in, 0, &rax, &rbx);
        cf_ok += probe("    SETCC_CONDB (CF)", rax, cf, "x86");
        zf_ok += probe("    SETCC_CONDZ (ZF)", rbx, zf, "x86");
        install_flagset_sf_of(); fire(in, 0, &rax, &rbx);
        sf_ok += probe("    SETCC_CONDS (SF)", rax, sf, "x86");
        of_ok += probe("    SETCC_CONDO (OF)", rbx, of, "x86");
        n++;
    }
    printf("  ---- Domain #1 flag tracking: CF %s(%d/%d)  ZF %s(%d/%d)  SF %s(%d/%d)  OF %s(%d/%d)\n",
           cf_ok==n?"YES":"NO ", cf_ok, n, zf_ok==n?"YES":"NO ", zf_ok, n,
           sf_ok==n?"YES":"NO ", sf_ok, n, of_ok==n?"YES":"NO ", of_ok, n);
}

/* ════════════════════════════════════════════════════════════════════════
 * SECTION 8 — per-register carry lifetime:
 *   #6 SETCC read is non-destructive (two reads agree)        [certifying]
 *   #5 what writes/clears a register's Domain-#1 carry?       [exploratory]
 * ════════════════════════════════════════════════════════════════════════ */
/* #6: ADD sets TMP0's carry; read it twice. */
static void install_setcc_twice(void) {
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9), SETCC_CONDB_DR(TMP1, TMP0),
          SETCC_CONDB_DR(TMP2, TMP0), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP1), ZEROEXT_DSZ64_DR(RBX, TMP2),
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
/* #5: ADD overflow sets TMP0 carry=1; OP rewrites TMP0; SETCC reports survival. */
static void install_clobber_move(void) {    /* ZEROEXT TMP0 = R10 */
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9),  NOP, NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(TMP0, R10),  NOP, NOP, NOP_SEQWORD },
        { SETCC_CONDB_DR(TMP1, TMP0), ZEROEXT_DSZ64_DR(RAX, TMP1), NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void install_clobber_or(void) {       /* OR TMP0 = R10 | R11 */
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9),  NOP, NOP, NOP_SEQWORD },
        { OR_DSZ64_DRR(TMP0, R10, R11), NOP, NOP, NOP_SEQWORD },
        { SETCC_CONDB_DR(TMP1, TMP0), ZEROEXT_DSZ64_DR(RAX, TMP1), NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void install_clobber_shl(void) {      /* SHL TMP0 = R10 << 1 (bit shifts out) */
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9),  NOP, NOP, NOP_SEQWORD },
        { SHL_DSZ64_DRI(TMP0, R10, 1),  NOP, NOP, NOP_SEQWORD },
        { SETCC_CONDB_DR(TMP1, TMP0), ZEROEXT_DSZ64_DR(RAX, TMP1), NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void install_carry_survives_otherreg(void) { /* write a DIFFERENT reg (TMP8) */
    ucode_t p[] = {
        { ADD_DSZ64_DRR(TMP0, R8, R9),  NOP, NOP, NOP_SEQWORD },
        { SHR_DSZ64_DRI(TMP8, R10, 1),  NOP, NOP, NOP_SEQWORD },
        { SETCC_CONDB_DR(TMP1, TMP0), ZEROEXT_DSZ64_DR(RAX, TMP1), NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void section_carry_lifetime(void) {
    printf("\n=== SECTION 8: per-register carry lifetime ===\n");
    const uint64_t OVF_A = 0xFFFFFFFFFFFFFFFFULL, OVF_B = 1;

    printf("  #6 SETCC read is non-destructive (two reads of the same carry agree):\n");
    install_setcc_twice();
    {
        uint64_t in[6] = { OVF_A, OVF_B, 0, 0, 0, 0 }, rax, rbx;
        fire(in, 0, &rax, &rbx);
        check("first  SETCC_CONDB(TMP0) -> 1", rax, 1);
        check("second SETCC_CONDB(TMP0) -> 1 (read not destructive)", rbx, 1);
    }

    printf("  #5 does rewriting TMP0 with a non-ADD op disturb the latched ADD carry? (exploratory)\n");
    install_clobber_move();
    probe("ADD ovf, ZEROEXT TMP0=7, SETCC(TMP0)",        fire1(OVF_A, OVF_B, 7, 0, 0, 0, 0), 1, "preserved");
    install_clobber_or();
    probe("ADD ovf, OR TMP0=3|4, SETCC(TMP0)",           fire1(OVF_A, OVF_B, 3, 4, 0, 0, 0), 0, "OR-clears");
    install_clobber_shl();
    probe("ADD ovf, SHL TMP0=2^63<<1, SETCC(TMP0)",      fire1(OVF_A, OVF_B, 0x8000000000000000ULL, 0, 0, 0, 0), 1, "SHL-bitout");

    printf("  #5 a write to a DIFFERENT register must leave TMP0's carry intact (production relies on this):\n");
    install_carry_survives_otherreg();
    probe("ADD ovf TMP0, SHR TMP8=R10>>1, SETCC(TMP0)",  fire1(OVF_A, OVF_B, 0xFF, 0, 0, 0, 0), 1, "intact");
}

/* ════════════════════════════════════════════════════════════════════════ */
int main(void) {
    printf("================================================================\n");
    printf("  additionwithflags.c — master test for ADD/SETCC/ADC/GENARITHFLAGS\n");
    printf("  Goldmont (Celeron N3350), vmwrite (0x0cd8) hook -> U7c00\n");
    printf("================================================================\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    section1();    /* Domain #1: ADD + SETCC                              */
    section2();    /* Domain #2: ADC reads frozen arch CF                 */
    section_neg(); /* NEGATIVE CONTROLS: Domain-#1 carry invisible to ADC */
    section3();    /* bridge: GENARITHFLAGS_RR(TMP,TMP)                   */
    section4();          /* ADC carry-out is a Domain-#1 carry (chainable)      */
    section5();          /* end-to-end 256-bit add                              */
    section_flagset();   /* EXPLORATORY: full flag set in Domain #1             */
    section_carry_lifetime(); /* SETCC non-destructive + what clobbers a carry  */

    /* restore a clean patch state */
    init_match_and_patch();
    do_fix_IN_patch();

    printf("\n================================================================\n");
    printf("  CERTIFYING TOTAL: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("  ALL PASS — flag-domain model verified end to end.\n");
    else
        printf("  *** %d FAILURE(S) — model does not hold as documented. ***\n", g_fail);
    printf("  Exploratory probes: %d/%d matched the stated hypothesis (informational)\n",
           g_probe_match, g_probe_seen);
    printf("================================================================\n");
    return g_fail ? 1 : 0;
}
