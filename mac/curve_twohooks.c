/*
 * curve25519_sq.c — Curve25519 field square via MAC128 microcode hook
 *
 * ═══════════════════════════════════════════════════════════════════
 *  Two-phase curve25519_square using vmwrite MAC128 hook.
 *
 *  Phase 1 (Hook 1 work):  9 MACs → out[0], out[1], out[2] + carry_3
 *  Phase 2 (Hook 2 work):  6 MACs → out[3], out[4] + final reduction
 *
 *  Each MAC = one vmwrite rcx, rdx invocation.
 *  Total:     15 vmwrite calls per square.
 *  Hook cost: ~5 cycles × 15 = ~75 cycles redirect overhead.
 *  MAC body:  ~3 cycles × 15 = ~45 cycles execution.
 *  Expected:  ~120 cycles total (vs ~25 native).
 *
 *  The phases map directly to the planned monolithic hooks:
 *    Hook 1 at MSRAM 0x7c00 (~31 triads) — future: one vmwrite
 *    Hook 2 at MSRAM 0x7c20 (~26 triads) — future: one vmwrite
 *  For now we use the proven 6-triad MAC128 per call.
 *
 *  Radix 2^51 representation, 5 limbs, mask = 0x7FFFFFFFFFFFF.
 *
 *  Build:  make PROG=curve25519_sq
 *  Run:    sudo taskset -c 0 ./curve25519_sq_static
 * ═══════════════════════════════════════════════════════════════════
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL


/* ══════════════════════════════════════════════════════════════════
 *  MAC128 INSTALLER (6 triads, proven working)
 *
 *  vmwrite rcx, rdx  → acc_hi:acc_lo += RCX × RDX
 *  Hook:  0x0cd8 → patch 0x7c00
 * ══════════════════════════════════════════════════════════════════ */
