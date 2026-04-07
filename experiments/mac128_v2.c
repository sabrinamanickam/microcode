/*
 * mac128_v2.c — Corrected MAC128 for Goldmont Microcode
 *
 * FINDINGS FROM v1 TESTING:
 *   MUL_DSZ64_DRR(dst, src0, src1):
 *     dst  = HIGH 64 bits of (src0 × src1)     ← CONFIRMED
 *     TMP1 = does NOT hold the low 64 bits      ← CONFIRMED (stale garbage)
 *     Low 64 bits location = UNKNOWN             ← MUST PROBE
 *
 * This file contains:
 *   1. probe_mul_low()  — Discovers where the low 64 bits land
 *   2. install_mac128() — Corrected MAC128 (once low location is known)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"


/* ══════════════════════════════════════════════════════════════════
 *  PHASE 1: PROBE — Find the low 64 bits of MUL_DSZ64
 *
 *  Strategy: Set ALL registers to unique sentinel values,
 *  execute MUL, then read everything back. Whatever register
 *  changed to the expected low product tells us where it went.
 *
 *  Test: 0x0000000100000002 × 0x0000000300000004
 *       = 0x00000003_00000004_00000008 (fits cleanly)
 *       Full: 0x000000030000000A00000008
 *       Let me compute properly:
 *       0x100000002 * 0x300000004
 *       = 0x300000004_00000000 + 0x200000000*0x300000004
 *       Actually let me just use simple values.
 *       3 * 5 = 15: high=0, low=15 (0xF)
 *       But that doesn't help distinguish 0 from "unchanged".
 *
 *  Better test: 0x0000000200000000 × 0x0000000300000000
 *  = 0x00000006_00000000_00000000 (96-bit)
 *  high64 = 0x0000000000000006, low64 = 0x0000000000000000
 *  ... low is 0, not useful.
 *
 *  Best test: 0x8000000000000001 × 0x3
 *  = 0x1_8000000000000003
 *  high64 = 0x0000000000000001, low64 = 0x8000000000000003
 *  Low is distinctive!
 * ══════════════════════════════════════════════════════════════════ */


/* ──────────────────────────────────────────────────────────────────
 *  Probe A: Check if low goes to src0 (RCX) or src1 (RDX)
 *
 *  Many microarchitectures overwrite a source register with
 *  the low product (like x86 MUL overwrites RAX).
 * ────────────────────────────────────────────────────────────────── */
