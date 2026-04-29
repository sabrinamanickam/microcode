/*
 * asm_op_p521_mul.c — P-521 field multiplication via microcode (vmwrite) vs native C
 *
 * Field: GF(2^521 - 1),  unsaturated radix-2^58,  9 limbs (limb 8 = 57 bits).
 *
 * Multiplication formula (81 products, 9 per limb):
 *   c[k] = sum over all (i,j) where (i+j) mod 9 == k of:
 *            a[i] * b[j]        if i+j < 9
 *            a[i] * (2*b[j])    if i+j >= 9
 *
 * Reduction: 2^522 = 2 mod p (Mersenne prime).
 *
 * Architecture: 9 vmwrite calls with ONE 20-triad generic 5-MAC patch.
 * Each vmwrite computes HALF a limb (5 of the 9 MACs). Two vmwrite calls
 * per limb (one for the first 5 products, one for the remaining 4 + accumulate).
 *
 * Actually: 9 MACs per limb is too many for one vmwrite with our MAC pattern.
 * Instead: use 9 vmwrites (one per limb), each running a 9-MAC patch (~32 triads).
 * But 9 MACs needs 9 srcA + 9 srcB = 18 register slots, exceeding the 16 TMPs
 * available for PREP.
 *
 * SOLUTION: split each limb's 9 MACs into two passes.
 * Pass 1 (vmwrite): 5 MACs, stores partial 128-bit sum.
 * Pass 2 (vmwrite): 4 MACs + partial sum from pass 1.
 *
 * This gives 18 vmwrites total (2 per limb). Each vmwrite runs the SAME
 * 5-MAC generic patch (or a 4-MAC variant).
 *
 * Alternatively: use the SAME 5-MAC patch for all 18 calls, padding the
 * 4-MAC pass with a zero product (a*0 = 0). This wastes one MUL but keeps
 * the patch simple.
 *
 * Carry chain: flows across limbs in native C after all vmwrites.
 *
 * Register convention (same as squaring):
 *   srcA: RDI, RSI, R12, R11, R14
 *   srcB: R15, R13, R9, R10, RBX
 *   RAX = carry/accumulator input, output
 *   R8 = hi accumulator (zeroed per vmwrite)
 *
 * Build:  make PROG=asm_op_p521_mul
 * Run:    sudo taskset -c 0 ./asm_op_p521_mul_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MASK58 UINT64_C(0x3ffffffffffffff)
#define MASK57 UINT64_C(0x1ffffffffffffff)
#define NLIMBS 9

/* ── fiat-crypto reference (the REAL GCC baseline CryptOpt compares against) ── */
#include "../curvesC/p521_mul.c"

static void fe_mul_fiat(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    fiat_p521_carry_mul(out, a, b);
}

/* ── microcode patch (generic 5-MAC accumulator) ──────────────── */