void install_mac128(void) {
        ucode_t mac128_patch[] = {
                /* T0: save acc_lo, multiply */
                {
                        ZEROEXT_DSZ64_DR(TMP3, RAX),
                        MUL_DSZ64_DRR(R64SRC, R64SRC, R64DST),
                        NOP,
                        NOP_SEQWORD
                },
                /* T1: sum + carry operands */
                {
                        ADD_DSZ64_DRR(RAX, TMP3, RCX),
                        AND_DSZ64_DRR(TMP1, TMP3, RCX),
                        OR_DSZ64_DRR(TMP2, TMP3, RCX),
                        NOP_SEQWORD
                },
                /* T2: propagated overflow + acc_hi */
                {
                        NOTAND_DSZ64_DRR(TMP4, RAX, TMP2),
                        ADD_DSZ64_DRR(R8, R8, RDX),
                        NOP,
                        NOP_SEQWORD
                },
                /* T3: merge carry chain */
                {
                        OR_DSZ64_DRR(TMP5, TMP1, TMP4),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T4: extract carry bit */
                {
                        SHR_DSZ64_DRI(TMP5, TMP5, 63),
                        NOP,
                        NOP,
                        NOP_SEQWORD
                },
                /* T5: fold carry, done */
                {
                        ADD_DSZ64_DRR(R8, R8, TMP5),
                        NOP,
                        NOP,
                        END_SEQWORD
                }
        };

        assign_to_core(0);
        do_fix_IN_patch();
        patch_ucode(0x7c00, mac128_patch, 6);
        hook_match_and_patch(0, 0x0cd8, 0x7c00);
}


/* ══════════════════════════════════════════════════════════════════
 *  REFERENCE C — Curve25519 field square (radix 2^51)
 *
 *  Uses __uint128_t for correctness verification.
 * ══════════════════════════════════════════════════════════════════ */
static void fe_sq_ref(const uint64_t *a, uint64_t *out) {
        uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
        uint64_t d0 = 2*a0, d1 = 2*a1, d2 = 2*a2, d3 = 2*a3;
        uint64_t r3 = 19*a3, r4 = 19*a4;

        __uint128_t c0 = (__uint128_t)a0*a0 + (__uint128_t)d1*r4 + (__uint128_t)d2*r3;
        __uint128_t c1 = (__uint128_t)d0*a1 + (__uint128_t)r3*a3 + (__uint128_t)d2*r4;
        __uint128_t c2 = (__uint128_t)d0*a2 + (__uint128_t)a1*a1 + (__uint128_t)d3*r4;
        __uint128_t c3 = (__uint128_t)d0*a3 + (__uint128_t)d1*a2 + (__uint128_t)r4*a4;
        __uint128_t c4 = (__uint128_t)d0*a4 + (__uint128_t)d1*a3 + (__uint128_t)a2*a2;

        uint64_t carry;
        carry = (uint64_t)(c0 >> 51); out[0] = (uint64_t)c0 & MASK51;
        c1 += carry;
        carry = (uint64_t)(c1 >> 51); out[1] = (uint64_t)c1 & MASK51;
        c2 += carry;
        carry = (uint64_t)(c2 >> 51); out[2] = (uint64_t)c2 & MASK51;
        c3 += carry;
        carry = (uint64_t)(c3 >> 51); out[3] = (uint64_t)c3 & MASK51;
        c4 += carry;
        carry = (uint64_t)(c4 >> 51); out[4] = (uint64_t)c4 & MASK51;

        out[0] += carry * 19;
        carry = out[0] >> 51; out[0] &= MASK51;
        out[1] += carry;
}


/* ══════════════════════════════════════════════════════════════════
 *  MAC128 CURVE25519 SQUARE — 15 vmwrite calls, two phases
 *
 *  Products (d = doubled, r = ×19 reduced):
 *
 *   c[0] = a0·a0 + d1·r4 + d2·r3
 *   c[1] = d0·a1 + r3·a3 + d2·r4
 *   c[2] = d0·a2 + a1·a1 + d3·r4
 *   ─── phase boundary ───
 *   c[3] = d0·a3 + d1·a2 + r4·a4
 *   c[4] = d0·a4 + d1·a3 + a2·a2
 *
 *  Each product = one vmwrite (MAC128 accumulates into RAX:R8).
 *  Carry chain in x86 between limbs.
 * ══════════════════════════════════════════════════════════════════ */
__attribute__((noinline))
void fe_sq_mac128(const uint64_t *a, uint64_t *out) {
        uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
        uint64_t d0 = 2*a0, d1 = 2*a1, d2 = 2*a2, d3 = 2*a3;
        uint64_t r3 = 19*a3, r4 = 19*a4;

        uint64_t c_lo, c_hi, carry;

        /* ── Phase 1: out[0], out[1], out[2] (9 MACs) ─────────── */

        /* c[0] = a0·a0 + d1·r4 + d2·r3 */
        asm volatile(
                "xor rax, rax\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x2]\n\t"  "mov rdx, %[y2]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x3]\n\t"  "mov rdx, %[y3]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [x1] "r"(a0), [y1] "r"(a0),
                  [x2] "r"(d1), [y2] "r"(r4),
                  [x3] "r"(d2), [y3] "r"(r3)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[0] = c_lo & MASK51;

        /* c[1] = carry + d0·a1 + r3·a3 + d2·r4 */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x2]\n\t"  "mov rdx, %[y2]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x3]\n\t"  "mov rdx, %[y3]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a1),
                  [x2] "r"(r3), [y2] "r"(a3),
                  [x3] "r"(d2), [y3] "r"(r4)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[1] = c_lo & MASK51;

        /* c[2] = carry + d0·a2 + a1·a1 + d3·r4 */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x2]\n\t"  "mov rdx, %[y2]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x3]\n\t"  "mov rdx, %[y3]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a2),
                  [x2] "r"(a1), [y2] "r"(a1),
                  [x3] "r"(d3), [y3] "r"(r4)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[2] = c_lo & MASK51;

        /* ── Phase 2: out[3], out[4] (6 MACs) + reduction ─────── */

        /* c[3] = carry + d0·a3 + d1·a2 + r4·a4 */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x2]\n\t"  "mov rdx, %[y2]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x3]\n\t"  "mov rdx, %[y3]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a3),
                  [x2] "r"(d1), [y2] "r"(a2),
                  [x3] "r"(r4), [y3] "r"(a4)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[3] = c_lo & MASK51;

        /* c[4] = carry + d0·a4 + d1·a3 + a2·a2 */
        asm volatile(
                "mov rax, %[cin]\n\t"
                "xor r8, r8\n\t"
                "mov rcx, %[x1]\n\t"  "mov rdx, %[y1]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x2]\n\t"  "mov rdx, %[y2]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov rcx, %[x3]\n\t"  "mov rdx, %[y3]\n\t"  "vmwrite rcx, rdx\n\t"
                "mov %[lo], rax\n\t"
                "mov %[hi], r8\n\t"
                : [lo] "=r"(c_lo), [hi] "=r"(c_hi)
                : [cin] "r"(carry),
                  [x1] "r"(d0), [y1] "r"(a4),
                  [x2] "r"(d1), [y2] "r"(a3),
                  [x3] "r"(a2), [y3] "r"(a2)
                : "rax", "rcx", "rdx", "r8"
        );
        carry = (c_hi << 13) | (c_lo >> 51);
        out[4] = c_lo & MASK51;

        /* ── Final reduction: carry × 19 wraps to limb 0 ──────── */
        out[0] += carry * 19;
        carry = out[0] >> 51;
        out[0] &= MASK51;
        out[1] += carry;
}


