/*
 * mac128_vmw.c — MAC128 using VMWRITE multiply hook
 *
 * ═══════════════════════════════════════════════════════════════════
 *  CONFIRMED HARDWARE BEHAVIOR (Intel Celeron N3350, Goldmont):
 *
 *  vmwrite rcx, rdx  (hooked at ucode entry 0x0cd8)
 *  MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST):
 *    RCX = LOW  64 bits of (RCX × RDX)
 *    RDX = HIGH 64 bits of (RCX × RDX)
 *    Both source registers clobbered with 128-bit result.
 *
 *  Microcode ALU (ADD, SUB, etc.) does NOT update flags readable
 *  by SETCC, CMOVCC, SELECTCC, or UJMPCC. No known bridge
 *  (GENARITHFLAGS, MOVEINSERTFLGS, READAFLAGS) works.
 *
 *  → Carry detection uses pure arithmetic: SHR((a&b)|(~s&(a|b)),63)
 * ═══════════════════════════════════════════════════════════════════
 *
 *  MAC128 operation:  acc_hi:acc_lo += RCX × RDX
 *
 *  Register convention:
 *    INPUT:   RCX = multiplicand,  RDX = multiplier
 *             RAX = acc_lo,        R8  = acc_hi
 *    OUTPUT:  RAX = acc_lo (updated), R8 = acc_hi (updated)
 *    CLOBBERED: RCX, RDX
 *
 *  Trigger: vmwrite rcx, rdx
 *  Hook:    ucode entry 0x0cd8 → patch at 0x7c00
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
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  WMUL — Wide Multiply (2 triads)
 *
 *  Just the multiply. Results in RCX:RDX (lo:hi), then rearranged
 *  to the standard RDX:RAX convention (hi in RDX, lo in RAX).
 *
 *  Use this if you prefer x86 add/adc for accumulation.
 * ══════════════════════════════════════════════════════════════════ */
void install_wmul(void) {
        ucode_t wmul_patch[] = {
                /* Triad 0: Multiply — RCX=lo, RDX=hi */
                {
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* Triad 1: Rearrange to RDX:RAX (standard convention)
                 *   RAX ← RCX (lo),  keep RDX (hi) as-is
                 *   Both reads are from triad 0 values — safe. */
                {
                        MOVE_DSZ64_DR(RAX, RCX),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };
        do_patch(wmul_patch, 2);
}


/* ══════════════════════════════════════════════════════════════════
 *  MAC128 — Multiply-Accumulate 128-bit (6 triads)
 *
 *  acc_hi:acc_lo += RCX × RDX
 *
 *  After MUL: RCX = prod_lo, RDX = prod_hi (both clobbered).
 *  We need old acc_lo for carry detection, so save it first.
 *
 *  Carry formula: for s = a + b:
 *    carry_out = MSB of ((a & b) | (~s & (a | b)))
 *
 *  Where a = old_acc_lo, b = prod_lo, s = new_acc_lo.
 * ══════════════════════════════════════════════════════════════════ */
void install_mac128(void) {
        ucode_t mac128_patch[] = {

                /* ── Triad 0: Save acc_lo, then multiply ──────────
                 *  MOVE must execute before MUL (same triad reads
                 *  use pre-triad values, so both see original RAX).
                 *  After triad: TMP3=old_acc, RCX=prod_lo, RDX=prod_hi
                 */
                {
                        MOVE_DSZ64_DR(TMP3, RAX),            /* TMP3 = old acc_lo */
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST), /* RCX=lo, RDX=hi */
                        NOP,
                        NOP_SEQWORD
                },

                /* ── Triad 1: Sum + carry-detect operands ─────────
                 *  All 3 ops read TMP3 (old_acc) and RCX (prod_lo).
                 *  a = TMP3 = old acc_lo
                 *  b = RCX  = prod_lo
                 *  All write different destinations — no conflicts.
                 */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),      /* RAX = a + b (new acc_lo) */
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),     /* TMP1 = a & b */
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),      /* TMP2 = a | b */
                        NOP_SEQWORD
                },

                /* ── Triad 2: Propagated overflow + accumulate hi ─
                 *  NOTAND(d, x, y) = ~x & y
                 *  TMP4 = ~RAX & TMP2 = ~sum & (a|b)
                 *  ADD:  acc_hi += prod_hi (independent, parallel)
                 */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),  /* ~sum & (a|b) */
                        ADD_DSZ64_DRR(R8, R8, RDX),         /* acc_hi += prod_hi */
                        NOP,
                        NOP_SEQWORD
                },

                /* ── Triad 3: Merge carry chain ───────────────── */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),     /* carry chain bits */
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },

                /* ── Triad 4: Extract carry (bit 63) ──────────── */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),      /* carry = 0 or 1 */
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },

                /* ── Triad 5: Fold carry into high, done ──────── */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),         /* acc_hi += carry */
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        do_patch(mac128_patch, 6);
}


