/*
 * asm_op_p521_sq.c — P-521 field squaring via microcode (vmwrite) vs native C
 *
 * Field: GF(2^521 - 1),  unsaturated radix-2^58,  9 limbs (limb 8 = 57 bits).
 *
 * Squaring formula (45 products, 5 per limb):
 *   c0 = a0^2       + 4*a1*a8 + 4*a2*a7 + 4*a3*a6 + 4*a4*a5
 *   c1 = 2*a0*a1    + 4*a2*a8 + 4*a3*a7 + 4*a4*a6 + 2*a5^2
 *   c2 = 2*a0*a2    + a1^2    + 4*a3*a8 + 4*a4*a7 + 4*a5*a6
 *   c3 = 2*a0*a3    + 2*a1*a2 + 4*a4*a8 + 4*a5*a7 + 2*a6^2
 *   c4 = 2*a0*a4    + 2*a1*a3 + a2^2    + 4*a5*a8 + 4*a6*a7
 *   c5 = 2*a0*a5    + 2*a1*a4 + 2*a2*a3 + 4*a6*a8 + 2*a7^2
 *   c6 = 2*a0*a6    + 2*a1*a5 + 2*a2*a4 + a3^2    + 4*a7*a8
 *   c7 = 2*a0*a7    + 2*a1*a6 + 2*a2*a5 + 2*a3*a4 + 2*a8^2
 *   c8 = 2*a0*a8    + 2*a1*a7 + 2*a2*a6 + 2*a3*a5 + a4^2
 *
 * Reduction: 2^522 = 2 mod p (Mersenne). Cross-terms that wrap get ×2
 * (reduction) plus ×2 (squaring symmetry) = ×4.
 *
 * Architecture: 9 vmwrite calls with ONE 20-triad generic patch.
 * Each vmwrite computes one limb (5 MACs + carry extraction).
 * The inline asm precomputes srcA[5] and srcB[5] for each limb,
 * loads them into registers, and calls vmwrite.
 * Carry propagates via RAX between calls.
 * Wrap-around carry (c8->c0->c1) done in native C.
 *
 * Patch register convention:
 *   srcA: RDI, RSI, R12, R11, R14 (5 multiplier A-operands)
 *   srcB: R15, R13, R9, R10, RBX  (5 multiplier B-operands, copied to TMPs)
 *   RAX = carry from previous limb (input), output limb (output)
 *   R8 = hi accumulator (zeroed at start of each limb)
 *   RCX, RDX = MUL scratch
 *
 * After vmwrite: R15 = output limb value, RAX = carry for next limb.
 *
 * Build:  make PROG=asm_op_p521_sq
 * Run:    sudo taskset -c 0 ./asm_op_p521_sq_static
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

/* ── microcode patch (generic 5-MAC limb) ─────────────────────── */

