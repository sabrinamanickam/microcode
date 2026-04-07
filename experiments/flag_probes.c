/*
 * flag_probes.c — Discover how microcode flags work on Goldmont
 *
 * KNOWN:
 *   - ADD_DSZ64 computes correct results
 *   - SETCC_CONDB after ADD always returns 0
 *   - CMOVCC_CONDB after ADD always returns 1 (fires unconditionally)
 *   → Neither reads flags produced by microcode ADD
 *
 * HYPOTHESIS: Microcode has a two-domain flag architecture:
 *   - ALU ops (ADD, SUB) compute results but don't auto-update
 *     the flag state that conditional ops (SETCC, CMOVCC, UJMPCC) read
 *   - GENARITHFLAGS explicitly generates/publishes flags
 *   - MOVEINSERTFLGS might be a flag-setting ALU operation
 *
 * STRATEGY: Test every plausible flag-generation mechanism.
 *
 * Test vector: 0xFFFFFFFFFFFFFFFF + 3 = 0x0000000000000002, CF=1
 *   We set RAX = 0xFFFFFFFFFFFFFFFF, add 3, check if we can observe CF.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

static void do_patch(ucode_t *patch, int n_triads) {
        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, patch, n_triads);
        hook_match_and_patch(0, 0x0428, 0x7c00);
}

/*
 * Helper: run rdrand with RCX=val, read back RAX, RBX, R8
 * The patch should arrange outputs in those regs.
 */
static void run_test(uint64_t rcx_in, uint64_t rdx_in,
                     uint64_t *rax_out, uint64_t *rbx_out, uint64_t *r8_out) {
        asm volatile(
                "mov rcx, %[c]\n\t"
                "mov rdx, %[d]\n\t"
                "rdrand rax\n\t"
                "mov %[a], rax\n\t"
                "mov %[b], rbx\n\t"
                "mov %[r], r8\n\t"
                : [a] "=r"(*rax_out), [b] "=r"(*rbx_out), [r] "=r"(*r8_out)
                : [c] "r"(rcx_in), [d] "r"(rdx_in)
                : "rax", "rbx", "rcx", "rdx", "r8"
        );
}

static void print_result(const char *label, uint64_t val, uint64_t expect) {
        int ok = (val == expect);
        printf("  %-20s = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")%s\n",
               label, val, expect, ok ? " ✓" : " ✗");
}


/* ══════════════════════════════════════════════════════════════════
 *  PROBE 1: GENARITHFLAGS_RR(src0, src1) after ADD, then SETCC
 *
 *  Theory: GENARITHFLAGS_RR recomputes flags for src0+src1
 *  and publishes them so SETCC can read CF.
 *
 *  We need the ORIGINAL operands (before ADD overwrites dst).
 *  So: save old_RAX to TMP3, ADD, then GENARITHFLAGS_RR(TMP3, RDX).
 * ══════════════════════════════════════════════════════════════════ */