/*
 * Same patch as p521_sq: 5 MACs + carry extraction.
 * PREP copies srcB from R15,R13,R9,R10,RBX to TMP10-14.
 * Accumulates into carry from RAX.
 * Output: R15=limb value (58-bit masked), RAX=carry for next.
 *
 * For multiplication, each limb needs 9 MACs. We call this patch TWICE:
 * first with 5 products, then with 4 products (plus 1 dummy a*0=0).
 * The carry from the first call feeds into the second.
 * After the second call, we have the complete limb value and carry.
 *
 * Actually, a simpler approach: do NOT extract carry after the first 5 MACs.
 * Instead, accumulate 5 MACs in pass 1, return the RAW 128-bit sum
 * (hi:lo split across two arch regs). Pass 2 adds 4 more MACs to the sum,
 * THEN does carry extraction.
 *
 * But 128-bit intermediate values don't fit in one 64-bit register.
 * We need TWO regs: R15 for lo, RAX for hi. Then pass 2's patch starts
 * with ZEROEXT(TMP0, R15) for lo and moves hi from RAX...
 *
 * This complicates the patch design. Let's keep it simple:
 * carry extract after both pass 1 and pass 2, add pass 1's output to
 * pass 2's output in C.
 *
 * Even simpler: compute all 81 products and carry chain entirely in C,
 * using vmwrite only for the MUL operations (the bottleneck). Each vmwrite
 * does 5 MULs, returns 5 (hi, lo) pairs. But we can't return 10 values
 * from one vmwrite.
 *
 * SIMPLEST CORRECT APPROACH: the same 5-MAC patch, called 18 times
 * (2 per limb). For each limb:
 *   Call 1 (5 products): RAX=0 in, RAX=carry_1 out, R15=partial_limb_1
 *   Call 2 (4 products + 1 dummy): RAX=0 in, RAX=carry_2 out, R15=partial_limb_2
 *   In C: full_sum = partial_1 + partial_2 + (carry_1 << 58) + (carry_2 << 58)
 *   But this doesn't work because partial_1 and partial_2 are each 58-bit masked.
 *   The true sum is > 58 bits.
 *
 * The 128-bit sum for a limb with 9 terms is at most 9 * 2^116 < 2^120.
 * Splitting into two groups (5 and 4 terms):
 *   Group 1 sum < 5 * 2^116 < 2^119
 *   Group 2 sum < 4 * 2^116 < 2^118
 * Each group's sum fits in 128 bits. After carry extraction:
 *   partial_limb = sum & MASK58 (58 bits)
 *   carry = sum >> 58 (at most ~61 bits)
 *
 * The true limb value = (group1_sum + group2_sum) & MASK58.
 * We can compute this as: (partial1 + partial2 + (carry1 << 58) ??? )
 *
 * No, this is wrong. We need the COMBINED sum, not separate sums.
 *
 * CORRECT APPROACH: pass the carry from group 1 into group 2.
 *   Call 1: RAX=0 in. Computes 5 MACs. RAX=carry_1 out, R15=limb_part_1.
 *   Call 2: RAX=carry_1 in (as init). Computes 4 MACs. RAX=carry_total, R15=limb_value.
 *
 * Wait — the init accumulator in the patch is: TMP0 = RAX (carry from previous).
 * If we pass carry_1 as RAX for call 2, the patch adds 4 MAC products to carry_1.
 * But carry_1 is the CARRY (high bits), not the partial sum!
 *
 * The carry_1 = (group1_sum >> 58) + hi_accumulation.
 * The limb_part_1 = group1_sum & MASK58.
 *
 * What we want for call 2: init with limb_part_1 (not carry_1).
 * Then add 4 more products. Then extract carry.
 *
 * So: RAX = limb_part_1 for call 2's init. The patch accumulates on top of it.
 *
 * But then carry from call 1 is lost! We need to pass it separately.
 *
 * REVISED: use TWO registers between calls.
 * After call 1: R15 = limb_part_1, RAX = carry_1.
 * For call 2: pass limb_part_1 as init in RAX. Save carry_1 somewhere else.
 * After call 2: R15 = final_limb_value (limb_part_1 + group2 products, masked).
 *               RAX = carry from this combined limb.
 *               PLUS carry_1 from call 1.
 * Total carry = carry_1 + carry_from_call_2.
 *
 * Actually no: if we init call 2 with limb_part_1 and add 4 more products:
 * call_2_sum = limb_part_1 + group2_products.
 * call_2_limb = call_2_sum & MASK58 = (group1_sum & MASK58 + group2_products) & MASK58.
 *
 * But the TRUE limb = (group1_sum + group2_sum) & MASK58, which is DIFFERENT
 * because (A & MASK) + B) & MASK != (A + B) & MASK when A overflows.
 *
 * Hmm, but group1_sum & MASK58 = group1_sum mod 2^58. Adding group2_products:
 * (group1_sum mod 2^58) + group2_products = group1_sum + group2_products - carry1 * 2^58.
 * So: result mod 2^58 = (group1_sum + group2_products) mod 2^58 = correct limb!
 * And: result >> 58 = floor((group1_sum mod 2^58 + group2_products) / 2^58).
 * Total carry = carry1 + result >> 58.
 *
 * Wait, that works! Because:
 * group1_sum = carry1 * 2^58 + partial1.
 * group1_sum + group2_sum = carry1 * 2^58 + partial1 + group2_sum.
 * (partial1 + group2_sum) = total_partial + carry2 * 2^58.
 * So total = (carry1 + carry2) * 2^58 + total_partial.
 * Where total_partial = limb_value and carry1 + carry2 = total_carry.
 *
 * So yes: init call 2 with partial1 (R15 from call 1), run 4 MACs,
 * extract carry2 and limb_value. Total carry = carry1 + carry2.
 * Carry1 is saved in a C variable between calls.
 *
 * IMPLEMENTATION:
 * For each limb k:
 *   save carry_total from previous limb
 *   // Call 1: 5 products
 *   set up srcA[0..4], srcB[0..4] for products 0-4
 *   RAX = carry_total (from previous limb)
 *   vmwrite
 *   carry1 = RAX (from patch)
 *   partial1 = R15 (from patch)
 *   // Call 2: 4 products + 1 dummy (0*0)
 *   set up srcA[0..3], srcB[0..3] for products 5-8, srcA[4]=0, srcB[4]=0
 *   RAX = partial1 (feed limb_part_1 into call 2)
 *   vmwrite
 *   carry2 = RAX (from patch)
 *   limb_value = R15 (from patch)
 *   carry_total = carry1 + carry2 (for next limb)
 *
 * 18 vmwrites total. Each vmwrite: ~20 triads × ~1 cycle = ~20 cycles.
 * Plus vmwrite overhead: ~5 cycles each. Total: 18 × 25 = 450 cycles.
 * Plus inline asm setup: ~10 mov per call × 18 = 180 instructions ≈ ~100 cycles.
 * Grand total: ~550 cycles.
 *
 * GCC -O3 for P-521 mul: 81 MULs × 4 cycles ≈ 324 cycles + overhead ≈ 400 cycles.
 *
 * So microcode might be a bit slower due to vmwrite overhead. But let's
 * implement it correctly first and benchmark.
 */