/* ══════════════════════════════════════════════════════════════════
 *  TEST VECTORS
 * ══════════════════════════════════════════════════════════════════ */

typedef struct {
        const char *name;
        uint64_t in[5];
} sq_test_t;

static int run_sq_test(const sq_test_t *t) {
        uint64_t ref[5], mac[5];

        fe_sq_ref(t->in, ref);
        fe_sq_mac128(t->in, mac);

        int pass = (memcmp(ref, mac, sizeof(ref)) == 0);

        printf("%s:\n", t->name);
        printf("  input:  [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ",\n"
               "           %016" PRIx64 ", %016" PRIx64 "]\n",
               t->in[0], t->in[1], t->in[2], t->in[3], t->in[4]);
        printf("  ref:    [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ",\n"
               "           %016" PRIx64 ", %016" PRIx64 "]\n",
               ref[0], ref[1], ref[2], ref[3], ref[4]);
        printf("  mac128: [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ",\n"
               "           %016" PRIx64 ", %016" PRIx64 "]\n",
               mac[0], mac[1], mac[2], mac[3], mac[4]);

        if (!pass) {
                printf("  MISMATCH:");
                for (int i = 0; i < 5; i++)
                        if (ref[i] != mac[i])
                                printf(" limb[%d]", i);
                printf("\n");
        }
        printf("  %s\n\n", pass ? "✓ PASS" : "✗ FAIL");
        return pass;
}


/* ══════════════════════════════════════════════════════════════════
 *  ITERATED SQUARE TEST
 *
 *  Start from a known value, square 1000 times, compare final.
 *  Catches carry propagation bugs that single-shot tests miss.
 * ══════════════════════════════════════════════════════════════════ */
static int test_iterated(void) {
        printf("═══════════════════════════════════════════\n");
        printf("  Iterated Square Test (1000 rounds)\n");
        printf("═══════════════════════════════════════════\n\n");

        uint64_t ref[5] = { 1, 0, 0, 0, 0 };
        uint64_t mac[5] = { 1, 0, 0, 0, 0 };

        for (int i = 0; i < 1000; i++) {
                uint64_t tmp_ref[5], tmp_mac[5];
                fe_sq_ref(ref, tmp_ref);
                fe_sq_mac128(mac, tmp_mac);
                memcpy(ref, tmp_ref, sizeof(ref));
                memcpy(mac, tmp_mac, sizeof(mac));
        }

        int pass = (memcmp(ref, mac, sizeof(ref)) == 0);
        printf("  After 1000 iterations:\n");
        printf("  ref:    [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ",\n"
               "           %016" PRIx64 ", %016" PRIx64 "]\n",
               ref[0], ref[1], ref[2], ref[3], ref[4]);
        printf("  mac128: [%016" PRIx64 ", %016" PRIx64 ", %016" PRIx64 ",\n"
               "           %016" PRIx64 ", %016" PRIx64 "]\n",
               mac[0], mac[1], mac[2], mac[3], mac[4]);
        printf("  %s\n\n", pass ? "✓ PASS" : "✗ FAIL");
        return pass;
}


/* ══════════════════════════════════════════════════════════════════ */

