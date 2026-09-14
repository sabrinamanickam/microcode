/*
 * subtractionwithflags.c — sibling of additionwithflags.c for the BORROW path.
 *
 * Status: FIRST empirical characterisation of SBB_DSZ64 (opcode 0x37f). It was
 * added to opcode.h purely by symmetry with ADC_DSZ64 (0x37e) and never run.
 * SUB_DSZ64 (0x040-family minus) is used in asm_op_p256_sq.c but its borrow
 * domain was never isolated. This file answers, for subtraction, the same
 * questions additionwithflags.c answered for addition:
 *
 *   • Does SBB_DSZ64 actually subtract-with-borrow, and in which operand order?
 *     (project_microcode_loops notes "SUB args reversed" — verified here.)
 *   • Does SBB read its borrow-IN from the frozen architectural CF (Domain #2),
 *     exactly like ADC reads carry-in?
 *   • Is a Domain-#1 borrow (from SUB→TMP, readable by SETCC_CONDB) invisible to
 *     SBB without GENARITHFLAGS_RR — and does the bridge make it visible?
 *   • Does a chained SBB + GENARITHFLAGS_RR compose into a correct 4×64 subtract?
 *
 * Because SBB is unproven, most checks are EXPLORATORY probes (report observed
 * vs the x86 hypothesis); they don't fail the build. The end-to-end subtract
 * auto-adapts to the discovered operand order and reports its pass-rate.
 *
 * Build: make PROG=subtractionwithflags
 * Run:   sudo taskset -c 0 ./subtractionwithflags_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "patch.h"
#include "ucode_macro.h"
#include "misc.h"

/* ─────────── tally (subtraction is fully exploratory: SBB was unproven) ─── */
static int g_probe_match = 0, g_probe_seen = 0;
static int probe(const char *what, uint64_t got, uint64_t if_hyp, const char *hyp) {
    g_probe_seen++;
    int m = (got == if_hyp);
    if (m) g_probe_match++;
    printf("    [obs ] %-46s got=%016" PRIx64 " (%s=%016" PRIx64 ") %s\n",
           what, got, hyp, if_hyp, m ? "MATCH" : "differs");
    return m;
}

/* ─────────── firing harness (memory-buffered, identical to addition test) ───
 *   buf[0..5] = R8..R13 inputs ; buf[6] = rflags (0x2 CF=0 / 0x3 CF=1)
 *   buf[7] = RAX out ; buf[8] = RBX out                                    */