static void install_p521_sq_patch(void) {
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

    /* INIT: set accumulator lo = carry from previous limb (RAX) */
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
    /* Extract: limb_value = lo_acc & MASK58, carry = lo_acc >> 58 + hi_acc */
    /* CE-0 */ { SHR_DSZ64_DRI(TMP8, TMP2, 58),    /* lo carry bits */
                 ADD_DSZ64_DRR(TMP0, RCX, TMP3),    /* MAC5 hi contribution */
                 NOP, NOP_SEQWORD },
    /* CE-1 */ { SHL_DSZ64_DRI(TMP9, TMP2, 6),     /* for 58-bit mask */
                 ADD_DSZ64_DRR(TMP1, TMP4, TMP5),   /* combine hi parts */
                 ADD_DSZ64_DRR(TMP0, TMP6, TMP0),
                 NOP_SEQWORD },
    /* CE-2 */ { ADD_DSZ64_DRR(R8, R8, TMP7),       /* add MAC5 hi part */
                 ADD_DSZ64_DRR(TMP0, TMP1, TMP0),   /* total hi */
                 NOP, NOP_SEQWORD },
    /* CE-3 */ { SHR_DSZ64_DRI(R15, TMP9, 6),       /* R15 = output limb */
                 ADD_DSZ64_DRR(R8, R8, TMP0),        /* R8 = final hi acc */
                 NOP, NOP_SEQWORD },

    /* ═══ COMBINE CARRY: RAX = TMP8 | (R8 << 6) ═══ */
    /* CC-0 */ { SHL_DSZ64_DRI(TMP1, R8, 6),
                 NOP, NOP, NOP_SEQWORD },
    /* CC-1 */ { OR_DSZ64_DRR(RAX, TMP8, TMP1),
                 NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("p521_sq: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ── fe_sq native C ──────────────────────────────────────────── */

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    uint64_t a0=a[0], a1=a[1], a2=a[2], a3=a[3], a4=a[4];
    uint64_t a5=a[5], a6=a[6], a7=a[7], a8=a[8];

    __uint128_t c0 = (__uint128_t)a0*a0     + (__uint128_t)a1*(4*a8) + (__uint128_t)a2*(4*a7) + (__uint128_t)a3*(4*a6) + (__uint128_t)a4*(4*a5);
    __uint128_t c1 = (__uint128_t)a0*(2*a1)  + (__uint128_t)a2*(4*a8) + (__uint128_t)a3*(4*a7) + (__uint128_t)a4*(4*a6) + (__uint128_t)a5*(2*a5);
    __uint128_t c2 = (__uint128_t)a0*(2*a2)  + (__uint128_t)a1*a1     + (__uint128_t)a3*(4*a8) + (__uint128_t)a4*(4*a7) + (__uint128_t)a5*(4*a6);
    __uint128_t c3 = (__uint128_t)a0*(2*a3)  + (__uint128_t)a1*(2*a2) + (__uint128_t)a4*(4*a8) + (__uint128_t)a5*(4*a7) + (__uint128_t)a6*(2*a6);
    __uint128_t c4 = (__uint128_t)a0*(2*a4)  + (__uint128_t)a1*(2*a3) + (__uint128_t)a2*a2     + (__uint128_t)a5*(4*a8) + (__uint128_t)a6*(4*a7);
    __uint128_t c5 = (__uint128_t)a0*(2*a5)  + (__uint128_t)a1*(2*a4) + (__uint128_t)a2*(2*a3) + (__uint128_t)a6*(4*a8) + (__uint128_t)a7*(2*a7);
    __uint128_t c6 = (__uint128_t)a0*(2*a6)  + (__uint128_t)a1*(2*a5) + (__uint128_t)a2*(2*a4) + (__uint128_t)a3*a3     + (__uint128_t)a7*(4*a8);
    __uint128_t c7 = (__uint128_t)a0*(2*a7)  + (__uint128_t)a1*(2*a6) + (__uint128_t)a2*(2*a5) + (__uint128_t)a3*(2*a4) + (__uint128_t)a8*(2*a8);
    __uint128_t c8 = (__uint128_t)a0*(2*a8)  + (__uint128_t)a1*(2*a7) + (__uint128_t)a2*(2*a6) + (__uint128_t)a3*(2*a5) + (__uint128_t)a4*a4;

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

static void fe_sq_reference(const uint64_t *a, uint64_t *out) {
    __uint128_t t[17] = {0};
    for (int i = 0; i < 9; i++)
        for (int j = 0; j < 9; j++)
            t[i + j] += (__uint128_t)a[i] * a[j];
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

/* ── fe_sq via microcode ─────────────────────────────────────── */

/*
 * Product table for squaring.
 * For each limb k (0..8), 5 products: srcA[j] * srcB[j] for j=0..4.
 *
 * The inline asm precomputes all srcB values (some are 2*a[i] or 4*a[i]),
 * then for each limb loads srcA[0..4] and srcB[0..4] into the designated
 * arch regs and fires vmwrite.
 *
 * srcA goes into: RDI, RSI, R12, R11, R14
 * srcB goes into: R15, R13, R9, R10, RBX
 *
 * Product table (indices into a[] and precomputed values):
 *   c0: a0*a0,     a1*4a8,   a2*4a7,   a3*4a6,   a4*4a5
 *   c1: a0*2a1,    a2*4a8,   a3*4a7,   a4*4a6,   a5*2a5
 *   c2: a0*2a2,    a1*a1,    a3*4a8,   a4*4a7,   a5*4a6
 *   c3: a0*2a3,    a1*2a2,   a4*4a8,   a5*4a7,   a6*2a6
 *   c4: a0*2a4,    a1*2a3,   a2*a2,    a5*4a8,   a6*4a7
 *   c5: a0*2a5,    a1*2a4,   a2*2a3,   a6*4a8,   a7*2a7
 *   c6: a0*2a6,    a1*2a5,   a2*2a4,   a3*a3,    a7*4a8
 *   c7: a0*2a7,    a1*2a6,   a2*2a5,   a3*2a4,   a8*2a8
 *   c8: a0*2a8,    a1*2a7,   a2*2a6,   a3*2a5,   a4*a4
 */

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

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    /* Precompute doubled and quadrupled values */
    uint64_t a0=a[0], a1=a[1], a2=a[2], a3=a[3], a4=a[4];
    uint64_t a5=a[5], a6=a[6], a7=a[7], a8=a[8];
    uint64_t d1=2*a1, d2=2*a2, d3=2*a3, d4=2*a4, d5=2*a5;
    uint64_t d6=2*a6, d7=2*a7, d8=2*a8;
    uint64_t q5=4*a5, q6=4*a6, q7=4*a7, q8=4*a8;

    /* srcA and srcB for each limb k, interleaved: sa0,sb0,...,sa4,sb4 */
    uint64_t tab[9][10] = {
      /* c0 */ {a0,a0,  a1,q8,  a2,q7,  a3,q6,  a4,q5},
      /* c1 */ {a0,d1,  a2,q8,  a3,q7,  a4,q6,  a5,d5},
      /* c2 */ {a0,d2,  a1,a1,  a3,q8,  a4,q7,  a5,q6},
      /* c3 */ {a0,d3,  a1,d2,  a4,q8,  a5,q7,  a6,d6},
      /* c4 */ {a0,d4,  a1,d3,  a2,a2,  a5,q8,  a6,q7},
      /* c5 */ {a0,d5,  a1,d4,  a2,d3,  a6,q8,  a7,d7},
      /* c6 */ {a0,d6,  a1,d5,  a2,d4,  a3,a3,  a7,q8},
      /* c7 */ {a0,d7,  a1,d6,  a2,d5,  a3,d4,  a8,d8},
      /* c8 */ {a0,d8,  a1,d7,  a2,d6,  a3,d5,  a4,a4},
    };

    uint64_t carry = 0;

    for (int k = 0; k < 9; k++) {
        uint64_t limb, cry;
        run_5mac(tab[k][0], tab[k][1], tab[k][2], tab[k][3],
                 tab[k][4], tab[k][5], tab[k][6], tab[k][7],
                 tab[k][8], tab[k][9], carry, &limb, &cry);
        out[k] = limb;
        carry = cry;
    }

    /* Fix c8: patch used 58-bit extraction, need 57-bit for top limb */
    uint64_t extra = out[8] >> 57;
    out[8] &= MASK57;
    uint64_t wrap_carry = carry * 2 + extra;

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
    uint64_t    input[9];
    uint64_t    expected[9];
    int         has_expected;
} test_vec_t;

static const test_vec_t test_vectors[] = {
    { "zero",
      {0,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0}, 1 },
    { "one",
      {1,0,0,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0,0}, 1 },
    { "nine",
      {9,0,0,0,0,0,0,0,0}, {81,0,0,0,0,0,0,0,0}, 1 },
    { "2^58",
      {0,1,0,0,0,0,0,0,0}, {0,0,1,0,0,0,0,0,0}, 1 },
    { "2^116",
      {0,0,1,0,0,0,0,0,0}, {0,0,0,0,1,0,0,0,0}, 1 },
    { "2^232",
      {0,0,0,0,1,0,0,0,0}, {0,0,0,0,0,0,0,0,1}, 1 },
    /* 2^464 squared = 2^928. 928=521+407. 2^521 ≡ 1 mod p → 2^928 = 2^407.
       407 = 7*58+1 → limb7 = 2^1 = 2. */
    { "2^464",
      {0,0,0,0,0,0,0,0,1}, {0,0,0,0,0,0,0,2,0}, 1 },
    /* 2^261 squared = 2^522 = 2 mod p */
    { "2^261",
      {0,0,0,0,UINT64_C(1)<<29,0,0,0,0}, {2,0,0,0,0,0,0,0,0}, 1 },
    { "all_ones",
      {1,1,1,1,1,1,1,1,1}, {0}, 0 },
    { "all_max58",
      {MASK58,MASK58,MASK58,MASK58,MASK58,MASK58,MASK58,MASK58,MASK57}, {0}, 0 },
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[9], nat[9], ucd[9];
    fe_sq_reference(t->input, ref);
    fe_sq_native(t->input, nat);
    fe_sq_ucode(t->input, ucd);

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

static int verify_random_quiet(const uint64_t a[9]) {
    uint64_t ref[9], nat[9], ucd[9];
    fe_sq_reference(a, ref);
    fe_sq_native(a, nat);
    fe_sq_ucode(a, ucd);
    if (memcmp(ref, nat, 72) != 0 || memcmp(ref, ucd, 72) != 0) {
        printf("  FAIL random: a={");
        for (int i = 0; i < 9; i++) printf("%s%016" PRIx64, i?",":"", a[i]);
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
        uint64_t a[9];
        for (int j = 0; j < 8; j++)
            a[j] = splitmix64(&rng) & MASK58;
        a[8] = splitmix64(&rng) & MASK57;
        if (verify_random_quiet(a)) rpass++;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    printf("\n--- Iterated chain (%d sq from (1,0,...,0)) ---\n", CHAIN_ITERS);
    uint64_t ri[9] = {1,0,0,0,0,0,0,0,0};
    uint64_t ni[9], ui[9];
    memcpy(ni, ri, 72); memcpy(ui, ri, 72);
    for (int i = 0; i < CHAIN_ITERS; i++) {
        fe_sq_reference(ri, ri);
        fe_sq_native(ni, ni);
        fe_sq_ucode(ui, ui);
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

    printf("=== P-521 field squaring: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_p521_sq_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    uint64_t state[9] = {
        0x00062D608F25D51AULL & MASK58, 0x000412A4B4F6592AULL & MASK58,
        0x00075B7171A4B31DULL & MASK58, 0x0001FF60527118FEULL & MASK58,
        0x000216936D3CD6E5ULL & MASK58, 0x0003B0A65E59EC35ULL & MASK58,
        0x00025EA3B4488A68ULL & MASK58, 0x0001DB1232A6754AULL & MASK58,
        0x0000E5B7C53A1B5EULL & MASK57,
    };

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t tmp[9];

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_native(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Native -O3:  min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_ucode(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