/* ══════════════════════════════════════════════════════════════════
 *  TEST SUITE
 * ══════════════════════════════════════════════════════════════════ */

typedef struct {
        const char *name;
        uint64_t acc_lo_in, acc_hi_in;
        uint64_t src1, src2;
        uint64_t exp_lo, exp_hi;
} mac_test_t;

static int run_mac_test(const mac_test_t *t) {
        uint64_t rax_out, r8_out;

        asm volatile(
                "mov rax, %[alo]\n\t"
                "mov r8,  %[ahi]\n\t"
                "mov rcx, %[s1]\n\t"
                "mov rdx, %[s2]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[rax], rax\n\t"
                "mov %[r8],  r8\n\t"
                : [rax] "=r"(rax_out), [r8] "=r"(r8_out)
                : [alo] "r"(t->acc_lo_in), [ahi] "r"(t->acc_hi_in),
                  [s1]  "r"(t->src1),      [s2]  "r"(t->src2)
                : "rax", "rcx", "rdx", "r8"
        );

        int pass = (rax_out == t->exp_lo && r8_out == t->exp_hi);
        printf("%s:\n", t->name);
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")%s\n",
               rax_out, t->exp_lo, (rax_out == t->exp_lo) ? "" : " ✗");
        printf("  R8  (hi) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")%s\n",
               r8_out, t->exp_hi, (r8_out == t->exp_hi) ? "" : " ✗");
        printf("  %s\n\n", pass ? "✓ PASS" : "✗ FAIL");
        return pass;
}


/* ── WMUL tests ─────────────────────────────────────────────────── */

typedef struct {
        const char *name;
        uint64_t src1, src2;
        uint64_t exp_lo, exp_hi;
} wmul_test_t;

static int run_wmul_test(const wmul_test_t *t) {
        uint64_t rax_out, rdx_out;

        asm volatile(
                "mov rcx, %[s1]\n\t"
                "mov rdx, %[s2]\n\t"
                "vmwrite rcx, rdx\n\t"
                "mov %[rax], rax\n\t"
                "mov %[rdx], rdx\n\t"
                : [rax] "=r"(rax_out), [rdx] "=r"(rdx_out)
                : [s1] "r"(t->src1), [s2] "r"(t->src2)
                : "rax", "rcx", "rdx"
        );

        int lo_ok = (rax_out == t->exp_lo);
        int hi_ok = (rdx_out == t->exp_hi);
        int pass = lo_ok && hi_ok;
        printf("%s:\n", t->name);
        printf("  RAX (lo) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")%s\n",
               rax_out, t->exp_lo, lo_ok ? "" : " ✗");
        printf("  RDX (hi) = 0x%016" PRIx64 "  (expect 0x%016" PRIx64 ")%s\n",
               rdx_out, t->exp_hi, hi_ok ? "" : " ✗");
        printf("  %s\n\n", pass ? "✓ PASS" : "✗ FAIL");
        return pass;
}


