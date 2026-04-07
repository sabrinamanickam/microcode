/*
 * flag_test.c — Ultimate microcode flag test for Goldmont
 *
 * Systematically probes every flag mechanism to answer:
 *
 *   Q1. Does ADD set flags SETCC can read? (intra-triad? cross-triad?)
 *   Q2. Does MUL poison the flag state SETCC reads?
 *   Q3. Does GENARITHFLAGS publish flags to SETCC?
 *   Q4. What raw bits does READAFLAGS return?
 *   Q5. Is CONDB the right condition code for carry?
 *   Q6. Does END_SEQWORD restore pre-hook EFLAGS (repeated invocations)?
 *   Q7. Do MOVEINSERTFLGS / MOVEMERGEFLGS affect flags?
 *   Q8. Can UJMPCC / CMOVCC / SELECTCC read carry?
 *
 * Hook: vmwrite rcx, rdx → patch at 0x7c00
 * Convention: inputs via RAX, RCX, RDX; result returned in RAX.
 *
 * Build:  make PROG=flag_test
 * Run:    sudo taskset -c 0 ./flag_test_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"


/* ═══════════════════════════════════════════════════════════════════
 *  INFRASTRUCTURE
 * ═══════════════════════════════════════════════════════════════════ */

static void do_patch(ucode_t *patch, int n_triads) {
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, n_triads);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* Single invocation: set RAX/RCX/RDX, trigger vmwrite, read RAX back */
static uint64_t invoke1(uint64_t rax_in, uint64_t rcx_in, uint64_t rdx_in) {
        uint64_t result;
        asm volatile(
                "mov rax, %[a]\n\t"
                "mov rcx, %[c]\n\t"
                "mov rdx, %[d]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[out], rax\n\t"
                : [out] "=r"(result)
                : [a] "r"(rax_in), [c] "r"(rcx_in), [d] "r"(rdx_in)
                : "rax", "rcx", "rdx", "r8"
        );
        return result;
}

/* Double invocation: two vmwrites, return RAX after second */
static uint64_t invoke2(uint64_t rax_in,
                         uint64_t rcx1, uint64_t rdx1,
                         uint64_t rcx2, uint64_t rdx2) {
        uint64_t result;
        asm volatile(
                "mov rax, %[a]\n\t"
                "mov rcx, %[c1]\n\t"
                "mov rdx, %[d1]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov rcx, %[c2]\n\t"
                "mov rdx, %[d2]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[out], rax\n\t"
                : [out] "=r"(result)
                : [a] "r"(rax_in),
                  [c1] "r"(rcx1), [d1] "r"(rdx1),
                  [c2] "r"(rcx2), [d2] "r"(rdx2)
                : "rax", "rcx", "rdx", "r8"
        );
        return result;
}

/* Invoke returning both RAX and RBX */
static void invoke1_ab(uint64_t rax_in, uint64_t rcx_in, uint64_t rdx_in,
                        uint64_t *rax_out, uint64_t *rbx_out) {
        asm volatile(
                "mov rax, %[a]\n\t"
                "mov rcx, %[c]\n\t"
                "mov rdx, %[d]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[oa], rax\n\t"
                "mov %[ob], rbx\n\t"
                : [oa] "=r"(*rax_out), [ob] "=r"(*rbx_out)
                : [a] "r"(rax_in), [c] "r"(rcx_in), [d] "r"(rdx_in)
                : "rax", "rbx", "rcx", "rdx", "r8"
        );
}

static const char *PF(int ok) { return ok ? "PASS" : "** FAIL **"; }

static void section(const char *title) {
        printf("\n══════════════════════════════════════════════════════════\n");
        printf("  %s\n", title);
        printf("══════════════════════════════════════════════════════════\n\n");
}


/* ═══════════════════════════════════════════════════════════════════
 *  GROUP 1: BASELINE — Flag mechanisms WITHOUT MUL
 *
 *  All hooks compute ADD(RAX + RCX), try to read CF, return it in RAX.
 *  Test with CF=0 (small+small) and CF=1 (0xFF..F + 3).
 * ═══════════════════════════════════════════════════════════════════ */