void probe1_genarithflags_rr_then_setcc(void) {
        ucode_t patch[] = {
                /* Triad 0: Save acc_lo, do ADD */
                {
                        MOVE_DSZ64_DR(TMP3, RAX),           /* TMP3 = old acc_lo */
                        MOVE_DSZ64_DI(TMP2, 0),             /* pre-zero carry */
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: ADD acc_lo += RDX */
                {
                        ADD_DSZ64_DRR(RAX, RAX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 2: Generate flags for old_acc + RDX */
                {
                        GENARITHFLAGS_RR(TMP3, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 3: Read CF via SETCC */
                {
                        SETCC_CONDB_DR(TMP2, TMP2),         /* TMP2 = CF? */
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 4: Output */
                {
                        MOVE_DSZ64_DR(RBX, TMP2),           /* RBX = carry */
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch, 5);

        uint64_t rax, rbx, r8;
        printf("=== PROBE 1: GENARITHFLAGS_RR → SETCC ===\n");

        /* CF=1 case: 0xFFFF...F + 3 overflows */
        run_test(0, 3, &rax, &rbx, &r8);  /* RAX set via patch from asm */
        /* Actually, RAX comes from the asm "rdrand rax" trigger... 
         * We need to set RAX before rdrand. Let me use RCX as acc. */

        printf("  (see probe1b for correct test)\n\n");
}

/*
 * Better version: use TMP registers to avoid rdrand clobbering RAX.
 * Load test values from RCX and RDX, compute in TMPs, report via RAX/RBX.
 */


/* ══════════════════════════════════════════════════════════════════
 *  PROBE 1b: Cleaner GENARITHFLAGS_RR test
 *
 *  Input:  RCX = operand A,  RDX = operand B
 *  Compute: TMP0 = A + B
 *  Generate flags for A + B
 *  Output:  RAX = sum,  RBX = carry (0 or 1)
 * ══════════════════════════════════════════════════════════════════ */
void probe1b_genarithflags_rr(void) {
        ucode_t patch[] = {
                /* Triad 0: Compute sum */
                {
                        ADD_DSZ64_DRR(TMP0, RCX, RDX),      /* TMP0 = A + B */
                        MOVE_DSZ64_DI(TMP2, 0),              /* pre-zero carry */
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: Generate flags for A+B using original operands */
                {
                        GENARITHFLAGS_RR(RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 2: Read CF */
                {
                        SETCC_CONDB_DR(TMP2, TMP2),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 3: Output */
                {
                        MOVE_DSZ64_DR(RAX, TMP0),            /* RAX = sum */
                        MOVE_DSZ64_DR(RBX, TMP2),            /* RBX = carry */
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch, 4);

        uint64_t rax, rbx, r8;
        printf("=== PROBE 1b: GENARITHFLAGS_RR(A,B) → SETCC ===\n");

        /* CF=1: overflow */
        run_test(0xFFFFFFFFFFFFFFFFULL, 3, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 0x2);
        print_result("RBX (carry)", rbx, 0x1);

        /* CF=0: no overflow */
        run_test(100, 50, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 150);
        print_result("RBX (carry)", rbx, 0x0);
        printf("\n");
}


/* ══════════════════════════════════════════════════════════════════
 *  PROBE 2: GENARITHFLAGS_RR → CMOVCC (instead of SETCC)
 * ══════════════════════════════════════════════════════════════════ */
void probe2_genarithflags_rr_cmovcc(void) {
        ucode_t patch[] = {
                {
                        ADD_DSZ64_DRR(TMP0, RCX, RDX),
                        MOVE_DSZ64_DI(TMP2, 0),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        GENARITHFLAGS_RR(RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        CMOVCC_DSZ64_CONDB_DRI(TMP2, TMP2, 1),  /* TMP2=CF?1:0 */
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        MOVE_DSZ64_DR(RAX, TMP0),
                        MOVE_DSZ64_DR(RBX, TMP2),
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch, 4);

        uint64_t rax, rbx, r8;
        printf("=== PROBE 2: GENARITHFLAGS_RR(A,B) → CMOVCC ===\n");

        run_test(0xFFFFFFFFFFFFFFFFULL, 3, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 0x2);
        print_result("RBX (carry)", rbx, 0x1);

        run_test(100, 50, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 150);
        print_result("RBX (carry)", rbx, 0x0);
        printf("\n");
}


/* ══════════════════════════════════════════════════════════════════
 *  PROBE 3: GENARITHFLAGS_RR → SELECTCC
 *
 *  SELECTCC_DSZ64_CONDB_DRI(dst, src, imm):
 *    Maybe: dst = CF ? imm : src
 *    Or:    dst = CF ? src : imm
 *  Test both interpretations.
 * ══════════════════════════════════════════════════════════════════ */
void probe3_genarithflags_rr_selectcc(void) {
        ucode_t patch[] = {
                {
                        ADD_DSZ64_DRR(TMP0, RCX, RDX),
                        MOVE_DSZ64_DI(TMP2, 0xAA),        /* sentinel */
                        NOP,
                        NOP_SEQWORD
                },
                {
                        GENARITHFLAGS_RR(RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        SELECTCC_DSZ64_CONDB_DRI(TMP2, TMP2, 0xBB),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        MOVE_DSZ64_DR(RAX, TMP0),
                        MOVE_DSZ64_DR(RBX, TMP2),
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch, 4);

        uint64_t rax, rbx, r8;
        printf("=== PROBE 3: GENARITHFLAGS_RR(A,B) → SELECTCC ===\n");
        printf("  SELECTCC_CONDB(dst=0xAA, imm=0xBB)\n");

        run_test(0xFFFFFFFFFFFFFFFFULL, 3, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 0x2);
        printf("  RBX (selectcc)      = 0x%016" PRIx64, rbx);
        if (rbx == 0xBB) printf("  ← CF=1: dst=imm (CF?imm:src)");
        else if (rbx == 0xAA) printf("  ← CF=1: dst=src (CF?src:imm)");
        else printf("  ← unexpected");
        printf("\n");

        run_test(100, 50, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 150);
        printf("  RBX (selectcc)      = 0x%016" PRIx64, rbx);
        if (rbx == 0xAA) printf("  ← CF=0: dst=src");
        else if (rbx == 0xBB) printf("  ← CF=0: dst=imm");
        else printf("  ← unexpected");
        printf("\n\n");
}


/* ══════════════════════════════════════════════════════════════════
 *  PROBE 4: GENARITHFLAGS in SAME triad as ADD
 *
 *  Within a single triad, all reads happen at start (old values).
 *  So ADD_DRR(TMP0, RCX, RDX) and GENARITHFLAGS_RR(RCX, RDX)
 *  both read old RCX and old RDX — no conflict.
 * ══════════════════════════════════════════════════════════════════ */
void probe4_same_triad(void) {
        ucode_t patch[] = {
                /* ADD + GENARITHFLAGS in same triad */
                {
                        ADD_DSZ64_DRR(TMP0, RCX, RDX),
                        GENARITHFLAGS_RR(RCX, RDX),
                        MOVE_DSZ64_DI(TMP2, 0),
                        NOP_SEQWORD
                },
                {
                        SETCC_CONDB_DR(TMP2, TMP2),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        MOVE_DSZ64_DR(RAX, TMP0),
                        MOVE_DSZ64_DR(RBX, TMP2),
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch, 3);

        uint64_t rax, rbx, r8;
        printf("=== PROBE 4: ADD + GENARITHFLAGS same triad → SETCC ===\n");

        run_test(0xFFFFFFFFFFFFFFFFULL, 3, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 0x2);
        print_result("RBX (carry)", rbx, 0x1);

        run_test(100, 50, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 150);
        print_result("RBX (carry)", rbx, 0x0);
        printf("\n");
}


/* ══════════════════════════════════════════════════════════════════
 *  PROBE 5: MOVEINSERTFLGS as an ADD-with-flags
 *
 *  Theory: MOVEINSERTFLGS_DSZ64_DRR(dst, src0, src1)
 *  might compute dst = src0 + src1 AND set flags.
 *  Or it might be dst = src0, flags = f(src1).
 *  Or dst = src0, insert flags from src1 as a bitmask.
 *
 *  Test: if it's ADD+flags, then dst should equal src0+src1.
 *  If it's MOV+flags, dst should equal src0.
 * ══════════════════════════════════════════════════════════════════ */
void probe5_moveinsertflgs(void) {
        ucode_t patch[] = {
                /* Do MOVEINSERTFLGS instead of ADD */
                {
                        MOVEINSERTFLGS_DSZ64_DRR(TMP0, RCX, RDX),
                        MOVE_DSZ64_DI(TMP2, 0),
                        NOP,
                        NOP_SEQWORD
                },
                /* Try reading flags immediately */
                {
                        SETCC_CONDB_DR(TMP2, TMP2),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        MOVE_DSZ64_DR(RAX, TMP0),
                        MOVE_DSZ64_DR(RBX, TMP2),
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch, 3);

        uint64_t rax, rbx, r8;
        printf("=== PROBE 5: MOVEINSERTFLGS_DRR(dst, A, B) → SETCC ===\n");

        run_test(0xFFFFFFFFFFFFFFFFULL, 3, &rax, &rbx, &r8);
        printf("  RAX (TMP0)          = 0x%016" PRIx64, rax);
        if (rax == 0x2) printf("  ← dst=A+B (ADD+flags!)");
        else if (rax == 0xFFFFFFFFFFFFFFFFULL) printf("  ← dst=A (MOV+flags)");
        else printf("  ← unexpected (0x%" PRIx64 ")", rax);
        printf("\n");
        printf("  RBX (carry)         = 0x%016" PRIx64, rbx);
        if (rbx == 1) printf("  ← CF=1 ✓");
        else printf("  ← CF not set");
        printf("\n");

        run_test(100, 50, &rax, &rbx, &r8);
        printf("  RAX (TMP0)          = 0x%016" PRIx64, rax);
        if (rax == 150) printf("  ← dst=A+B");
        else if (rax == 100) printf("  ← dst=A");
        printf("\n");
        printf("  RBX (carry)         = 0x%016" PRIx64, rbx);
        if (rbx == 0) printf("  ← CF=0 ✓");
        else printf("  ← CF wrong");
        printf("\n\n");
}


/* ══════════════════════════════════════════════════════════════════
 *  PROBE 6: READAFLAGS — dump raw flag register
 *
 *  READAFLAGS_DR(dst, src): read flags into dst.
 *  The 'src' operand is unclear — try with a dummy register.
 *  Do this after ADD, and separately after ADD+GENARITHFLAGS,
 *  to see what changes.
 *
 *  x86 RFLAGS CF = bit 0.
 * ══════════════════════════════════════════════════════════════════ */
void probe6_readaflags(void) {
        /* 6a: READAFLAGS after bare ADD (no GENARITHFLAGS) */
        ucode_t patch_a[] = {
                {
                        ADD_DSZ64_DRR(TMP0, RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        READAFLAGS_DR(TMP1, TMP1),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        MOVE_DSZ64_DR(RAX, TMP0),
                        MOVE_DSZ64_DR(RBX, TMP1),
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch_a, 3);

        uint64_t rax, rbx, r8;
        printf("=== PROBE 6a: READAFLAGS after bare ADD ===\n");

        run_test(0xFFFFFFFFFFFFFFFFULL, 3, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 0x2);
        printf("  RBX (flags)         = 0x%016" PRIx64 "  (bit0=CF=%d)\n",
               rbx, (int)(rbx & 1));

        run_test(100, 50, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 150);
        printf("  RBX (flags)         = 0x%016" PRIx64 "  (bit0=CF=%d)\n\n",
               rbx, (int)(rbx & 1));


        /* 6b: READAFLAGS after ADD + GENARITHFLAGS */
        ucode_t patch_b[] = {
                {
                        ADD_DSZ64_DRR(TMP0, RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        GENARITHFLAGS_RR(RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        READAFLAGS_DR(TMP1, TMP1),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        MOVE_DSZ64_DR(RAX, TMP0),
                        MOVE_DSZ64_DR(RBX, TMP1),
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch_b, 4);

        printf("=== PROBE 6b: READAFLAGS after ADD + GENARITHFLAGS ===\n");

        run_test(0xFFFFFFFFFFFFFFFFULL, 3, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 0x2);
        printf("  RBX (flags)         = 0x%016" PRIx64 "  (bit0=CF=%d)\n",
               rbx, (int)(rbx & 1));

        run_test(100, 50, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 150);
        printf("  RBX (flags)         = 0x%016" PRIx64 "  (bit0=CF=%d)\n\n",
               rbx, (int)(rbx & 1));
}


/* ══════════════════════════════════════════════════════════════════
 *  PROBE 7: GENARITHFLAGS_R(result) — test-style flags on result
 *
 *  Theory: Maybe GENARITHFLAGS_R generates flags by TESTING the
 *  result value (like x86 TEST). This wouldn't give CF for addition
 *  but could give ZF, SF. Testing to see if it updates anything.
 * ══════════════════════════════════════════════════════════════════ */
void probe7_genarithflags_r(void) {
        ucode_t patch[] = {
                {
                        ADD_DSZ64_DRR(TMP0, RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Generate flags from just the result */
                {
                        GENARITHFLAGS_R(TMP0),
                        MOVE_DSZ64_DI(TMP2, 0),
                        NOP,
                        NOP_SEQWORD
                },
                /* Check ZF via SETCC_CONDZ — if sum is 0, ZF should be 1 */
                {
                        SETCC_CONDZ_DR(TMP2, TMP2),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        MOVE_DSZ64_DR(RAX, TMP0),
                        MOVE_DSZ64_DR(RBX, TMP2),
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch, 4);

        uint64_t rax, rbx, r8;
        printf("=== PROBE 7: GENARITHFLAGS_R(result) → SETCC_CONDZ ===\n");

        /* ZF=1: 0 + 0 = 0 */
        run_test(0, 0, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 0);
        printf("  RBX (ZF)            = %" PRIu64 "  (expect 1 if ZF works)\n", rbx);

        /* ZF=0: result non-zero */
        run_test(100, 50, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 150);
        printf("  RBX (ZF)            = %" PRIu64 "  (expect 0 if ZF works)\n\n", rbx);
}


/* ══════════════════════════════════════════════════════════════════
 *  PROBE 8: GENARITHFLAGS_IR(0, src) — flags for 0+src
 *
 *  If GENARITHFLAGS_IR(imm, src) generates flags for imm+src,
 *  then GENARITHFLAGS_IR(0, result) gives TEST-like flags.
 *  More importantly, GENARITHFLAGS_IR with the operands of our
 *  add could regenerate the correct carry.
 * ══════════════════════════════════════════════════════════════════ */
void probe8_genarithflags_ir(void) {
        /*
         * Can't encode 0xFFFFFFFFFFFFFFFF as immediate.
         * Instead: do A + B where B is small, use IR(A_low_bits, B)
         * or just test if it generates ZF for 0+0.
         */
        ucode_t patch[] = {
                /* GENARITHFLAGS_IR(0, RCX) — flags for 0 + RCX */
                {
                        GENARITHFLAGS_IR(0, RCX),
                        MOVE_DSZ64_DI(TMP2, 0),
                        MOVE_DSZ64_DI(TMP3, 0),
                        NOP_SEQWORD
                },
                {
                        SETCC_CONDZ_DR(TMP2, TMP2),       /* ZF */
                        SETCC_CONDB_DR(TMP3, TMP3),       /* CF */
                        NOP,
                        NOP_SEQWORD
                },
                {
                        MOVE_DSZ64_DR(RAX, TMP2),          /* ZF result */
                        MOVE_DSZ64_DR(RBX, TMP3),          /* CF result */
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch, 3);

        uint64_t rax, rbx, r8;
        printf("=== PROBE 8: GENARITHFLAGS_IR(0, src) ===\n");

        /* src=0: 0+0=0, ZF=1, CF=0 */
        run_test(0, 0, &rax, &rbx, &r8);
        printf("  src=0: ZF=%" PRIu64 " (expect 1), CF=%" PRIu64 " (expect 0)\n",
               rax, rbx);

        /* src=1: 0+1=1, ZF=0, CF=0 */
        run_test(1, 0, &rax, &rbx, &r8);
        printf("  src=1: ZF=%" PRIu64 " (expect 0), CF=%" PRIu64 " (expect 0)\n\n",
               rax, rbx);
}


/* ══════════════════════════════════════════════════════════════════
 *  PROBE 9: UJMPCC — test if microcode conditional jump reads flags
 *
 *  If GENARITHFLAGS works, UJMPCC should be able to branch on CF.
 *  This tests a different consumer than SETCC/CMOVCC.
 *
 *  Strategy: GENARITHFLAGS, then UJMPCC_CONDB to skip a triad
 *  that writes a sentinel. If CF=1, sentinel is skipped.
 * ══════════════════════════════════════════════════════════════════ */
void probe9_ujmpcc(void) {
        /*
         * Layout:
         *   Triad at 0x7c00: ADD + GENARITHFLAGS
         *   Triad at 0x7c01: UJMPCC_CONDB → 0x7c03 (skip sentinel)
         *   Triad at 0x7c02: write sentinel 0xDEAD to RBX
         *   Triad at 0x7c03: output + END
         */
        ucode_t patch[] = {
                /* 0x7c00: Compute and gen flags */
                {
                        ADD_DSZ64_DRR(TMP0, RCX, RDX),
                        GENARITHFLAGS_RR(RCX, RDX),
                        MOVE_DSZ64_DI(RBX, 0),            /* RBX=0 (no carry path) */
                        NOP_SEQWORD
                },
                /* 0x7c01: If CF, jump to 0x7c03 (skip sentinel) */
                {
                        UJMPCC_DIRECT_NOTTAKEN_CONDB_RI(RCX, 0x7c03),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* 0x7c02: No-carry path: write sentinel */
                {
                        MOVE_DSZ64_DI(RBX, 0xBBBB),       /* sentinel = no carry */
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* 0x7c03: Output */
                {
                        MOVE_DSZ64_DR(RAX, TMP0),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(patch, 4);

        uint64_t rax, rbx, r8;
        printf("=== PROBE 9: GENARITHFLAGS → UJMPCC_CONDB ===\n");

        /* CF=1: should skip sentinel, RBX stays 0 */
        run_test(0xFFFFFFFFFFFFFFFFULL, 3, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 0x2);
        printf("  RBX                 = 0x%" PRIx64, rbx);
        if (rbx == 0) printf("  ← jumped (CF=1 seen!) ✓");
        else if (rbx == 0xBBBB) printf("  ← fell through (CF not seen)");
        else printf("  ← unexpected");
        printf("\n");

        /* CF=0: should NOT skip, RBX = 0xBBBB */
        run_test(100, 50, &rax, &rbx, &r8);
        print_result("RAX (sum)", rax, 150);
        printf("  RBX                 = 0x%" PRIx64, rbx);
        if (rbx == 0xBBBB) printf("  ← fell through (CF=0) ✓");
        else if (rbx == 0) printf("  ← jumped (wrong!)");
        else printf("  ← unexpected");
        printf("\n\n");
}


/* ══════════════════════════════════════════════════════════════════ */

int main(void) {
        printf("╔════════════════════════════════════════════════════╗\n");
        printf("║  FLAG PROBES — Goldmont Microcode Flag Discovery   ║\n");
        printf("╚════════════════════════════════════════════════════╝\n\n");

        probe1b_genarithflags_rr();
        probe2_genarithflags_rr_cmovcc();
        probe3_genarithflags_rr_selectcc();
        probe4_same_triad();
        probe5_moveinsertflgs();
        probe6_readaflags();
        probe7_genarithflags_r();
        probe8_genarithflags_ir();
        probe9_ujmpcc();

        printf("════════════════════════════════════════════════════\n");
        printf("INTERPRETATION GUIDE:\n\n");
        printf("If probe 1b/2/3/4 pass → GENARITHFLAGS_RR publishes\n");
        printf("  carry flags; use it after ADD in MAC128.\n\n");
        printf("If probe 5 passes → MOVEINSERTFLGS IS the flag-\n");
        printf("  setting ADD; replace ADD_DSZ64 with it.\n\n");
        printf("If probe 6 shows CF bit changing → READAFLAGS can\n");
        printf("  extract carry directly; AND with 1 to get it.\n\n");
        printf("If probe 7/8 pass for ZF → GENARITHFLAGS works but\n");
        printf("  only for test-style flags (ZF,SF), not carry.\n\n");
        printf("If probe 9 passes → UJMPCC reads flags from\n");
        printf("  GENARITHFLAGS; use jump-based carry extraction.\n");
        printf("════════════════════════════════════════════════════\n");

        return 0;
}