static void test_wmul(void) {
        printf("═══════════════════════════════════════════\n");
        printf("  WMUL Tests (multiply only, 2 triads)\n");
        printf("═══════════════════════════════════════════\n\n");

        install_wmul();

        wmul_test_t tests[] = {
                { "WMUL 1: 7 × 13",
                  7, 13, 91, 0 },
                { "WMUL 2: max × max",
                  0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
                  0x0000000000000001ULL, 0xFFFFFFFFFFFFFFFEULL },
                { "WMUL 3: 0x8000000000000001 × 3",
                  0x8000000000000001ULL, 3,
                  0x8000000000000003ULL, 0x0000000000000001ULL },
                { "WMUL 4: Curve25519 limbs",
                  0x0006000000000000ULL, 0x0004000000000000ULL,
                  0x0000000000000000ULL, 0x0000001800000000ULL },
        };

        int n = sizeof(tests) / sizeof(tests[0]);
        int pass = 0;
        for (int i = 0; i < n; i++)
                pass += run_wmul_test(&tests[i]);

        printf("  WMUL: %d / %d passed\n\n", pass, n);
}


static void test_mac128(void) {
        printf("═══════════════════════════════════════════\n");
        printf("  MAC128 Tests (multiply-accumulate, 6 triads)\n");
        printf("═══════════════════════════════════════════\n\n");

        install_mac128();

        mac_test_t tests[] = {
                {
                        .name = "MAC 1: acc=100, += 7×13 → 191",
                        .acc_lo_in = 100, .acc_hi_in = 0,
                        .src1 = 7, .src2 = 13,
                        .exp_lo = 191, .exp_hi = 0
                },
                {
                        .name = "MAC 2: acc=0, += max×max",
                        .acc_lo_in = 0, .acc_hi_in = 0,
                        .src1 = 0xFFFFFFFFFFFFFFFFULL,
                        .src2 = 0xFFFFFFFFFFFFFFFFULL,
                        .exp_lo = 0x0000000000000001ULL,
                        .exp_hi = 0xFFFFFFFFFFFFFFFEULL
                },
                {
                        .name = "MAC 3: acc=MAX128, += 1×2 (CARRY WRAP)",
                        .acc_lo_in = 0xFFFFFFFFFFFFFFFFULL,
                        .acc_hi_in = 0xFFFFFFFFFFFFFFFFULL,
                        .src1 = 1, .src2 = 2,
                        .exp_lo = 0x0000000000000001ULL,
                        .exp_hi = 0x0000000000000000ULL
                },
                {
                        .name = "MAC 4: Curve25519 limb product",
                        .acc_lo_in = 0, .acc_hi_in = 0,
                        .src1 = 0x0006000000000000ULL,
                        .src2 = 0x0004000000000000ULL,
                        .exp_lo = 0x0000000000000000ULL,
                        .exp_hi = 0x0000001800000000ULL
                },
                {
                        .name = "MAC 5: existing high, small MAC",
                        .acc_lo_in = 0, .acc_hi_in = 0xFF,
                        .src1 = 1, .src2 = 1,
                        .exp_lo = 1, .exp_hi = 0xFF
                },
                {
                        .name = "MAC 6: low overflow → carry to hi",
                        .acc_lo_in = 0xFFFFFFFFFFFFFFFFULL,
                        .acc_hi_in = 5,
                        .src1 = 1, .src2 = 3,
                        .exp_lo = 0x0000000000000002ULL,
                        .exp_hi = 6
                },
                {
                        .name = "MAC 7: sequential (pre-accumulated)",
                        .acc_lo_in = 15, .acc_hi_in = 0,
                        .src1 = 7, .src2 = 11,
                        .exp_lo = 92, .exp_hi = 0
                },
                {
                        .name = "MAC 8: probe vector (0x8..1 × 3)",
                        .acc_lo_in = 0, .acc_hi_in = 0,
                        .src1 = 0x8000000000000001ULL,
                        .src2 = 0x3ULL,
                        .exp_lo = 0x8000000000000003ULL,
                        .exp_hi = 0x0000000000000001ULL
                },
                {
                        .name = "MAC 9: double overflow (product+acc)",
                        .acc_lo_in = 0xFFFFFFFFFFFFFFFFULL,
                        .acc_hi_in = 0,
                        .src1 = 0xFFFFFFFFFFFFFFFFULL,
                        .src2 = 0xFFFFFFFFFFFFFFFFULL,
                        /* max*max = FFFFFFFFFFFFFFFE:0000000000000001
                         * + acc_lo=FFFFFFFFFFFFFFFF
                         * new_lo = 1 + FFFFFFFFFFFFFFFF = 0 (carry!)
                         * new_hi = FFFFFFFFFFFFFFFE + 1 = FFFFFFFFFFFFFFFF */
                        .exp_lo = 0x0000000000000000ULL,
                        .exp_hi = 0xFFFFFFFFFFFFFFFFULL
                },
                {
                        .name = "MAC 10: boundary carry (acc=1, prod_lo=max)",
                        .acc_lo_in = 1, .acc_hi_in = 0,
                        .src1 = 0xFFFFFFFFFFFFFFFFULL, .src2 = 1,
                        /* FFFFFFFFFFFFFFFF * 1 = 0:FFFFFFFFFFFFFFFF
                         * + acc_lo=1
                         * new_lo = FFFFFFFFFFFFFFFF + 1 = 0 (carry!)
                         * new_hi = 0 + 0 + 1 = 1 */
                        .exp_lo = 0x0000000000000000ULL,
                        .exp_hi = 0x0000000000000001ULL
                },
        };

        int n = sizeof(tests) / sizeof(tests[0]);
        int pass = 0;
        for (int i = 0; i < n; i++)
                pass += run_mac_test(&tests[i]);

        printf("════════════════════════════════════════\n");
        printf("  MAC128: %d / %d passed\n", pass, n);
        printf("════════════════════════════════════════\n\n");

        if (pass == n) {
                printf("MAC128 is fully operational!\n\n");
                printf("  Trigger:     vmwrite rcx, rdx\n");
                printf("  Hook entry:  0x0cd8 → patch 0x7c00\n");
                printf("  Triads:      6 (10 active uops)\n");
                printf("  Carry:       SHR((a&b)|(~s&(a|b)),63)\n\n");
                printf("  MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST):\n");
                printf("    RCX = LOW,  RDX = HIGH (both clobbered)\n\n");
        }
}