void probe_source_regs(void) {

        /*
         * Patch: Do the MUL, then copy RCX→RAX and RDX→RBX
         * so we can inspect both source registers after MUL.
         */
        ucode_t probe_patch[] = {
                /* Triad 0: Do the multiply, isolate it */
                {
                        MUL_DSZ64_DRR(TMP0, RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: Copy results to architectural regs */
                {
                        MOVE_DSZ64_DR(RAX, RCX),          /* RAX = RCX after MUL */
                        MOVE_DSZ64_DR(RBX, RDX),          /* RBX = RDX after MUL */
                        MOVE_DSZ64_DR(R8,  TMP0),         /* R8  = dst (HIGH confirmed) */
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, probe_patch, ARRAY_SZ(probe_patch));
        hook_match_and_patch(0, 0x0428, 0x7c00);

        /* Test: 0x8000000000000001 × 0x3 = 0x1_8000000000000003
         *   HIGH = 0x0000000000000001
         *   LOW  = 0x8000000000000003
         */
        uint64_t rax_out, rbx_out, r8_out;
        asm volatile(
                "mov rcx, 0x8000000000000001\n\t"
                "mov rdx, 0x3\n\t"
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, rbx\n\t"
                "mov %2, r8\n\t"
                : "=r"(rax_out), "=r"(rbx_out), "=r"(r8_out)
                :
                : "rax", "rbx", "rcx", "rdx", "r8"
        );

        printf("=== PROBE A: Source register check ===\n");
        printf("0x8000000000000001 * 0x3:\n");
        printf("  Expected HIGH = 0x0000000000000001\n");
        printf("  Expected LOW  = 0x8000000000000003\n\n");
        printf("  R8  (TMP0=dst) = 0x%016" PRIx64, r8_out);
        if (r8_out == 0x1ULL) printf("  ← HIGH ✓");
        printf("\n");
        printf("  RAX (RCX=src0) = 0x%016" PRIx64, rax_out);
        if (rax_out == 0x8000000000000003ULL) printf("  ← LOW found in src0!");
        else if (rax_out == 0x8000000000000001ULL) printf("  (unchanged)");
        printf("\n");
        printf("  RBX (RDX=src1) = 0x%016" PRIx64, rbx_out);
        if (rbx_out == 0x8000000000000003ULL) printf("  ← LOW found in src1!");
        else if (rbx_out == 0x3ULL) printf("  (unchanged)");
        printf("\n\n");
}


/* ──────────────────────────────────────────────────────────────────
 *  Probe B: Check TMP registers for low product
 *
 *  Scans TMP0 through TMP5 (and beyond if needed).
 *  We already know TMP0 = HIGH, TMP1 = garbage.
 *  The low might be in TMP2, TMP3, or another internal reg.
 * ────────────────────────────────────────────────────────────────── */
void probe_tmp_regs(void) {

        /* Probe TMP2 and TMP3 */
        ucode_t probe_patch[] = {
                /* Triad 0: Multiply */
                {
                        MUL_DSZ64_DRR(TMP0, RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: Read TMP registers to architectural regs */
                {
                        MOVE_DSZ64_DR(RAX, TMP0),         /* should be HIGH */
                        MOVE_DSZ64_DR(RBX, TMP2),         /* check TMP2 */
                        MOVE_DSZ64_DR(R8,  TMP3),         /* check TMP3 */
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, probe_patch, ARRAY_SZ(probe_patch));
        hook_match_and_patch(0, 0x0428, 0x7c00);

        uint64_t rax_out, rbx_out, r8_out;
        asm volatile(
                "mov rcx, 0x8000000000000001\n\t"
                "mov rdx, 0x3\n\t"
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, rbx\n\t"
                "mov %2, r8\n\t"
                : "=r"(rax_out), "=r"(rbx_out), "=r"(r8_out)
                :
                : "rax", "rbx", "rcx", "rdx", "r8"
        );

        printf("=== PROBE B: TMP register scan ===\n");
        printf("  RAX (TMP0) = 0x%016" PRIx64, rax_out);
        if (rax_out == 0x1ULL) printf("  ← HIGH ✓");
        if (rax_out == 0x8000000000000003ULL) printf("  ← LOW!");
        printf("\n");
        printf("  RBX (TMP2) = 0x%016" PRIx64, rbx_out);
        if (rbx_out == 0x8000000000000003ULL) printf("  ← LOW found in TMP2!");
        printf("\n");
        printf("  R8  (TMP3) = 0x%016" PRIx64, r8_out);
        if (r8_out == 0x8000000000000003ULL) printf("  ← LOW found in TMP3!");
        printf("\n\n");
}


/* ──────────────────────────────────────────────────────────────────
 *  Probe C: Check TMP4, TMP5 and also try dst+1 with explicit dst
 *
 *  If none of the above work, try writing to a DIFFERENT dst
 *  and checking dst+1. If MUL_DSZ64_DRR(TMP2, RCX, RDX) puts
 *  high in TMP2, maybe low goes to TMP3 (the actual +1 neighbor).
 * ────────────────────────────────────────────────────────────────── */
void probe_alt_dst(void) {

        /* Use TMP2 as dst, check TMP3 for the pair */
        ucode_t probe_patch[] = {
                /* Triad 0: Multiply with TMP2 as destination */
                {
                        MUL_DSZ64_DRR(TMP2, RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: Read TMP2 (dst) and TMP3 (dst+1?) */
                {
                        MOVE_DSZ64_DR(RAX, TMP2),         /* should be HIGH */
                        MOVE_DSZ64_DR(RBX, TMP3),         /* LOW if pair is dst+1? */
                        MOVE_DSZ64_DR(R8,  TMP0),         /* check TMP0 as well */
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, probe_patch, ARRAY_SZ(probe_patch));
        hook_match_and_patch(0, 0x0428, 0x7c00);

        uint64_t rax_out, rbx_out, r8_out;
        asm volatile(
                "mov rcx, 0x8000000000000001\n\t"
                "mov rdx, 0x3\n\t"
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, rbx\n\t"
                "mov %2, r8\n\t"
                : "=r"(rax_out), "=r"(rbx_out), "=r"(r8_out)
                :
                : "rax", "rbx", "rcx", "rdx", "r8"
        );

        printf("=== PROBE C: Alternate dst (TMP2), check pair ===\n");
        printf("  RAX (TMP2=dst)  = 0x%016" PRIx64, rax_out);
        if (rax_out == 0x1ULL) printf("  ← HIGH ✓");
        if (rax_out == 0x8000000000000003ULL) printf("  ← LOW!");
        printf("\n");
        printf("  RBX (TMP3=dst+1)= 0x%016" PRIx64, rbx_out);
        if (rbx_out == 0x8000000000000003ULL) printf("  ← LOW found! Pair is dst+1");
        printf("\n");
        printf("  R8  (TMP0)      = 0x%016" PRIx64, r8_out);
        if (r8_out == 0x8000000000000003ULL) printf("  ← LOW in fixed TMP0!");
        printf("\n\n");
}


/* ──────────────────────────────────────────────────────────────────
 *  Probe D: Check if low goes to RAX implicitly
 *
 *  x86 MUL puts low in RAX. Maybe MUL_DSZ64 at the ucode level
 *  also implicitly writes the low product to architectural RAX
 *  regardless of the dst field.
 * ────────────────────────────────────────────────────────────────── */
void probe_implicit_rax(void) {

        ucode_t probe_patch[] = {
                /* Triad 0: Set RAX to a sentinel, then MUL */
                {
                        MOVE_DSZ64_DI(RAX, 0xDEAD),       /* sentinel in RAX */
                        MUL_DSZ64_DRR(TMP0, RCX, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: Read RAX (did MUL overwrite it?) */
                {
                        MOVE_DSZ64_DR(RBX, TMP0),         /* RBX = HIGH */
                        MOVE_DSZ64_DR(R8,  RCX),          /* R8 = src0 after */
                        MOVE_DSZ64_DR(R9,  RDX),          /* R9 = src1 after */
                        END_SEQWORD
                }
                /* RAX is not touched in triad 1 — whatever MUL wrote stays */
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, probe_patch, ARRAY_SZ(probe_patch));
        hook_match_and_patch(0, 0x0428, 0x7c00);

        uint64_t rax_out, rbx_out, r8_out, r9_out;
        asm volatile(
                "mov rcx, 0x8000000000000001\n\t"
                "mov rdx, 0x3\n\t"
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, rbx\n\t"
                "mov %2, r8\n\t"
                "mov %3, r9\n\t"
                : "=r"(rax_out), "=r"(rbx_out), "=r"(r8_out), "=r"(r9_out)
                :
                : "rax", "rbx", "rcx", "rdx", "r8", "r9"
        );

        printf("=== PROBE D: Implicit RAX write check ===\n");
        printf("  RAX (untouched) = 0x%016" PRIx64, rax_out);
        if (rax_out == 0x8000000000000003ULL)
                printf("  ← LOW! MUL implicitly writes RAX!");
        else if (rax_out == 0xDEADULL)
                printf("  (sentinel intact — MUL does NOT touch RAX)");
        else
                printf("  (unexpected value)");
        printf("\n");
        printf("  RBX (TMP0=dst)  = 0x%016" PRIx64, rbx_out);
        if (rbx_out == 0x1ULL) printf("  ← HIGH ✓");
        printf("\n");
        printf("  R8  (RCX=src0)  = 0x%016" PRIx64, r8_out);
        if (r8_out == 0x8000000000000003ULL) printf("  ← LOW in src0!");
        else if (r8_out == 0x8000000000000001ULL) printf("  (unchanged)");
        printf("\n");
        printf("  R9  (RDX=src1)  = 0x%016" PRIx64, r9_out);
        if (r9_out == 0x8000000000000003ULL) printf("  ← LOW in src1!");
        else if (r9_out == 0x3ULL) printf("  (unchanged)");
        printf("\n\n");
}


/* ──────────────────────────────────────────────────────────────────
 *  Probe E: Comprehensive URAM scan
 *
 *  If the low product goes nowhere visible, try reading URAM
 *  locations. The multiply unit might deposit it in a fixed
 *  URAM slot.
 * ────────────────────────────────────────────────────────────────── */
void probe_uram(void) {

        /* Check URAM slots 0-2 */
        ucode_t probe_patch[] = {
                /* Triad 0: Multiply */
                {
                        MUL_DSZ64_DRR(TMP0, RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: Read URAM slots */
                {
                        READURAM_DI(RAX, 0x0),            /* URAM[0] → RAX */
                        READURAM_DI(RBX, 0x1),            /* URAM[1] → RBX */
                        READURAM_DI(R8,  0x2),            /* URAM[2] → R8  */
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, probe_patch, ARRAY_SZ(probe_patch));
        hook_match_and_patch(0, 0x0428, 0x7c00);

        uint64_t rax_out, rbx_out, r8_out;
        asm volatile(
                "mov rcx, 0x8000000000000001\n\t"
                "mov rdx, 0x3\n\t"
                "rdrand rax\n\t"
                "mov %0, rax\n\t"
                "mov %1, rbx\n\t"
                "mov %2, r8\n\t"
                : "=r"(rax_out), "=r"(rbx_out), "=r"(r8_out)
                :
                : "rax", "rbx", "rcx", "rdx", "r8"
        );

        printf("=== PROBE E: URAM scan ===\n");
        printf("  RAX (URAM[0]) = 0x%016" PRIx64, rax_out);
        if (rax_out == 0x8000000000000003ULL) printf("  ← LOW in URAM[0]!");
        printf("\n");
        printf("  RBX (URAM[1]) = 0x%016" PRIx64, rbx_out);
        if (rbx_out == 0x8000000000000003ULL) printf("  ← LOW in URAM[1]!");
        printf("\n");
        printf("  R8  (URAM[2]) = 0x%016" PRIx64, r8_out);
        if (r8_out == 0x8000000000000003ULL) printf("  ← LOW in URAM[2]!");
        printf("\n\n");
}


/* ══════════════════════════════════════════════════════════════════
 *  PHASE 2: CORRECTED MAC128 (template — fill in LOW_REG)
 *
 *  Once you identify where the low product lands, replace
 *  LOW_REG below with the correct register name.
 *
 *  Convention:
 *    INPUT:   RCX = src1,  RDX = src2
 *             RAX = acc_lo, R8 = acc_hi
 *    OUTPUT:  RAX = acc_lo (updated), R8 = acc_hi (updated)
 * ══════════════════════════════════════════════════════════════════ */

/*
 * *** UNCOMMENT AND EDIT ONCE LOW_REG IS KNOWN ***
 *
 * Example: if Probe A reveals LOW goes to RCX (src0):
 *   #define MUL_LOW_REG  RCX
 *
 * Example: if Probe C reveals LOW goes to TMP3 (dst+1 when dst=TMP2):
 *   Then use TMP2 as MUL dst and read TMP3 for low.
 */

/* ── Option 1: LOW is in a source register (e.g., RCX) ────────── */
#ifdef MUL_LOW_IN_SRC0

void install_mac128(void) {
        ucode_t mac128_patch[] = {
                /* Triad 0: Multiply — isolate in own triad
                 * After this: TMP0 = HIGH, RCX = LOW              */
                {
                        MUL_DSZ64_DRR(TMP0, RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: Accumulate low, prep carry
                 * RAX += RCX (prod_lo), capture carry              */
                {
                        ADD_DSZ64_DRR(RAX, RAX, RCX),     /* acc_lo += prod_lo */
                        SETCC_CONDB_DR(TMP2, TMP2),       /* TMP2 = CF         */
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 2: Accumulate high + carry, done           */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP0),      /* acc_hi += prod_hi */
                        ADD_DSZ64_DRR(R8, R8, TMP2),      /* acc_hi += carry   */
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac128_patch, ARRAY_SZ(mac128_patch));
        hook_match_and_patch(0, 0x0428, 0x7c00);
}

#endif /* MUL_LOW_IN_SRC0 */


/* ── Option 2: LOW is in src1 (RDX) ───────────────────────────── */
#ifdef MUL_LOW_IN_SRC1

void install_mac128(void) {
        ucode_t mac128_patch[] = {
                {
                        MUL_DSZ64_DRR(TMP0, RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(RAX, RAX, RDX),     /* acc_lo += prod_lo (in RDX) */
                        SETCC_CONDB_DR(TMP2, TMP2),
                        NOP,
                        NOP_SEQWORD
                },
                {
                        ADD_DSZ64_DRR(R8, R8, TMP0),      /* acc_hi += prod_hi */
                        ADD_DSZ64_DRR(R8, R8, TMP2),      /* acc_hi += carry   */
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac128_patch, ARRAY_SZ(mac128_patch));
        hook_match_and_patch(0, 0x0428, 0x7c00);
}

#endif /* MUL_LOW_IN_SRC1 */


/* ── Option 3: LOW is in implicit RAX ─────────────────────────── */
#ifdef MUL_LOW_IN_RAX

/*
 * This is the trickiest case because RAX is also our accumulator.
 * We need to save ACC_LO to a TMP first, do MUL (which overwrites RAX
 * with prod_lo), then add the saved ACC_LO back.
 */
void install_mac128(void) {
        ucode_t mac128_patch[] = {
                /* Triad 0: Save acc_lo, then multiply
                 * MUL will overwrite RAX with prod_lo             */
                {
                        MOVE_DSZ64_DR(TMP3, RAX),          /* TMP3 = saved acc_lo */
                        MUL_DSZ64_DRR(TMP0, RCX, RDX),    /* TMP0=HIGH, RAX=LOW  */
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: RAX now has prod_lo, add saved acc     */
                {
                        ADD_DSZ64_DRR(RAX, RAX, TMP3),    /* acc_lo = prod_lo + saved */
                        SETCC_CONDB_DR(TMP2, TMP2),       /* TMP2 = CF         */
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 2: Accumulate high + carry                */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP0),      /* acc_hi += prod_hi */
                        ADD_DSZ64_DRR(R8, R8, TMP2),      /* acc_hi += carry   */
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac128_patch, ARRAY_SZ(mac128_patch));
        hook_match_and_patch(0, 0x0428, 0x7c00);
}

#endif /* MUL_LOW_IN_RAX */


/* ── Option 4: LOW is in dst+1 pair (use TMP2→TMP3) ──────────── */
#ifdef MUL_LOW_IN_DST_PLUS1

/*
 * If Probe C shows TMP3 holds the low when dst=TMP2,
 * then the register pair convention IS real but TMP0/TMP1
 * might not be adjacent in the physical register file.
 * Use TMP2 as dst, read TMP3 for low.
 */
void install_mac128(void) {
        ucode_t mac128_patch[] = {
                /* Triad 0: Multiply into TMP2 (high) : TMP3 (low) */
                {
                        MUL_DSZ64_DRR(TMP2, RCX, RDX),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: Accumulate low                         */
                {
                        ADD_DSZ64_DRR(RAX, RAX, TMP3),    /* acc_lo += prod_lo */
                        SETCC_CONDB_DR(TMP0, TMP0),       /* TMP0 = CF         */
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 2: Accumulate high + carry                */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP2),      /* acc_hi += prod_hi */
                        ADD_DSZ64_DRR(R8, R8, TMP0),      /* acc_hi += carry   */
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac128_patch, ARRAY_SZ(mac128_patch));
        hook_match_and_patch(0, 0x0428, 0x7c00);
}

#endif /* MUL_LOW_IN_DST_PLUS1 */


/* ══════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {

        printf("╔════════════════════════════════════════════════╗\n");
        printf("║  MAC128 v2 — MUL Low-Half Discovery Probes     ║\n");
        printf("╚════════════════════════════════════════════════╝\n\n");
        printf("Test vector: 0x8000000000000001 × 0x3\n");
        printf("  Expected HIGH = 0x0000000000000001\n");
        printf("  Expected LOW  = 0x8000000000000003\n\n");
        printf("Scanning for LOW location...\n\n");

        probe_source_regs();   /* Check RCX, RDX */
        probe_tmp_regs();      /* Check TMP2, TMP3 */
        probe_alt_dst();       /* Check dst+1 pair with different dst */
        probe_implicit_rax();  /* Check if MUL writes RAX implicitly */
        probe_uram();          /* Check URAM slots */

        printf("══════════════════════════════════════════════════\n");
        printf("SUMMARY: Look for '← LOW' markers above.\n");
        printf("Once found, edit mac128_v2.c:\n");
        printf("  - Define the matching MUL_LOW_IN_xxx macro\n");
        printf("  - Recompile and run MAC128 tests\n");
        printf("══════════════════════════════════════════════════\n");

        return 0;
}