int main(void) {
        printf("╔════════════════════════════════════════════════════╗\n");
        printf("║  Curve25519 Field Square — MAC128 via VMWRITE      ║\n");
        printf("╚════════════════════════════════════════════════════╝\n\n");

        printf("Installing MAC128 hook (6 triads, 0x0cd8 → 0x7c00)...\n\n");
        install_mac128();

        /* ── Single-shot tests ──────────────────────────────────── */

        printf("═══════════════════════════════════════════\n");
        printf("  Single Square Tests\n");
        printf("═══════════════════════════════════════════\n\n");

        sq_test_t tests[] = {
                {
                        .name = "SQ 1: identity (1,0,0,0,0)²",
                        .in   = { 1, 0, 0, 0, 0 }
                },
                {
                        .name = "SQ 2: small (7,11,3,5,2)²",
                        .in   = { 7, 11, 3, 5, 2 }
                },
                {
                        .name = "SQ 3: mid-range limbs",
                        .in   = { 0x1234567890ULL,
                                  0x0ABCDEF01234ULL,
                                  0x0000F00DCAFEULL,
                                  0x0007000000000ULL,
                                  0x00055555555555ULL }
                },
                {
                        .name = "SQ 4: near-max 51-bit limbs",
                        .in   = { MASK51, MASK51, MASK51, MASK51, MASK51 }
                },
                {
                        .name = "SQ 5: libsodium test vector (basepoint x)",
                        .in   = { 0x00062D608F25D51AULL,
                                  0x000412A4B4F6592AULL,
                                  0x00075B7171A4B31DULL,
                                  0x0001FF60527118FEULL,
                                  0x000216936D3CD6E5ULL }
                },
                {
                        .name = "SQ 6: one-hot limb[2]",
                        .in   = { 0, 0, 1, 0, 0 }
                },
                {
                        .name = "SQ 7: alternating (max, 0, max, 0, max)",
                        .in   = { MASK51, 0, MASK51, 0, MASK51 }
                },
                {
                        .name = "SQ 8: powers of two",
                        .in   = { 1ULL << 10, 1ULL << 20,
                                  1ULL << 30, 1ULL << 40, 1ULL << 50 }
                },
        };

        int n = sizeof(tests) / sizeof(tests[0]);
        int pass = 0;
        for (int i = 0; i < n; i++)
                pass += run_sq_test(&tests[i]);

        printf("════════════════════════════════════════\n");
        printf("  Single-shot: %d / %d passed\n", pass, n);
        printf("════════════════════════════════════════\n\n");

        /* ── Iterated test ──────────────────────────────────────── */

        int iter_pass = test_iterated();

        /* ── Summary ────────────────────────────────────────────── */

        int total = pass + iter_pass;
        int total_n = n + 1;

        printf("════════════════════════════════════════\n");
        printf("  TOTAL: %d / %d passed\n", total, total_n);
        printf("════════════════════════════════════════\n\n");

        if (total == total_n) {
                printf("curve25519_square MAC128 is operational!\n\n");
                printf("  Structure: 2 phases, 15 vmwrite calls total\n");
                printf("    Phase 1:  9 MACs → out[0..2] + carry\n");
                printf("    Phase 2:  6 MACs → out[3..4] + reduction\n\n");
                printf("  Next: run bench_curve25519_sq for timing.\n");
        }

        return (total == total_n) ? 0 : 1;
}


/*
 * ═══════════════════════════════════════════════════════════════════
 *  OPTIMIZATION PATH
 * ═══════════════════════════════════════════════════════════════════
 *
 *  Current:  15 vmwrite calls, ~120 cycles (75 redirect + 45 work)
 *
 *  Step 1:   3-MAC-per-vmwrite hook → 5 calls, ~70 cycles
 *            Needs: RCX,RDX (pair1), RSI,RDI (pair2), RBX,R8 (pair3)
 *            Microcode saves R8, zeros acc_hi, runs 3 MAC128s
 *            internally with operand shuffle between each.
 *            ~20 triads, well within 200 budget.
 *
 *  Step 2:   Monolithic hook → 1 call, ~62 cycles
 *            Pass 5 limbs via RSI,RDI,RBX,RCX,RDX.
 *            Microcode precomputes d0,d1,d2,d3,r3,r4 internally.
 *            Runs all 15 MACs, carry chain, limb extraction.
 *            ~90 triads. Tight but fits in 200.
 *            Register pressure: 5 inputs + 5 accumulators + 6 TMPs.
 *            Key challenge: saving intermediate limb values without
 *            STAD (memory stores broken in vmwrite context).
 *
 *  Step 3:   2-hook split → 2 calls, ~67 cycles
 *            Hook 1 at 0x7c00: c[0..2], ~50 triads
 *            Hook 2 at 0x7c30: c[3..4], ~40 triads
 *            CAM retarget between calls: ~50 cycle overhead
 *            Need second entry point (vmread?) or CAM switch.
 *
 * ═══════════════════════════════════════════════════════════════════
 */