/* ══════════════════════════════════════════════════════════════════
 *  MAC128 CHAIN TEST — Multiple MACs in sequence
 *
 *  Simulates curve25519 limb accumulation: 3 MACs into one acc.
 * ══════════════════════════════════════════════════════════════════ */

static void test_mac_chain(void) {
        printf("═══════════════════════════════════════════\n");
        printf("  MAC128 Chain Test (3 MACs in sequence)\n");
        printf("═══════════════════════════════════════════\n\n");

        install_mac128();

        /* Chain: acc = 0, acc += 7*13, acc += 11*17, acc += 3*5
         * = 0 + 91 + 187 + 15 = 293 */
        uint64_t acc_lo, acc_hi;
        asm volatile(
                "xor rax, rax\n\t"          /* acc_lo = 0 */
                "xor r8, r8\n\t"            /* acc_hi = 0 */

                "mov rcx, 7\n\t"
                "mov rdx, 13\n\t"
                "vmwrite rcx, rdx\n\t"      /* acc += 7*13 */

                "mov rcx, 11\n\t"
                "mov rdx, 17\n\t"
                "vmwrite rcx, rdx\n\t"      /* acc += 11*17 */

                "mov rcx, 3\n\t"
                "mov rdx, 5\n\t"
                "vmwrite rcx, rdx\n\t"      /* acc += 3*5 */

                "mov %0, rax\n\t"
                "mov %1, r8\n\t"
                : "=r"(acc_lo), "=r"(acc_hi)
                :
                : "rax", "rcx", "rdx", "r8"
        );

        int ok = (acc_lo == 293 && acc_hi == 0);
        printf("Chain: 0 + 7*13 + 11*17 + 3*5:\n");
        printf("  lo=%" PRIu64 " (expect 293), hi=%" PRIu64 " (expect 0)\n",
               acc_lo, acc_hi);
        printf("  %s\n\n", ok ? "✓ PASS" : "✗ FAIL");


        /* Chain with large values and carry propagation:
         * Use 51-bit limb values typical of Curve25519.
         * a = 0x0007FFFFFFFFFFFF (max 51-bit)
         * b1 = 0x0006000000000000
         * b2 = 0x0005000000000000
         * b3 = 0x0004000000000000
         *
         * acc = a*b1 + a*b2 + a*b3 = a*(b1+b2+b3)
         */
        uint64_t a  = 0x0007FFFFFFFFFFFFULL;
        uint64_t b1 = 0x0006000000000000ULL;
        uint64_t b2 = 0x0005000000000000ULL;
        uint64_t b3 = 0x0004000000000000ULL;

        __uint128_t ref = (__uint128_t)a * b1
                        + (__uint128_t)a * b2
                        + (__uint128_t)a * b3;
        uint64_t ref_lo = (uint64_t)ref;
        uint64_t ref_hi = (uint64_t)(ref >> 64);

        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"

                "mov rcx, %[a]\n\t"
                "mov rdx, %[b1]\n\t"
                "vmwrite rcx, rdx\n\t"

                "mov rcx, %[a]\n\t"
                "mov rdx, %[b2]\n\t"
                "vmwrite rcx, rdx\n\t"

                "mov rcx, %[a]\n\t"
                "mov rdx, %[b3]\n\t"
                "vmwrite rcx, rdx\n\t"

                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(acc_lo), [hi] "=r"(acc_hi)
                : [a] "r"(a), [b1] "r"(b1), [b2] "r"(b2), [b3] "r"(b3)
                : "rax", "rcx", "rdx", "r8"
        );

        ok = (acc_lo == ref_lo && acc_hi == ref_hi);
        printf("Chain: a*(b1+b2+b3), 51-bit Curve25519 limbs:\n");
        printf("  lo=0x%016" PRIx64 " (expect 0x%016" PRIx64 ")%s\n",
               acc_lo, ref_lo, (acc_lo == ref_lo) ? "" : " ✗");
        printf("  hi=0x%016" PRIx64 " (expect 0x%016" PRIx64 ")%s\n",
               acc_hi, ref_hi, (acc_hi == ref_hi) ? "" : " ✗");
        printf("  %s\n\n", ok ? "✓ PASS" : "✗ FAIL");
}