static void install_p521_mul_patch(void) {
    ucode_t patch[] = {

    /* PREP: copy srcB from arch regs to TMPs, zero hi accumulator */
    /* P0 */ { ZEROEXT_DSZ64_DR(TMP10, R15),
               ZEROEXT_DSZ64_DR(TMP11, R13),
               ZEROEXT_DSZ64_DR(TMP12, R9),
               NOP_SEQWORD },
    /* P1 */ { ZEROEXT_DSZ64_DR(TMP13, R10),
               ZEROEXT_DSZ64_DR(TMP14, RBX),
               NOTAND_DSZ64_DRR(R8, R8, R8),
               NOP_SEQWORD },

    /* INIT: set accumulator lo = carry/partial from RAX */
    /* I0 */ { ZEROEXT_DSZ64_DR(TMP0, RAX),
               NOP, NOP, NOP_SEQWORD },

    /* ═══ MAC 1: RDI × TMP10 ═══ */
    /* M1-0 */ { MUL_DSZ64_DRR(RCX, RDI, TMP10),
                 NOP, NOP, NOP_SEQWORD },
    /* M1-1 */ { ADD_DSZ64_DRR(TMP2, TMP0, TMP10),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 NOP, NOP_SEQWORD },

    /* ═══ MAC 2: RSI × TMP11 ═══ */
    /* M2-0 */ { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, RSI, TMP11),
                 NOP, NOP_SEQWORD },
    /* M2-1 */ { ADD_DSZ64_DRR(TMP0, TMP2, TMP11),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },

    /* ═══ MAC 3: R12 × TMP12 ═══ */
    /* M3-0 */ { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R12, TMP12),
                 NOP, NOP_SEQWORD },
    /* M3-1 */ { ADD_DSZ64_DRR(TMP2, TMP0, TMP12),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 NOP, NOP_SEQWORD },

    /* ═══ MAC 4: R11 × TMP13 ═══ */
    /* M4-0 */ { ADD_DSZ64_DRR(TMP6, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R11, TMP13),
                 NOP, NOP_SEQWORD },
    /* M4-1 */ { ADD_DSZ64_DRR(TMP0, TMP2, TMP13),
                 SETCC_CONDB_DR(TMP3, TMP0),
                 NOP, NOP_SEQWORD },

    /* ═══ MAC 5: R14 × TMP14 ═══ */
    /* M5-0 */ { ADD_DSZ64_DRR(TMP7, RCX, TMP3),
                 MUL_DSZ64_DRR(RCX, R14, TMP14),
                 NOP, NOP_SEQWORD },
    /* M5-1 */ { ADD_DSZ64_DRR(TMP2, TMP0, TMP14),
                 SETCC_CONDB_DR(TMP3, TMP2),
                 NOP, NOP_SEQWORD },

    /* ═══ CARRY EXTRACTION (58-bit) ═══ */
    /* CE-0 */ { SHR_DSZ64_DRI(TMP8, TMP2, 58),
                 ADD_DSZ64_DRR(TMP0, RCX, TMP3),
                 NOP, NOP_SEQWORD },
    /* CE-1 */ { SHL_DSZ64_DRI(TMP9, TMP2, 6),
                 ADD_DSZ64_DRR(TMP1, TMP4, TMP5),
                 ADD_DSZ64_DRR(TMP0, TMP6, TMP0),
                 NOP_SEQWORD },
    /* CE-2 */ { ADD_DSZ64_DRR(R8, R8, TMP7),
                 ADD_DSZ64_DRR(TMP0, TMP1, TMP0),
                 NOP, NOP_SEQWORD },
    /* CE-3 */ { SHR_DSZ64_DRI(R15, TMP9, 6),
                 ADD_DSZ64_DRR(R8, R8, TMP0),
                 NOP, NOP_SEQWORD },

    /* ═══ COMBINE CARRY: RAX = TMP8 | (R8 << 6) ═══ */
    /* CC-0 */ { SHL_DSZ64_DRI(TMP1, R8, 6),
                 NOP, NOP, NOP_SEQWORD },
    /* CC-1 */ { OR_DSZ64_DRR(RAX, TMP8, TMP1),
                 NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("p521_mul: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ── fe_mul native C ─────────────────────────────────────────── */

static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t a0=a[0], a1=a[1], a2=a[2], a3=a[3], a4=a[4];
    uint64_t a5=a[5], a6=a[6], a7=a[7], a8=a[8];
    uint64_t b0=b[0], b1=b[1], b2=b[2], b3=b[3], b4=b[4];
    uint64_t b5=b[5], b6=b[6], b7=b[7], b8=b[8];
    /* Reduced (wrapped) b values: multiply by 2 */
    uint64_t r1=2*b1, r2=2*b2, r3=2*b3, r4=2*b4;
    uint64_t r5=2*b5, r6=2*b6, r7=2*b7, r8=2*b8;

    /* c[k] = sum of a[i]*b[j] for i+j==k (unreduced)
     *       + sum of a[i]*2*b[j] for i+j==k+9 (reduced) */

    __uint128_t c0 = (__uint128_t)a0*b0 + (__uint128_t)a1*r8 + (__uint128_t)a2*r7
                   + (__uint128_t)a3*r6 + (__uint128_t)a4*r5 + (__uint128_t)a5*r4
                   + (__uint128_t)a6*r3 + (__uint128_t)a7*r2 + (__uint128_t)a8*r1;

    __uint128_t c1 = (__uint128_t)a0*b1 + (__uint128_t)a1*b0 + (__uint128_t)a2*r8
                   + (__uint128_t)a3*r7 + (__uint128_t)a4*r6 + (__uint128_t)a5*r5
                   + (__uint128_t)a6*r4 + (__uint128_t)a7*r3 + (__uint128_t)a8*r2;

    __uint128_t c2 = (__uint128_t)a0*b2 + (__uint128_t)a1*b1 + (__uint128_t)a2*b0
                   + (__uint128_t)a3*r8 + (__uint128_t)a4*r7 + (__uint128_t)a5*r6
                   + (__uint128_t)a6*r5 + (__uint128_t)a7*r4 + (__uint128_t)a8*r3;

    __uint128_t c3 = (__uint128_t)a0*b3 + (__uint128_t)a1*b2 + (__uint128_t)a2*b1
                   + (__uint128_t)a3*b0 + (__uint128_t)a4*r8 + (__uint128_t)a5*r7
                   + (__uint128_t)a6*r6 + (__uint128_t)a7*r5 + (__uint128_t)a8*r4;

    __uint128_t c4 = (__uint128_t)a0*b4 + (__uint128_t)a1*b3 + (__uint128_t)a2*b2
                   + (__uint128_t)a3*b1 + (__uint128_t)a4*b0 + (__uint128_t)a5*r8
                   + (__uint128_t)a6*r7 + (__uint128_t)a7*r6 + (__uint128_t)a8*r5;

    __uint128_t c5 = (__uint128_t)a0*b5 + (__uint128_t)a1*b4 + (__uint128_t)a2*b3
                   + (__uint128_t)a3*b2 + (__uint128_t)a4*b1 + (__uint128_t)a5*b0
                   + (__uint128_t)a6*r8 + (__uint128_t)a7*r7 + (__uint128_t)a8*r6;

    __uint128_t c6 = (__uint128_t)a0*b6 + (__uint128_t)a1*b5 + (__uint128_t)a2*b4
                   + (__uint128_t)a3*b3 + (__uint128_t)a4*b2 + (__uint128_t)a5*b1
                   + (__uint128_t)a6*b0 + (__uint128_t)a7*r8 + (__uint128_t)a8*r7;

    __uint128_t c7 = (__uint128_t)a0*b7 + (__uint128_t)a1*b6 + (__uint128_t)a2*b5
                   + (__uint128_t)a3*b4 + (__uint128_t)a4*b3 + (__uint128_t)a5*b2
                   + (__uint128_t)a6*b1 + (__uint128_t)a7*b0 + (__uint128_t)a8*r8;

    __uint128_t c8 = (__uint128_t)a0*b8 + (__uint128_t)a1*b7 + (__uint128_t)a2*b6
                   + (__uint128_t)a3*b5 + (__uint128_t)a4*b4 + (__uint128_t)a5*b3
                   + (__uint128_t)a6*b2 + (__uint128_t)a7*b1 + (__uint128_t)a8*b0;

    uint64_t carry;
    carry = (uint64_t)(c0 >> 58); out[0] = (uint64_t)c0 & MASK58;
    c1 += carry;
    carry = (uint64_t)(c1 >> 58); out[1] = (uint64_t)c1 & MASK58;
    c2 += carry;
    carry = (uint64_t)(c2 >> 58); out[2] = (uint64_t)c2 & MASK58;
    c3 += carry;
    carry = (uint64_t)(c3 >> 58); out[3] = (uint64_t)c3 & MASK58;
    c4 += carry;
    carry = (uint64_t)(c4 >> 58); out[4] = (uint64_t)c4 & MASK58;
    c5 += carry;
    carry = (uint64_t)(c5 >> 58); out[5] = (uint64_t)c5 & MASK58;
    c6 += carry;
    carry = (uint64_t)(c6 >> 58); out[6] = (uint64_t)c6 & MASK58;
    c7 += carry;
    carry = (uint64_t)(c7 >> 58); out[7] = (uint64_t)c7 & MASK58;
    c8 += carry;
    carry = (uint64_t)(c8 >> 57); out[8] = (uint64_t)c8 & MASK57;
    out[0] += carry;
    carry = out[0] >> 58; out[0] &= MASK58;
    out[1] += carry;
    carry = out[1] >> 58; out[1] &= MASK58;
    out[2] += carry;
}

/* ── reference (naive schoolbook) ────────────────────────────── */

static void fe_mul_reference(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    __uint128_t t[17] = {0};
    for (int i = 0; i < 9; i++)
        for (int j = 0; j < 9; j++)
            t[i + j] += (__uint128_t)a[i] * b[j];
    /* 2^(9*58) = 2^522 ≡ 2 mod p */
    for (int i = 9; i <= 16; i++)
        t[i - 9] += t[i] * 2;
    __uint128_t carry = 0;
    for (int i = 0; i < 9; i++) {
        t[i] += carry;
        int bits = (i == 8) ? 57 : 58;
        uint64_t mask = (i == 8) ? MASK57 : MASK58;
        carry = t[i] >> bits;
        out[i] = (uint64_t)t[i] & mask;
    }
    out[0] += (uint64_t)carry;
    carry = out[0] >> 58; out[0] &= MASK58;
    out[1] += (uint64_t)carry;
    carry = out[1] >> 58; out[1] &= MASK58;
    out[2] += (uint64_t)carry;
}

/* ── helper: run one 5-MAC vmwrite ────────────────────────────── */

static inline void run_5mac(uint64_t sa0, uint64_t sb0,
                            uint64_t sa1, uint64_t sb1,
                            uint64_t sa2, uint64_t sb2,
                            uint64_t sa3, uint64_t sb3,
                            uint64_t sa4, uint64_t sb4,
                            uint64_t carry_in,
                            uint64_t *limb_out, uint64_t *carry_out) {
    register uint64_t _sa0 asm("rdi") = sa0;
    register uint64_t _sb0 asm("r15") = sb0;
    register uint64_t _sa1 asm("rsi") = sa1;
    register uint64_t _sb1 asm("r13") = sb1;
    register uint64_t _sa2 asm("r12") = sa2;
    register uint64_t _sb2 asm("r9")  = sb2;
    register uint64_t _sa3 asm("r11") = sa3;
    register uint64_t _sb3 asm("r10") = sb3;
    register uint64_t _sa4 asm("r14") = sa4;
    register uint64_t _sb4 asm("rbx") = sb4;
    register uint64_t _cin asm("rax") = carry_in;

    asm volatile(
        "vmwrite rcx, rdx\n\t"
        : "+a"(_cin), "+r"(_sb0)
        : "r"(_sa0), "r"(_sa1), "r"(_sa2), "r"(_sa3), "r"(_sa4),
          "r"(_sb1), "r"(_sb2), "r"(_sb3), "r"(_sb4)
        : "rcx", "rdx", "r8", "memory", "cc"
    );

    *limb_out = _sb0;   /* R15 */
    *carry_out = _cin;   /* RAX */
}

/* ── fe_mul via microcode ─────────────────────────────────────── */

static void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    /* Precompute reduced b values: 2*b[j] for Mersenne reduction */
    uint64_t bvals[9], rvals[9];
    for (int j = 0; j < 9; j++) {
        bvals[j] = b[j];
        rvals[j] = 2 * b[j];
    }

    /* For each limb k: 9 products split into pass1 (5 MACs) + pass2 (4+1 dummy).
     * For limb k, product m uses a[m] * B where:
     *   B = b[(k-m) mod 9]     if m <= k  (unreduced, i+j < 9)
     *   B = 2*b[(k-m+9) mod 9] if m > k   (reduced, i+j >= 9)
     */

    uint64_t carry_total = 0;

    for (int k = 0; k < 9; k++) {
        /* Compute srcA[9] and srcB[9] for this limb */
        uint64_t sA[9], sB[9];
        for (int m = 0; m < 9; m++) {
            sA[m] = a[m];
            int j = (k - m + 9) % 9;  /* b index */
            if (m <= k) {
                sB[m] = bvals[j];     /* unreduced */
            } else {
                sB[m] = rvals[j];     /* reduced: 2*b[j] */
            }
        }

        /* Pass 1: 5 MACs (products 0-4) */
        uint64_t limb1, carry1;
        run_5mac(sA[0], sB[0], sA[1], sB[1], sA[2], sB[2], sA[3], sB[3], sA[4], sB[4],
                 carry_total, &limb1, &carry1);

        /* Pass 2: 4 MACs (products 5-8) + dummy, init with limb1 */
        uint64_t limb2, carry2;
        run_5mac(sA[5], sB[5], sA[6], sB[6], sA[7], sB[7], sA[8], sB[8], 0, 0,
                 limb1, &limb2, &carry2);

        out[k] = limb2;
        carry_total = carry1 + carry2;
    }

    /* Fix c8: re-extract with 57-bit mask */
    uint64_t extra = out[8] >> 57;
    out[8] &= MASK57;
    uint64_t wrap_carry = carry_total * 2 + extra;

    out[0] += wrap_carry;
    uint64_t c = out[0] >> 58; out[0] &= MASK58;
    out[1] += c;
    c = out[1] >> 58; out[1] &= MASK58;
    out[2] += c;
}