static void fire(const uint64_t in[6], int cf_in, uint64_t *rax_out, uint64_t *rbx_out) {
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
        "mov  qword ptr [%[bp] + 64], rbx\n\t"
        :
        : [bp] "r"(buf)
        : "rax", "rbx", "rcx", "rdx",
          "r8", "r9", "r10", "r11", "r12", "r13", "cc", "memory"
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
static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* Discovered operand order: 0 = SUB/SBB(dst,a,b) computes a-b (x86-like),
 * 1 = computes b-a (reversed). Set by section A. The OP macros below build
 * "minuend - subtrahend" correctly under either convention. */
static int g_rev = 0;
#define SUBOP(dst, minu, subt) (g_rev ? SUB_DSZ64_DRR(dst, subt, minu) : SUB_DSZ64_DRR(dst, minu, subt))
#define SBBOP(dst, minu, subt) (g_rev ? SBB_DSZ64_DRR(dst, subt, minu) : SBB_DSZ64_DRR(dst, minu, subt))

/* ════════════════════════════════════════════════════════════════════════
 * SECTION A — does SBB_DSZ64 subtract, in which order, and where is borrow-in?
 * ════════════════════════════════════════════════════════════════════════ */
static void install_sbb_single(void) {  /* RAX = SBB(R8, R9) with raw operand order */
    ucode_t p[] = {
        { SBB_DSZ64_DRR(RAX, R8, R9), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void section_A(void) {
    printf("\n=== SECTION A: SBB_DSZ64 (0x37f) basic semantics ===\n");
    install_sbb_single();

    /* Determine operand order with an asymmetric input, entry CF=0. */
    uint64_t r = fire1(10, 3, 0, 0, 0, 0, /*CF*/0);
    printf("  SBB(R8=10, R9=3), entry CF=0 -> %016" PRIx64 "\n", r);
    if (r == 7) {
        g_rev = 0;
        printf("  => order is dst = src0 - src1  (x86-like, NOT reversed)\n");
    } else if (r == (uint64_t)-7) {
        g_rev = 1;
        printf("  => order is dst = src1 - src0  (REVERSED — operands flipped)\n");
    } else {
        printf("  => SBB_DSZ64 did NOT produce 10-3 either way (got %016" PRIx64 ").\n", r);
        printf("     0x37f may not be SBB on this part — later sections are moot.\n");
    }

    /* Now test borrow-IN: does entry arch CF subtract an extra 1? Use SBBOP so
     * the arithmetic is minuend-subtrahend regardless of order. */
    printf("  borrow-in source (does entry arch CF subtract 1?):\n");
    ucode_t p[] = {
        { SBBOP(RAX, R8, R9), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
    probe("SBB 10-3, entry CF=0 -> 7",  fire1(10, 3, 0, 0, 0, 0, 0), 7, "x86");
    probe("SBB 10-3, entry CF=1 -> 6",  fire1(10, 3, 0, 0, 0, 0, 1), 6, "x86 borrow-in=archCF");
    probe("SBB 0-0,  entry CF=1 -> -1", fire1(0, 0, 0, 0, 0, 0, 1), (uint64_t)-1, "x86 borrow-in=archCF");
    probe("SBB 3-10, entry CF=0 -> -7", fire1(3, 10, 0, 0, 0, 0, 0), (uint64_t)-7, "x86 (underflow)");
}

/* ════════════════════════════════════════════════════════════════════════
 * SECTION B — SUB writes a Domain-#1 borrow that SETCC_CONDB reads.
 *   (CF=1 after a subtraction means "borrow"; SETCC_CONDB = "below" = CF.)
 * ════════════════════════════════════════════════════════════════════════ */
static void install_sub_setcc(void) {  /* RAX = borrow flag of (R8 - R9) */
    ucode_t p[] = {
        { SUBOP(TMP0, R8, R9), SETCC_CONDB_DR(TMP1, TMP0), NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(RAX, TMP1), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void section_B(void) {
    printf("\n=== SECTION B: SUB sets a Domain-#1 borrow; SETCC_CONDB reads it ===\n");
    install_sub_setcc();
    probe("SUB 3-10 (underflow) -> SETCC borrow = 1", fire1(3, 10, 0, 0, 0, 0, 0), 1, "borrow");
    probe("SUB 10-3 (no borrow) -> SETCC borrow = 0", fire1(10, 3, 0, 0, 0, 0, 0), 0, "no-borrow");
    probe("SUB 5-5  (exact)     -> SETCC borrow = 0", fire1(5, 5, 0, 0, 0, 0, 0), 0, "no-borrow");
}

/* ════════════════════════════════════════════════════════════════════════
 * SECTION C — is the Domain-#1 borrow invisible to SBB without the bridge,
 *             and does GENARITHFLAGS_RR(TMP,TMP) make it visible?
 *   entry arch CF forced to 0, so any "-1" SBB returns came from the bridge.
 *     RAX = SBB(0 - 0 - borrow):  no borrow -> 0 ; borrow -> 0xFFFF...FFFF
 * ════════════════════════════════════════════════════════════════════════ */
static void install_sub_then_sbb_nobridge(void) {
    ucode_t p[] = {
        { SUBOP(TMP0, R8, R9), NOP, NOP, NOP_SEQWORD },         /* Domain-#1 borrow */
        { SBBOP(RAX, R10, R11), NOP, NOP, NOP_SEQWORD },        /* reads frozen arch CF */
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void install_sub_bridge_sbb(void) {
    ucode_t p[] = {
        { SUBOP(TMP0, R8, R9), NOP, NOP, NOP_SEQWORD },
        { GENARITHFLAGS_RR(TMP0, TMP0), NOP, NOP, NOP_SEQWORD },/* borrow -> arch CF */
        { SBBOP(RAX, R10, R11), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    install(p, ARRAY_SZ(p));
}
static void section_C(void) {
    printf("\n=== SECTION C: Domain-#1 borrow vs SBB, with/without GENARITHFLAGS ===\n");
    printf("  (A) SUB 3-10 borrow exists (Section B already showed SETCC reads it)\n");

    printf("  (B) WITHOUT bridge: SBB(0-0) can't see it (entry arch CF=0)\n");
    install_sub_then_sbb_nobridge();
    probe("SUB 3-10 borrow, no bridge -> SBB(0-0) = 0", fire1(3, 10, 0, 0, 0, 0, 0), 0, "invisible");

    printf("  (C) WITH bridge: GENARITHFLAGS_RR routes the borrow -> SBB sees it\n");
    install_sub_bridge_sbb();
    probe("SUB 3-10 borrow + bridge -> SBB(0-0-1) = -1", fire1(3, 10, 0, 0, 0, 0, 0), (uint64_t)-1, "bridged");
    probe("SUB 10-3 no borrow + bridge -> SBB(0-0-0) = 0", fire1(10, 3, 0, 0, 0, 0, 0), 0, "bridged");
}

/* ════════════════════════════════════════════════════════════════════════
 * SECTION D — end-to-end 256-bit subtract via chained SBB + GENARITHFLAGS_RR.
 *   Operand order auto-adapted via SUBOP/SBBOP (g_rev from Section A).
 *   limb0: SUB t0 = a0-b0 ; bridge   limbs1..3: SBB ti = ai-bi-borrow ; bridge
 * ════════════════════════════════════════════════════════════════════════ */
static void install_sub256(void) {
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
        { SUBOP(TMP8, TMP0, TMP4),  GENARITHFLAGS_RR(TMP8, TMP8),  NOP, NOP_SEQWORD },
        { SBBOP(TMP9, TMP1, TMP5),  GENARITHFLAGS_RR(TMP9, TMP9),  NOP, NOP_SEQWORD },
        { SBBOP(TMP10, TMP2, TMP6), GENARITHFLAGS_RR(TMP10, TMP10), NOP, NOP_SEQWORD },
        { SBBOP(TMP11, TMP3, TMP7), GENARITHFLAGS_RR(TMP11, TMP11), NOP, NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(R8,  TMP8),  ZEROEXT_DSZ64_DR(R9,  TMP9),
          ZEROEXT_DSZ64_DR(R10, TMP10), NOP_SEQWORD },
        { ZEROEXT_DSZ64_DR(R11, TMP11), SBB_DSZ64_DRR(RAX, RDX, RSI),  /* RDX=RSI=0 -> RAX = final borrow */
          NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, END_SEQWORD },
    };
    patch_ucode(0x7c00, p, ARRAY_SZ(p));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}
static uint64_t sub256_ucode(const uint64_t a[4], const uint64_t b[4], uint64_t out[4]) {
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
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "cc", "memory"
    );
    for (int i = 0; i < 4; i++) out[i] = buf[8 + i];
    return buf[12];
}
static uint64_t sub256_ref(const uint64_t a[4], const uint64_t b[4], uint64_t out[4]) {
    uint64_t borrow = 0;
    for (int i = 0; i < 4; i++) {
        __uint128_t d = (__uint128_t)a[i] - b[i] - borrow;
        out[i] = (uint64_t)d;
        borrow = (uint64_t)(d >> 64) & 1;
    }
    return borrow;
}
static void section_D(void) {
    printf("\n=== SECTION D: end-to-end 256-bit subtract (chained SBB + bridge) ===\n");
    install_sub256();
    struct { uint64_t a[4], b[4]; const char *label; } v[] = {
        { {5,0,0,0}, {3,0,0,0}, "limb0, no borrow" },
        { {0,0,0,0}, {1,0,0,0}, "0 - 1 (borrow through all 4)" },
        { {0,1,0,0}, {1,0,0,0}, "limb0 borrow into limb1" },
        { {0xFFFFFFFFFFFFFFFFULL,0xFFFFFFFFFFFFFFFFULL,0,0},
          {0xFFFFFFFFFFFFFFFFULL,0,0,0}, "mixed" },
    };
    int pass = 0, tot = 0;
    for (size_t i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
        uint64_t ou[4], orf[4];
        uint64_t bu = sub256_ucode(v[i].a, v[i].b, ou);
        uint64_t br = sub256_ref(v[i].a, v[i].b, orf);
        int ok = (bu == br && !memcmp(ou, orf, 32));
        printf("    [%s] %-40s borrow ucode=%" PRIu64 " ref=%" PRIu64 "\n",
               ok ? "ok " : "DIFF", v[i].label, bu, br);
        pass += ok; tot++;
    }
    uint64_t rng = 0x5BB0123456789ABCULL; int rp = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t a[4], b[4], ou[4], orf[4];
        for (int j = 0; j < 4; j++) { a[j] = splitmix64(&rng); b[j] = splitmix64(&rng); }
        uint64_t bu = sub256_ucode(a, b, ou);
        uint64_t br = sub256_ref(a, b, orf);
        if (bu == br && !memcmp(ou, orf, 32)) rp++;
    }
    printf("    [%s] 10000 random 256-bit subtracts: %d/10000\n", rp==10000?"ok ":"DIFF", rp);
    pass += (rp == 10000); tot++;
    /* report (not certifying — SBB is the thing under test) */
    g_probe_seen += tot; g_probe_match += pass;
    printf("  end-to-end subtract: %d/%d vector groups matched the reference\n", pass, tot);
}

int main(void) {
    printf("================================================================\n");
    printf("  subtractionwithflags.c — SBB_DSZ64 (0x37f) + borrow domain\n");
    printf("  Goldmont (Celeron N3350). FIRST empirical test of this opcode.\n");
    printf("================================================================\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    section_A();   /* SBB exists? order? borrow-in source?     */
    section_B();   /* SUB sets Domain-#1 borrow, SETCC reads it */
    section_C();   /* bridge needed for SBB to see it           */
    section_D();   /* end-to-end 256-bit subtract               */

    init_match_and_patch();
    do_fix_IN_patch();

    printf("\n================================================================\n");
    printf("  Operand order: %s\n", g_rev ? "REVERSED (dst=src1-src0)" : "x86-like (dst=src0-src1)");
    printf("  Probes matching x86/borrow hypothesis: %d/%d\n", g_probe_match, g_probe_seen);
    printf("  (All subtraction results are exploratory — SBB_DSZ64 was unproven.)\n");
    printf("================================================================\n");
    return 0;
}