/* ══════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {
        printf("╔════════════════════════════════════════════════════╗\n");
        printf("║  MAC128 via VMWRITE — Goldmont Microcode           ║\n");
        printf("╚════════════════════════════════════════════════════╝\n\n");

        if (argc > 1 && argv[1][0] == 'w') {
                test_wmul();
                return 0;
        }

        test_mac128();
        test_mac_chain();

        return 0;
}


/*
 * ═══════════════════════════════════════════════════════════════════
 *  CURVE25519 INTEGRATION
 * ═══════════════════════════════════════════════════════════════════
 *
 *  Limb 0: acc = arg1[0]² + arg1[1]*x2 + arg1[2]*x5
 *
 *  ┌──────────────────────────────────────────────────────────┐
 *  │  xor rax, rax           ; acc_lo = 0                    │
 *  │  xor r8, r8             ; acc_hi = 0                    │
 *  │                                                          │
 *  │  mov rcx, [rsi+0x10]    ; arg1[2]                       │
 *  │  mov rdx, r9            ; x5                             │
 *  │  vmwrite rcx, rdx       ; acc += arg1[2]*x5             │
 *  │                                                          │
 *  │  mov rcx, [rsi+0x08]    ; arg1[1]                       │
 *  │  mov rdx, r10           ; x2                             │
 *  │  vmwrite rcx, rdx       ; acc += arg1[1]*x2             │
 *  │                                                          │
 *  │  mov rcx, [rsi+0x00]    ; arg1[0]                       │
 *  │  mov rdx, rcx           ; self-square                   │
 *  │  vmwrite rcx, rdx       ; acc += arg1[0]²               │
 *  │                                                          │
 *  │  ; R8:RAX = limb 0 accumulator                          │
 *  │  ; → extract: limb0 = RAX & 0x7FFFFFFFFFFFF             │
 *  │  ;            carry  = (R8 << 13) | (RAX >> 51)         │
 *  └──────────────────────────────────────────────────────────┘
 *
 *  Per limb: 2 (init) + 3×3 (MACs) = 11 instructions
 *  vs current: ~24 instructions + stack spills
 *
 * ═══════════════════════════════════════════════════════════════════
 */