/* 1A: ADD + SETCC_CONDB intra-triad (slot 0 + slot 1) */
static void install_1a(void) {
        ucode_t p[] = {
                { ADD_DSZ64_DRR(TMP0, RAX, RCX),
                  SETCC_CONDB_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 2);
}

/* 1B: ADD + SETCC_CONDNB intra-triad (inverted polarity) */
static void install_1b(void) {
        ucode_t p[] = {
                { ADD_DSZ64_DRR(TMP0, RAX, RCX),
                  SETCC_CONDNB_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 2);
}

/* 1C: ADD + SETCC_CONDB cross-triad (ADD in T0, SETCC in T1) */
static void install_1c(void) {
        ucode_t p[] = {
                { ADD_DSZ64_DRR(TMP0, RAX, RCX),
                  NOP, NOP, NOP_SEQWORD },
                { SETCC_CONDB_DR(TMP1, TMP0),
                  NOP, NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 3);
}

/* 1D: GENARITHFLAGS_RR(A,B) + SETCC_CONDB same triad (no ADD) */
static void install_1d(void) {
        ucode_t p[] = {
                { GENARITHFLAGS_RR(RAX, RCX),
                  SETCC_CONDB_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 2);
}

/* 1E: ADD in T0, GENARITHFLAGS_RR(result,operand) + SETCC_CONDB in T1 */
static void install_1e(void) {
        ucode_t p[] = {
                { ADD_DSZ64_DRR(TMP0, RAX, RCX),
                  NOP, NOP, NOP_SEQWORD },
                { GENARITHFLAGS_RR(TMP0, RAX),
                  SETCC_CONDB_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 3);
}

/* 1F: GENARITHFLAGS_RR(A,B) + SETCC_CONDB cross-triad */
static void install_1f(void) {
        ucode_t p[] = {
                { GENARITHFLAGS_RR(RAX, RCX),
                  NOP, NOP, NOP_SEQWORD },
                { SETCC_CONDB_DR(TMP1, TMP0),
                  NOP, NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 3);
}

/* 1G: SUB + SETCC_CONDB intra-triad (does SUB behave like ADD?) */
static void install_1g(void) {
        ucode_t p[] = {
                /* SUB(TMP0, RAX, RCX) = RAX - RCX; CF=1 if borrow */
                { SUB_DSZ64_DRR(TMP0, RAX, RCX),
                  SETCC_CONDB_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 2);
}

/* 1H: SETCC_CONDB with NO flag-generating op — what's the default? */
static void install_1h(void) {
        ucode_t p[] = {
                { SETCC_CONDB_DR(TMP1, TMP0),
                  NOP, NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 2);
}

static void run_group1(void) {
        section("GROUP 1: Baseline flag mechanisms (no MUL)");

        uint64_t cf0_a = 100, cf0_c = 50;   /* no carry */
        uint64_t cf1_a = 0xFFFFFFFFFFFFFFFFULL, cf1_c = 3;  /* carry */

        struct { const char *name; void (*install)(void); int expect_cf0; int expect_cf1; } tests[] = {
                { "1A  ADD+SETCC intra-triad",           install_1a, 0, 1 },
                { "1B  ADD+SETCC_CONDNB intra-triad",    install_1b, 1, 0 },
                { "1C  ADD+SETCC cross-triad",           install_1c, 0, 1 },
                { "1D  GENARITHFLAGS+SETCC same triad",  install_1d, 0, 1 },
                { "1E  ADD,GENFLAGS+SETCC cross-triad",  install_1e, 0, 1 },
                { "1F  GENFLAGS,SETCC both cross-triad", install_1f, 0, 1 },
                { "1G  SUB+SETCC intra-triad",           install_1g, 0, 1 },
        };

        for (int i = 0; i < 7; i++) {
                tests[i].install();
                uint64_t r0 = invoke1(cf0_a, cf0_c, 0);
                uint64_t r1 = invoke1(cf1_a, cf1_c, 0);
                int ok0 = ((int)r0 == tests[i].expect_cf0);
                int ok1 = ((int)r1 == tests[i].expect_cf1);
                printf("  %-42s CF=0→%lu(%s)  CF=1→%lu(%s)  %s\n",
                       tests[i].name,
                       r0, PF(ok0), r1, PF(ok1),
                       (ok0 && ok1) ? "WORKS" : "BROKEN");
        }

        /* 1H: default state */
        install_1h();
        uint64_t def = invoke1(0, 0, 0);
        printf("  %-42s value=%lu (stale flags from pre-hook)\n",
               "1H  SETCC with no flag gen", def);
}


/* ═══════════════════════════════════════════════════════════════════
 *  GROUP 2: MUL INTERACTION — Does MUL poison SETCC?
 *
 *  Same ADD(RAX+RCX) carry test, but with MUL preceding it.
 *  MUL operands are controlled separately via RDX.
 * ═══════════════════════════════════════════════════════════════════ */

/* 2A: MUL in T0, ADD+SETCC in T1 (intra-triad SETCC) */
static void install_2a(void) {
        ucode_t p[] = {
                /* T0: save operands, MUL pollutes RCX/RDX */
                { ZEROEXT_DSZ64_DR(TMP3, RAX),
                  ZEROEXT_DSZ64_DR(TMP4, RCX),
                  NOP, NOP_SEQWORD },
                /* T1: MUL (just to pollute flags) */
                { MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                  NOP, NOP, NOP_SEQWORD },
                /* T2: ADD + SETCC (intra-triad) */
                { ADD_DSZ64_DRR(TMP0, TMP3, TMP4),
                  SETCC_CONDB_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 4);
}

/* 2B: MUL, NOP gap, then ADD+SETCC */
static void install_2b(void) {
        ucode_t p[] = {
                { ZEROEXT_DSZ64_DR(TMP3, RAX),
                  ZEROEXT_DSZ64_DR(TMP4, RCX),
                  NOP, NOP_SEQWORD },
                { MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                  NOP, NOP, NOP_SEQWORD },
                { NOP, NOP, NOP, NOP_SEQWORD },
                { ADD_DSZ64_DRR(TMP0, TMP3, TMP4),
                  SETCC_CONDB_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 5);
}

/* 2C: MUL, ADD, then GENARITHFLAGS+SETCC same triad */
static void install_2c(void) {
        ucode_t p[] = {
                { ZEROEXT_DSZ64_DR(TMP3, RAX),
                  ZEROEXT_DSZ64_DR(TMP4, RCX),
                  NOP, NOP_SEQWORD },
                { MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                  NOP, NOP, NOP_SEQWORD },
                { ADD_DSZ64_DRR(TMP0, TMP3, TMP4),
                  NOP, NOP, NOP_SEQWORD },
                { GENARITHFLAGS_RR(TMP0, TMP3),
                  SETCC_CONDB_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 5);
}

/* 2D: Exact MAC layout — ZEROEXT+MUL in T0, ADD+SETCC in T1
 * ADD uses MUL's product_lo (in RCX after MUL) */
static void install_2d(void) {
        ucode_t p[] = {
                { ZEROEXT_DSZ64_DR(TMP3, RAX),
                  MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                  NOP, NOP_SEQWORD },
                { ADD_DSZ64_DRR(TMP0, TMP3, RCX),
                  SETCC_CONDB_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 3);
}

static void run_group2(void) {
        section("GROUP 2: MUL interaction — flag pollution");

        /* ADD(0xFF..F + 3) should carry. MUL operands small (RCX*RDX=small). */
        uint64_t cf0_a = 100, cf0_c = 50;
        uint64_t cf1_a = 0xFFFFFFFFFFFFFFFFULL, cf1_c = 3;

        struct { const char *name; void (*install)(void); } tests[] = {
                { "2A  MUL, ADD+SETCC intra-triad",    install_2a },
                { "2B  MUL, NOP gap, ADD+SETCC",       install_2b },
                { "2C  MUL, ADD, GENFLAGS+SETCC",      install_2c },
        };

        for (int i = 0; i < 3; i++) {
                tests[i].install();
                uint64_t r0 = invoke1(cf0_a, cf0_c, 5);
                uint64_t r1 = invoke1(cf1_a, cf1_c, 5);
                printf("  %-42s CF=0→%lu(%s)  CF=1→%lu(%s)  %s\n",
                       tests[i].name,
                       r0, PF(r0 == 0), r1, PF(r1 == 1),
                       (r0 == 0 && r1 == 1) ? "WORKS" : "MUL POISONS");
        }

        /* 2D: exact MAC layout — different test vectors since ADD uses product_lo */
        install_2d();
        /* acc=0xFF..F, mul 1*3 → product_lo=3, ADD overflows → CF=1 */
        uint64_t r2d_1 = invoke1(0xFFFFFFFFFFFFFFFFULL, 1, 3);
        /* acc=0, mul 1*3 → product_lo=3, ADD no overflow → CF=0 */
        uint64_t r2d_0 = invoke1(0, 1, 3);
        printf("  %-42s CF=0→%lu(%s)  CF=1→%lu(%s)  %s\n",
               "2D  Exact MAC: ZEROEXT+MUL,ADD+SETCC",
               r2d_0, PF(r2d_0 == 0), r2d_1, PF(r2d_1 == 1),
               (r2d_0 == 0 && r2d_1 == 1) ? "WORKS" : "MUL POISONS");
}


/* ═══════════════════════════════════════════════════════════════════
 *  GROUP 3: READAFLAGS — Raw flag bits
 *
 *  Returns raw flags word in RAX for inspection.
 * ═══════════════════════════════════════════════════════════════════ */

/* 3A: READAFLAGS with no prior ops */
static void install_3a(void) {
        ucode_t p[] = {
                { READAFLAGS_DR(TMP1, TMP0),
                  NOP, NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 2);
}

/* 3B: READAFLAGS after ADD (overflow) */
static void install_3b(void) {
        ucode_t p[] = {
                { ADD_DSZ64_DRR(TMP0, RAX, RCX),
                  READAFLAGS_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 2);
}

/* 3C: READAFLAGS after MUL */
static void install_3c(void) {
        ucode_t p[] = {
                { MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                  NOP, NOP, NOP_SEQWORD },
                { READAFLAGS_DR(TMP1, TMP0),
                  NOP, NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 3);
}

/* 3D: READAFLAGS after GENARITHFLAGS_RR (same triad) */
static void install_3d(void) {
        ucode_t p[] = {
                { GENARITHFLAGS_RR(RAX, RCX),
                  READAFLAGS_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 2);
}

/* 3E: READAFLAGS after ADD + GENARITHFLAGS (same triad as GENFLAGS) */
static void install_3e(void) {
        ucode_t p[] = {
                { ADD_DSZ64_DRR(TMP0, RAX, RCX),
                  NOP, NOP, NOP_SEQWORD },
                { GENARITHFLAGS_RR(TMP0, RAX),
                  READAFLAGS_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 3);
}

static void decode_flags(uint64_t f) {
        printf("    raw=0x%016" PRIx64 "  CF=%lu PF=%lu AF=%lu ZF=%lu SF=%lu OF=%lu\n",
               f,
               (f >> 0) & 1,   /* CF */
               (f >> 2) & 1,   /* PF */
               (f >> 4) & 1,   /* AF */
               (f >> 6) & 1,   /* ZF */
               (f >> 7) & 1,   /* SF */
               (f >> 11) & 1); /* OF */
}

static void run_group3(void) {
        section("GROUP 3: READAFLAGS — raw flag bits");

        /* Two test vectors */
        uint64_t a_nocarry = 100, c_nocarry = 50;
        uint64_t a_carry = 0xFFFFFFFFFFFFFFFFULL, c_carry = 3;

        struct { const char *name; void (*install)(void); } tests[] = {
                { "3A  no prior ops (baseline)",     install_3a },
                { "3B  after ADD (intra-triad)",     install_3b },
                { "3C  after MUL",                   install_3c },
                { "3D  after GENARITHFLAGS (same T)",install_3d },
                { "3E  after ADD+GENARITHFLAGS",     install_3e },
        };

        for (int i = 0; i < 5; i++) {
                tests[i].install();
                printf("  %s\n", tests[i].name);
                printf("   no-carry (100+50):\n");
                decode_flags(invoke1(a_nocarry, c_nocarry, 0));
                printf("   carry (0xFF..F+3):\n");
                decode_flags(invoke1(a_carry, c_carry, 0));
        }
}


/* ═══════════════════════════════════════════════════════════════════
 *  GROUP 4: ALL CONDITION CODES — Sweep all 16 after ADD overflow
 *
 *  Uses intra-triad ADD+SETCC (from group 1A baseline).
 * ═══════════════════════════════════════════════════════════════════ */

#define MAKE_COND_TEST(suffix, macro_name) \
static void install_cond_##suffix(void) { \
        ucode_t p[] = { \
                { ADD_DSZ64_DRR(TMP0, RAX, RCX), \
                  macro_name(TMP1, TMP0), \
                  NOP, NOP_SEQWORD }, \
                { ZEROEXT_DSZ64_DR(RAX, TMP1), \
                  NOP, NOP, END_SEQWORD } \
        }; \
        do_patch(p, 2); \
}

MAKE_COND_TEST(o,    SETCC_CONDO_DR)
MAKE_COND_TEST(no,   SETCC_CONDNO_DR)
MAKE_COND_TEST(b,    SETCC_CONDB_DR)
MAKE_COND_TEST(nb,   SETCC_CONDNB_DR)
MAKE_COND_TEST(z,    SETCC_CONDZ_DR)
MAKE_COND_TEST(nz,   SETCC_CONDNZ_DR)
MAKE_COND_TEST(be,   SETCC_CONDBE_DR)
MAKE_COND_TEST(nbe,  SETCC_CONDNBE_DR)
MAKE_COND_TEST(s,    SETCC_CONDS_DR)
MAKE_COND_TEST(ns,   SETCC_CONDNS_DR)
MAKE_COND_TEST(p,    SETCC_CONDP_DR)
MAKE_COND_TEST(np,   SETCC_CONDNP_DR)
MAKE_COND_TEST(l,    SETCC_CONDL_DR)
MAKE_COND_TEST(nl,   SETCC_CONDNL_DR)
MAKE_COND_TEST(le,   SETCC_CONDLE_DR)
MAKE_COND_TEST(nle,  SETCC_CONDNLE_DR)

static void run_group4(void) {
        section("GROUP 4: All 16 condition codes (ADD intra-triad)");

        /*
         * Test vectors designed to exercise specific flags:
         *
         * V1: 0xFF..F + 3 = 2     → CF=1, ZF=0, SF=0, OF=0
         * V2: 100 + 50 = 150      → CF=0, ZF=0, SF=0, OF=0
         * V3: 0 + 0 = 0           → CF=0, ZF=1, SF=0, OF=0
         * V4: 0x7F..F + 1 = 0x80..0 → CF=0, ZF=0, SF=1, OF=1 (signed overflow)
         */
        struct { uint64_t a, c; const char *desc; } vecs[] = {
                { 0xFFFFFFFFFFFFFFFFULL, 3,                    "0xFF..F+3 (CF=1)" },
                { 100,                   50,                   "100+50    (all=0)" },
                { 0,                     0,                    "0+0       (ZF=1)"  },
                { 0x7FFFFFFFFFFFFFFFULL, 1,                    "0x7F..F+1 (SF=1,OF=1)" },
        };

        struct { const char *name; void (*install)(void); } conds[] = {
                { "CONDO  (OF=1)",    install_cond_o },
                { "CONDNO (OF=0)",    install_cond_no },
                { "CONDB  (CF=1)",    install_cond_b },
                { "CONDNB (CF=0)",    install_cond_nb },
                { "CONDZ  (ZF=1)",    install_cond_z },
                { "CONDNZ (ZF=0)",    install_cond_nz },
                { "CONDBE (CF|ZF)",   install_cond_be },
                { "CONDNBE(!CF&!ZF)", install_cond_nbe },
                { "CONDS  (SF=1)",    install_cond_s },
                { "CONDNS (SF=0)",    install_cond_ns },
                { "CONDP  (PF=1)",    install_cond_p },
                { "CONDNP (PF=0)",    install_cond_np },
                { "CONDL  (SF!=OF)",  install_cond_l },
                { "CONDNL (SF==OF)",  install_cond_nl },
                { "CONDLE (ZF|SF!=OF)", install_cond_le },
                { "CONDNLE(!ZF&SF==OF)", install_cond_nle },
        };

        printf("  %-22s", "");
        for (int v = 0; v < 4; v++)
                printf("  %-14s", vecs[v].desc);
        printf("\n");

        for (int c = 0; c < 16; c++) {
                conds[c].install();
                printf("  %-22s", conds[c].name);
                for (int v = 0; v < 4; v++) {
                        uint64_t r = invoke1(vecs[v].a, vecs[v].c, 0);
                        printf("  %-14lu", r);
                }
                printf("\n");
        }

        printf("\n  If flags work correctly, expected matrix:\n");
        printf("  %-22s  %-14s  %-14s  %-14s  %-14s\n", "", "CF=1", "all=0", "ZF=1", "SF=1,OF=1");
        printf("  %-22s  %-14s  %-14s  %-14s  %-14s\n", "CONDO  (OF=1)",    "0","0","0","1");
        printf("  %-22s  %-14s  %-14s  %-14s  %-14s\n", "CONDB  (CF=1)",    "1","0","0","0");
        printf("  %-22s  %-14s  %-14s  %-14s  %-14s\n", "CONDZ  (ZF=1)",    "0","0","1","0");
        printf("  %-22s  %-14s  %-14s  %-14s  %-14s\n", "CONDS  (SF=1)",    "0","0","0","1");
}


/* ═══════════════════════════════════════════════════════════════════
 *  GROUP 5: REPEATED INVOCATION — EFLAGS restore by END_SEQWORD
 *
 *  Test if carry detection works on 2nd vmwrite in same asm block.
 * ═══════════════════════════════════════════════════════════════════ */

static void run_group5(void) {
        section("GROUP 5: Repeated invocation (EFLAGS restore)");

        /* Use 1A (ADD+SETCC intra-triad, known working for single call) */
        install_1a();

        /* Single call: should work */
        uint64_t r1 = invoke1(0xFFFFFFFFFFFFFFFFULL, 3, 0);
        printf("  Single vmwrite (CF=1):             %lu  %s\n", r1, PF(r1 == 1));

        /* Two calls: both should detect carry */
        /* First: 0xFF..F + 3 → CF=1, result=2 (returned in RAX)
         * Second: 2 + 0xFF..F → CF=1 */
        uint64_t r2 = invoke2(0xFFFFFFFFFFFFFFFFULL,
                              3, 0,     /* 1st: 0xFF..F + 3 = 2, CF=1 */
                              0xFFFFFFFFFFFFFFFFULL, 0); /* 2nd: 2 + 0xFF..F (wait, RAX=carry from 1st...) */
        /* Actually invoke2 doesn't set RAX between calls — hook returns SETCC in RAX.
         * After 1st call: RAX = 1 (the carry bit). 2nd call: ADD(1 + 0xFF..F) = 0, CF=1 */
        printf("  2nd vmwrite (CF=1 both):           %lu  %s\n", r2, PF(r2 == 1));

        /* Two calls: no carry on first, carry on second */
        /* 1st: ADD(0 + 5) = 5, CF=0 → RAX=0. 2nd: ADD(0 + 0xFF..F) = 0xFF..F, CF=0 */
        /* Need to be careful — RAX after 1st hook = carry result, not sum */
        /* So let's make it clearer with a hook that returns sum in RBX and carry in RAX */
        uint64_t r3 = invoke2(0, 5, 0,
                              0xFFFFFFFFFFFFFFFFULL, 0);
        /* 1st: ADD(0 + 5) = 5, RAX=0(CF=0). 2nd: ADD(0 + 0xFF..F) → 0xFF..F, CF=0 */
        /* Actually this won't trigger carry. Need RAX=0xFF..F going into 2nd call. */
        printf("  2nd vmwrite (CF=0 then CF=0):      %lu  %s\n", r3, PF(r3 == 0));

        /* Better repeated test: install MAC-like hook that keeps accumulator */
        printf("\n  (For definitive repeated-carry test, see MAC128 test 3/4 results)\n");
}


/* ═══════════════════════════════════════════════════════════════════
 *  GROUP 6: ALTERNATIVE FLAG CONSUMERS
 *
 *  Test CMOVCC, SELECTCC, UJMPCC with intra-triad ADD as flag source.
 * ═══════════════════════════════════════════════════════════════════ */

/* 6A: ADD + CMOVCC_CONDB same triad */
static void install_6a(void) {
        ucode_t p[] = {
                { MOVE_DSZ64_DI(TMP2, 0),
                  NOP, NOP, NOP_SEQWORD },
                { ADD_DSZ64_DRR(TMP0, RAX, RCX),
                  CMOVCC_DSZ64_CONDB_DRI(TMP2, TMP2, 1),  /* TMP2 = CF ? 1 : TMP2(0) */
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP2),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 3);
}

/* 6B: ADD + SELECTCC_CONDB same triad */
static void install_6b(void) {
        ucode_t p[] = {
                { MOVE_DSZ64_DI(TMP2, 0),
                  NOP, NOP, NOP_SEQWORD },
                { ADD_DSZ64_DRR(TMP0, RAX, RCX),
                  SELECTCC_DSZ64_CONDB_DRI(TMP2, TMP2, 1),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP2),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 3);
}

/* 6C: ADD + UJMPCC_CONDB (jump over sentinel) */
static void install_6c(void) {
        /*
         * 0x7c00: ADD, set RBX=0 (default: no-carry)
         * 0x7c01: UJMPCC_CONDB → 0x7c03 (if CF, skip sentinel)
         * 0x7c02: write RBX=0xBBBB (no-carry sentinel)
         * 0x7c03: ZEROEXT RAX, RBX | END
         */
        ucode_t p[] = {
                { ADD_DSZ64_DRR(TMP0, RAX, RCX),
                  MOVE_DSZ64_DI(RBX, 0),
                  NOP, NOP_SEQWORD },
                { UJMPCC_DIRECT_NOTTAKEN_CONDB_RI(RCX, 0x7c03),
                  NOP, NOP, NOP_SEQWORD },
                { MOVE_DSZ64_DI(RBX, 0xBBBB),
                  NOP, NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, RBX),
                  NOP, NOP, END_SEQWORD }
        };
        do_patch(p, 4);
}

static void run_group6(void) {
        section("GROUP 6: Alternative flag consumers (intra-triad ADD)");

        uint64_t cf0_a = 100, cf0_c = 50;
        uint64_t cf1_a = 0xFFFFFFFFFFFFFFFFULL, cf1_c = 3;

        struct { const char *name; void (*install)(void); uint64_t cf0_expect; uint64_t cf1_expect; } tests[] = {
                { "6A  ADD+CMOVCC_CONDB",    install_6a, 0, 1 },
                { "6B  ADD+SELECTCC_CONDB",  install_6b, 0, 1 },
                { "6C  ADD+UJMPCC_CONDB",    install_6c, 0xBBBB, 0 },
        };

        for (int i = 0; i < 3; i++) {
                tests[i].install();
                uint64_t r0 = invoke1(cf0_a, cf0_c, 0);
                uint64_t r1 = invoke1(cf1_a, cf1_c, 0);
                int ok0 = (r0 == tests[i].cf0_expect);
                int ok1 = (r1 == tests[i].cf1_expect);
                printf("  %-42s CF=0→0x%lx(%s)  CF=1→0x%lx(%s)  %s\n",
                       tests[i].name,
                       r0, PF(ok0), r1, PF(ok1),
                       (ok0 && ok1) ? "WORKS" : "BROKEN");
        }
}


/* ═══════════════════════════════════════════════════════════════════
 *  GROUP 7: MOVEINSERTFLGS / MOVEMERGEFLGS
 *
 *  Test if these set flags that SETCC can read.
 * ═══════════════════════════════════════════════════════════════════ */

/* 7A: MOVEINSERTFLGS + SETCC intra-triad
 * Returns: RAX=SETCC result, RBX=MOVEINSERTFLGS output (what did it compute?) */
static void install_7a(void) {
        ucode_t p[] = {
                { MOVEINSERTFLGS_DSZ64_DRR(TMP0, RAX, RCX),
                  SETCC_CONDB_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  ZEROEXT_DSZ64_DR(RBX, TMP0),
                  NOP, END_SEQWORD }
        };
        do_patch(p, 2);
}

/* 7B: MOVEMERGEFLGS + SETCC intra-triad */
static void install_7b(void) {
        ucode_t p[] = {
                { MOVEMERGEFLGS_DSZ64_DRR(TMP0, RAX, RCX),
                  SETCC_CONDB_DR(TMP1, TMP0),
                  NOP, NOP_SEQWORD },
                { ZEROEXT_DSZ64_DR(RAX, TMP1),
                  ZEROEXT_DSZ64_DR(RBX, TMP0),
                  NOP, END_SEQWORD }
        };
        do_patch(p, 2);
}

static void run_group7(void) {
        section("GROUP 7: MOVEINSERTFLGS / MOVEMERGEFLGS");

        uint64_t cf0_a = 100, cf0_c = 50;
        uint64_t cf1_a = 0xFFFFFFFFFFFFFFFFULL, cf1_c = 3;

        struct { const char *name; void (*install)(void); } tests[] = {
                { "7A  MOVEINSERTFLGS+SETCC",  install_7a },
                { "7B  MOVEMERGEFLGS+SETCC",   install_7b },
        };

        for (int i = 0; i < 2; i++) {
                tests[i].install();
                uint64_t rax0, rbx0, rax1, rbx1;
                invoke1_ab(cf0_a, cf0_c, 0, &rax0, &rbx0);
                invoke1_ab(cf1_a, cf1_c, 0, &rax1, &rbx1);
                printf("  %s\n", tests[i].name);
                printf("    no-carry: SETCC=%lu  dst=0x%lx", rax0, rbx0);
                if (rbx0 == 150) printf(" (A+B)");
                else if (rbx0 == 100) printf(" (A=src0)");
                else if (rbx0 == 50) printf(" (B=src1)");
                printf("\n");
                printf("    carry:    SETCC=%lu  dst=0x%lx", rax1, rbx1);
                if (rbx1 == 0x2) printf(" (A+B, wrapped)");
                else if (rbx1 == cf1_a) printf(" (A=src0)");
                else if (rbx1 == cf1_c) printf(" (B=src1)");
                printf("\n");
                printf("    -> %s\n",
                       (rax0 == 0 && rax1 == 1) ? "FLAGS WORK" : "FLAGS BROKEN");
        }
}


/* ═══════════════════════════════════════════════════════════════════
 *  MAIN
 * ═══════════════════════════════════════════════════════════════════ */

int main(void) {
        printf("╔════════════════════════════════════════════════════════╗\n");
        printf("║  ULTIMATE MICROCODE FLAG TEST — Goldmont               ║\n");
        printf("╚════════════════════════════════════════════════════════╝\n");

        run_group1();
        run_group2();
        run_group3();
        run_group4();
        run_group5();
        run_group6();
        run_group7();

        section("CONCLUSIONS");
        printf("  Read the results above to determine:\n\n");
        printf("  Q1. ADD→SETCC intra-triad:  Check 1A (should WORK)\n");
        printf("  Q2. MUL poisons flags:      Check 2A-2D vs 1A\n");
        printf("      If 1A works but 2D fails → MUL poisons\n");
        printf("  Q3. GENARITHFLAGS helps:     Check 1D/1E vs 1C\n");
        printf("      If 1D works but 1F fails → same-triad only\n");
        printf("  Q4. Raw flag bits:           Check Group 3\n");
        printf("      If 3B carry≠3B no-carry → ADD sets real flags\n");
        printf("      If 3C differs from 3A   → MUL changes flags\n");
        printf("  Q5. Condition codes:         Check Group 4 matrix\n");
        printf("      Compare actual vs expected to find working CCs\n");
        printf("  Q6. Repeated invocation:     Check Group 5\n");
        printf("      If 2nd call fails → END_SEQWORD restores EFLAGS\n");
        printf("  Q7. MOVEINSERTFLGS:          Check Group 7\n");
        printf("      If 7A SETCC=1 on carry → it's ADD-with-flags\n");
        printf("  Q8. Alt consumers:           Check Group 6\n");
        printf("      If 6C works but 6A/6B fail → different flag bus\n\n");

        printf("  KEY DECISION TREE:\n");
        printf("  ┌─ 1A works, 2D works → 3-triad MAC possible\n");
        printf("  ├─ 1A works, 2D fails → MUL poisons; need 6-triad\n");
        printf("  │   └─ Unless 2B works → NOP gap clears MUL flags (5-triad)\n");
        printf("  │   └─ Unless 2C works → GENARITHFLAGS clears MUL (5-triad)\n");
        printf("  ├─ 1A fails, 1D works → need explicit GENARITHFLAGS\n");
        printf("  └─ All fail → 6-triad arithmetic carry is the only path\n\n");

        return 0;
}