/* ── verification ────────────────────────────────────────────── */

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

typedef struct {
    const char *label;
    uint64_t    a[9];
    uint64_t    b[9];
    uint64_t    expected[9];
    int         has_expected;
} test_vec_t;

static const test_vec_t test_vectors[] = {
    { "0*0",
      {0,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0}, 1 },
    { "1*1",
      {1,0,0,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0,0}, 1 },
    { "0*1",
      {0,0,0,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0}, 1 },
    { "2*3",
      {2,0,0,0,0,0,0,0,0}, {3,0,0,0,0,0,0,0,0}, {6,0,0,0,0,0,0,0,0}, 1 },
    { "9*9",
      {9,0,0,0,0,0,0,0,0}, {9,0,0,0,0,0,0,0,0}, {81,0,0,0,0,0,0,0,0}, 1 },
    { "2^58*2^58",
      {0,1,0,0,0,0,0,0,0}, {0,1,0,0,0,0,0,0,0}, {0,0,1,0,0,0,0,0,0}, 1 },
    /* 2^261 * 2^261 = 2^522 = 2 mod p */
    { "2^261*2^261",
      {0,0,0,0,UINT64_C(1)<<29,0,0,0,0},
      {0,0,0,0,UINT64_C(1)<<29,0,0,0,0},
      {2,0,0,0,0,0,0,0,0}, 1 },
    { "1*x=x",
      {1,0,0,0,0,0,0,0,0},
      {0x123456789ABCDEFULL & MASK58, 0x0FEDCBA987654321ULL & MASK58,
       0x1111111111111111ULL & MASK58, 0x2222222222222222ULL & MASK58,
       0x3333333333333333ULL & MASK58, 0x0444444444444444ULL & MASK58,
       0x0555555555555555ULL & MASK58, 0x0666666666666666ULL & MASK58,
       0x0777777777777777ULL & MASK57},
      {0x123456789ABCDEFULL & MASK58, 0x0FEDCBA987654321ULL & MASK58,
       0x1111111111111111ULL & MASK58, 0x2222222222222222ULL & MASK58,
       0x3333333333333333ULL & MASK58, 0x0444444444444444ULL & MASK58,
       0x0555555555555555ULL & MASK58, 0x0666666666666666ULL & MASK58,
       0x0777777777777777ULL & MASK57}, 1 },
    { "all_max*all_max",
      {MASK58,MASK58,MASK58,MASK58,MASK58,MASK58,MASK58,MASK58,MASK57},
      {MASK58,MASK58,MASK58,MASK58,MASK58,MASK58,MASK58,MASK58,MASK57},
      {0}, 0 },
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[9], nat[9], ucd[9];
    fe_mul_reference(t->a, t->b, ref);
    fe_mul_native(t->a, t->b, nat);
    fe_mul_ucode(t->a, t->b, ucd);

    int ok = 1;

    if (memcmp(ref, nat, 72) != 0) {
        printf("  FAIL [%s] native != reference\n", t->label);
        for (int i = 0; i < 9; i++)
            printf("    [%d] native=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, nat[i], ref[i], nat[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (memcmp(ref, ucd, 72) != 0) {
        printf("  FAIL [%s] ucode != reference\n", t->label);
        for (int i = 0; i < 9; i++)
            printf("    [%d] ucode=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, ucd[i], ref[i], ucd[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (t->has_expected && memcmp(ref, t->expected, 72) != 0) {
        printf("  FAIL [%s] result != expected\n", t->label);
        for (int i = 0; i < 9; i++)
            printf("    [%d] got=%016" PRIx64 " exp=%016" PRIx64 " %s\n",
                   i, ref[i], t->expected[i],
                   ref[i] == t->expected[i] ? "" : "***");
        ok = 0;
    }
    if (ok) printf("  PASS [%s]\n", t->label);
    return ok;
}

static int verify_random_quiet(const uint64_t a[9], const uint64_t b[9]) {
    uint64_t ref[9], nat[9], ucd[9];
    fe_mul_reference(a, b, ref);
    fe_mul_native(a, b, nat);
    fe_mul_ucode(a, b, ucd);
    if (memcmp(ref, nat, 72) != 0 || memcmp(ref, ucd, 72) != 0) {
        printf("  FAIL random: a={");
        for (int i = 0; i < 9; i++) printf("%s%016" PRIx64, i?",":"", a[i]);
        printf("} b={");
        for (int i = 0; i < 9; i++) printf("%s%016" PRIx64, i?",":"", b[i]);
        printf("}\n");
        if (memcmp(ref, nat, 72) != 0) {
            printf("    native mismatch:");
            for (int i = 0; i < 9; i++) printf(" %016" PRIx64, nat[i]);
            printf("\n");
        }
        if (memcmp(ref, ucd, 72) != 0) {
            printf("    ucode  mismatch:");
            for (int i = 0; i < 9; i++) printf(" %016" PRIx64, ucd[i]);
            printf("\n");
        }
        printf("    reference:      ");
        for (int i = 0; i < 9; i++) printf(" %016" PRIx64, ref[i]);
        printf("\n");
        return 0;
    }
    return 1;
}

#define RANDOM_TESTS 10000
#define CHAIN_ITERS  1000

static int verify_all(void) {
    int pass = 0, fail = 0;

    printf("--- Known test vectors ---\n");
    for (int i = 0; i < (int)N_VECS; i++) {
        if (verify_one(&test_vectors[i])) pass++; else fail++;
    }

    printf("\n--- Random stress test (%d vectors) ---\n", RANDOM_TESTS);
    uint64_t rng = 0xDEADBEEFCAFE1234ULL;
    int rpass = 0;
    for (int i = 0; i < RANDOM_TESTS; i++) {
        uint64_t a[9], b[9];
        for (int j = 0; j < 8; j++) {
            a[j] = splitmix64(&rng) & MASK58;
            b[j] = splitmix64(&rng) & MASK58;
        }
        a[8] = splitmix64(&rng) & MASK57;
        b[8] = splitmix64(&rng) & MASK57;
        if (verify_random_quiet(a, b)) rpass++;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    printf("\n--- Iterated chain (%d mul-self from (2,0,...,0)) ---\n", CHAIN_ITERS);
    uint64_t ri[9] = {2,0,0,0,0,0,0,0,0};
    uint64_t ni[9], ui[9];
    memcpy(ni, ri, 72); memcpy(ui, ri, 72);
    for (int i = 0; i < CHAIN_ITERS; i++) {
        uint64_t tmp[9];
        memcpy(tmp, ri, 72); fe_mul_reference(tmp, tmp, ri);
        memcpy(tmp, ni, 72); fe_mul_native(tmp, tmp, ni);
        memcpy(tmp, ui, 72); fe_mul_ucode(tmp, tmp, ui);
    }
    int ref_nat = memcmp(ri, ni, 72) == 0;
    int ref_ucd = memcmp(ri, ui, 72) == 0;
    printf("  ref==native: %s   ref==ucode: %s   -> %s\n",
           ref_nat ? "yes" : "NO", ref_ucd ? "yes" : "NO",
           (ref_nat && ref_ucd) ? "PASS" : "FAIL");
    if (ref_nat && ref_ucd) pass++; else fail++;

    printf("\n=== Verification: %d passed, %d failed ===\n\n", pass, fail);
    return fail;
}

/* ── timing ───────────────────────────────────────────────────── */

static inline uint64_t rdtsc_start(void) {
    uint32_t lo, hi;
    asm volatile("cpuid\n\trdtsc" : "=a"(lo), "=d"(hi) :: "rbx", "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdtsc_end(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx", "memory");
    asm volatile("cpuid" ::: "rax", "rbx", "rcx", "rdx", "memory");
    return ((uint64_t)hi << 32) | lo;
}

#define BATCH 1000
#define REPS  100

int main(void) {
    uint64_t t0, t1, min, sum;

    printf("=== P-521 field multiplication: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_p521_mul_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    uint64_t state_a[9] = {
        0x00062D608F25D51AULL & MASK58, 0x000412A4B4F6592AULL & MASK58,
        0x00075B7171A4B31DULL & MASK58, 0x0001FF60527118FEULL & MASK58,
        0x000216936D3CD6E5ULL & MASK58, 0x0003B0A65E59EC35ULL & MASK58,
        0x00025EA3B4488A68ULL & MASK58, 0x0001DB1232A6754AULL & MASK58,
        0x0000E5B7C53A1B5EULL & MASK57,
    };
    uint64_t state_b[9] = {
        0x0006B17D1F2E12C4ULL & MASK58, 0x000CF2546785FD89ULL & MASK58,
        0x00068B2F9BCCDC68ULL & MASK58, 0x000E8AFBFC23A862ULL & MASK58,
        0x00014FE13A0540EAULL & MASK58, 0x000A1B2C3D4E5F60ULL & MASK58,
        0x0007890ABCDEF012ULL & MASK58, 0x000456789ABCDE01ULL & MASK58,
        0x000123456789AB00ULL & MASK57,
    };

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t tmp_a[9], tmp_b[9];

    /* native C -O3 */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, sizeof(tmp_a));
        memcpy(tmp_b, state_b, sizeof(tmp_b));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_native(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Naive -O3:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* fiat-crypto (the real GCC baseline CryptOpt compares against) */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, sizeof(tmp_a));
        memcpy(tmp_b, state_b, sizeof(tmp_b));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_fiat(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Fiat-crypto: min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* microcode */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, sizeof(tmp_a));
        memcpy(tmp_b, state_b, sizeof(tmp_b));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_ucode(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* clean up */
    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
