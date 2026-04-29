/*
 * bench_sq.c — fe_sq via microcode (vmwrite) vs native C (-O3)
 *
 * Patch: 56 triads at U7c00, hooked on vmwrite (0x0cd8).
 * All 15 MACs + carry propagation + reduction in microcode.
 *
 * Register convention (caller → microcode):
 *   RDI=a0  RSI=a1  R12=a2  R11=a3  R14=a4
 *   R15=2*a0  R13=2*a1  R9=2*a2  R10=2*a3
 *   RBX=19*a4  RDX=19*a3
 *   RAX=0  R8=0
 *
 * Output: RDI=h0  R9=h1  R10=h2  RBX=h3  RAX=h4
 *
 * Build:  gcc -O3 -o bench_sq bench_sq.c -I../../include
 * Run:    sudo taskset -c 0 ./bench_sq
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

/* ── fiat-crypto reference (the REAL GCC baseline CryptOpt compares against) ── */
#include "../curvesC/curve25519_square.c"

static void fe_sq_fiat(const uint64_t *a, uint64_t *out) {
    fiat_curve25519_carry_square(out, a);
}

/* ── microcode patch ──────────────────────────────────────────── */

static void install_fe_sq_patch(void) {
    ucode_t patch[] = {

    /* ═══ LIMB c0 = a0*a0 + d1*r4 + d2*r3 ═══ */
    { ZEROEXT_DSZ64_DR(TMP0, RAX),
      MUL_DSZ64_DRR(RCX, RDI, RDI),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, RDI),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R13),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, R13),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RDX, R9),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R9),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(RDI, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ c0→c1 transition ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(R13, RSI),
      NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, R13),
      ADD_DSZ64_DRR(R9, R12, R12),
      NOP_SEQWORD },

    /* ═══ LIMB c1 = d0*a1 + r3*a3 + d2*r4 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, R13),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R9),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R9),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(R9, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ c1→c2 transition ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(RDX, R12),
      NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, RDX),
      ZEROEXT_DSZ64_DR(R13, RSI),
      NOP_SEQWORD },

    /* ═══ LIMB c2 = d0*a2 + a1*a1 + d3*r4 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RSI, R13),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, R13),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R10),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R10),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(R10, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ c2→c3 transition ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(RDX, R11),
      NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, RDX),
      ADD_DSZ64_DRR(R13, RSI, RSI),
      NOP_SEQWORD },

    /* ═══ LIMB c3 = d0*a3 + d1*a2 + r4*a4 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RSI, R12),
      ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { NOP,
      MUL_DSZ64_DRR(RCX, R13, RSI),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, RSI),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R14, RBX),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, RBX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(RBX, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ c3→c4 transition ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      NOP, NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, R14),
      NOP, NOP_SEQWORD },

    /* ═══ LIMB c4 = d0*a4 + d1*a3 + a2*a2 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, R14),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R13, R11),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, R11),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R12, R12),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R12),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(RAX, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ FINAL REDUCTION ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOP, NOP, NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      NOP, NOP, NOP_SEQWORD },
    { MUL_DSZ64_DIR(TMP1, 19, TMP0),
      NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(RDI, RDI, TMP0),
      NOP, NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, RDI, 51),
      NOP, NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP1, RDI, 13),
      ADD_DSZ64_DRR(R9, R9, TMP0),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(RDI, TMP1, 13),
      NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("fe_sq patch installed: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ── fe_sq via microcode (optimized register I/O) ─────────────── */

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    /*
     * Pin the two pointers to specific registers via register vars,
     * then exclude those from the clobber list. GCC sees two registers
     * it doesn't need to allocate, and the clobber list only claims
     * the ones we actually destroy.
     *
     * We pick RCX and R15 as pointer holders:
     *   - RCX is overwritten late (by vmwrite output + our stores)
     *   - R15 is overwritten after we read all limbs via RCX
     * So we read [in] through RCX first, push out (R15) to stack,
     * then destroy both freely.
     */
    register uint64_t *_in  asm("rcx") = (uint64_t *)a;
    register uint64_t *_out asm("r15") = out;

    asm volatile(
        /* save output pointer before we destroy r15 */
        "push r15\n\t"

        /* load 5 limbs directly from rcx (= input pointer) */
        "mov rdi, [rcx]\n\t"
        "mov rsi, [rcx + 8]\n\t"
        "mov r12, [rcx + 16]\n\t"
        "mov r11, [rcx + 24]\n\t"
        "mov r14, [rcx + 32]\n\t"

        /* precompute doubled and reduced values */
        "lea r15, [rdi + rdi]\n\t"
        "lea r13, [rsi + rsi]\n\t"
        "lea r9,  [r12 + r12]\n\t"
        "lea r10, [r11 + r11]\n\t"
        "imul rbx, r14, 19\n\t"
        "imul rdx, r11, 19\n\t"

        /* clear accumulators */
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        /* fire microcode */
        "vmwrite rcx, rdx\n\t"

        /* recover output pointer, store 5 result limbs */
        "pop rcx\n\t"
        "mov [rcx],      rdi\n\t"
        "mov [rcx + 8],  r9\n\t"
        "mov [rcx + 16], r10\n\t"
        "mov [rcx + 24], rbx\n\t"
        "mov [rcx + 32], rax\n\t"

        : "+r"(_in), "+r"(_out)
        :
        : "rax", "rbx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14",
          "memory", "cc"
    );
}

/* ── fe_sq native C (compiled with -O3) ───────────────────────── */

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    uint64_t a0=a[0], a1=a[1], a2=a[2], a3=a[3], a4=a[4];
    uint64_t d0=2*a0, d1=2*a1, d2=2*a2, d3=2*a3;
    uint64_t r3=19*a3, r4=19*a4;

    __uint128_t c0 = (__uint128_t)a0*a0 + (__uint128_t)d1*r4 + (__uint128_t)d2*r3;
    __uint128_t c1 = (__uint128_t)d0*a1 + (__uint128_t)r3*a3 + (__uint128_t)d2*r4;
    __uint128_t c2 = (__uint128_t)d0*a2 + (__uint128_t)a1*a1 + (__uint128_t)d3*r4;
    __uint128_t c3 = (__uint128_t)d0*a3 + (__uint128_t)d1*a2 + (__uint128_t)r4*a4;
    __uint128_t c4 = (__uint128_t)d0*a4 + (__uint128_t)d1*a3 + (__uint128_t)a2*a2;

    uint64_t carry;
    carry = (uint64_t)(c0>>51); out[0] = (uint64_t)c0 & MASK51;
    c1 += carry;
    carry = (uint64_t)(c1>>51); out[1] = (uint64_t)c1 & MASK51;
    c2 += carry;
    carry = (uint64_t)(c2>>51); out[2] = (uint64_t)c2 & MASK51;
    c3 += carry;
    carry = (uint64_t)(c3>>51); out[3] = (uint64_t)c3 & MASK51;
    c4 += carry;
    carry = (uint64_t)(c4>>51); out[4] = (uint64_t)c4 & MASK51;
    out[0] += carry * 19;
    carry = out[0] >> 51; out[0] &= MASK51;
    out[1] += carry;
}

/* ── independent reference (naive schoolbook, no optimisations) ── */

static void fe_sq_reference(const uint64_t *a, uint64_t *out) {
    __uint128_t t[9] = {0};
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++)
            t[i + j] += (__uint128_t)a[i] * a[j];
    /* 2^(51*5) = 2^255 ≡ 19 (mod p) */
    for (int i = 5; i <= 8; i++)
        t[i - 5] += t[i] * 19;
    __uint128_t carry;
    carry = t[0] >> 51; out[0] = (uint64_t)t[0] & MASK51;
    t[1] += carry;
    carry = t[1] >> 51; out[1] = (uint64_t)t[1] & MASK51;
    t[2] += carry;
    carry = t[2] >> 51; out[2] = (uint64_t)t[2] & MASK51;
    t[3] += carry;
    carry = t[3] >> 51; out[3] = (uint64_t)t[3] & MASK51;
    t[4] += carry;
    carry = t[4] >> 51; out[4] = (uint64_t)t[4] & MASK51;
    out[0] += (uint64_t)carry * 19;
    carry = out[0] >> 51; out[0] &= MASK51;
    out[1] += (uint64_t)carry;
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
    uint64_t    input[5];
    uint64_t    expected[5];
    int         has_expected;
} test_vec_t;

/*
 * Hand-verified expected outputs:
 *   0² = 0
 *   1² = 1
 *   9² = 81
 *   (2^51)²  = 2^102                   → limb 2
 *   (2^102)² = 2^204                   → limb 4
 *   (2^153)² = 2^306 ≡ 19·2^51 mod p  → limb 1 = 19
 *   (2^204)² = 2^408 ≡ 19·2^153 mod p → limb 3 = 19
 *   (1+2^51)² = 1 + 2·2^51 + 2^102    → {1, 2, 1, 0, 0}
 *   (2^51-1)² = 2^102 - 2^52 + 1      → {1, MASK51-1, 0, 0, 0}
 */
static const test_vec_t test_vectors[] = {
    { "zero",      {0,0,0,0,0}, {0,0,0,0,0}, 1 },
    { "one",       {1,0,0,0,0}, {1,0,0,0,0}, 1 },
    { "nine",      {9,0,0,0,0}, {81,0,0,0,0}, 1 },
    { "2^51",      {0,1,0,0,0}, {0,0,1,0,0}, 1 },
    { "2^102",     {0,0,1,0,0}, {0,0,0,0,1}, 1 },
    { "2^153",     {0,0,0,1,0}, {0,19,0,0,0}, 1 },
    { "2^204",     {0,0,0,0,1}, {0,0,0,19,0}, 1 },
    { "1+2^51",    {1,1,0,0,0}, {1,2,1,0,0}, 1 },
    { "max_limb0", {MASK51,0,0,0,0}, {1,MASK51-1,0,0,0}, 1 },
    { "all_ones",  {1,1,1,1,1}, {0}, 0 },
    { "all_max",   {MASK51,MASK51,MASK51,MASK51,MASK51}, {0}, 0 },
    { "basepoint", {0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                    0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                    0x000216936D3CD6E5ULL}, {0}, 0 },
};
#define N_VECS (sizeof test_vectors / sizeof test_vectors[0])

static int verify_one(const test_vec_t *t) {
    uint64_t ref[5], nat[5], ucd[5];
    fe_sq_reference(t->input, ref);
    fe_sq_native(t->input, nat);
    fe_sq_ucode(t->input, ucd);

    int ok = 1;

    if (memcmp(ref, nat, 40) != 0) {
        printf("  FAIL [%s] native != reference\n", t->label);
        for (int i = 0; i < 5; i++)
            printf("    [%d] native=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, nat[i], ref[i], nat[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (memcmp(ref, ucd, 40) != 0) {
        printf("  FAIL [%s] ucode != reference\n", t->label);
        for (int i = 0; i < 5; i++)
            printf("    [%d] ucode=%016" PRIx64 " ref=%016" PRIx64 " %s\n",
                   i, ucd[i], ref[i], ucd[i] == ref[i] ? "" : "***");
        ok = 0;
    }
    if (t->has_expected && memcmp(ref, t->expected, 40) != 0) {
        printf("  FAIL [%s] result != expected\n", t->label);
        for (int i = 0; i < 5; i++)
            printf("    [%d] got=%016" PRIx64 " exp=%016" PRIx64 " %s\n",
                   i, ref[i], t->expected[i],
                   ref[i] == t->expected[i] ? "" : "***");
        ok = 0;
    }
    for (int i = 0; i < 5; i++) {
        if (ucd[i] >> 52) {
            printf("  FAIL [%s] limb %d overflow: %016" PRIx64 "\n",
                   t->label, i, ucd[i]);
            ok = 0;
        }
    }
    if (ok) printf("  PASS [%s]\n", t->label);
    return ok;
}

static int verify_random_quiet(const uint64_t in[5]) {
    uint64_t ref[5], nat[5], ucd[5];
    fe_sq_reference(in, ref);
    fe_sq_native(in, nat);
    fe_sq_ucode(in, ucd);
    if (memcmp(ref, nat, 40) != 0 || memcmp(ref, ucd, 40) != 0) {
        printf("  FAIL random: in={%016" PRIx64 ",%016" PRIx64 ",%016" PRIx64
               ",%016" PRIx64 ",%016" PRIx64 "}\n",
               in[0], in[1], in[2], in[3], in[4]);
        if (memcmp(ref, nat, 40) != 0) {
            printf("    native mismatch:");
            for (int i = 0; i < 5; i++)
                printf(" %016" PRIx64, nat[i]);
            printf("\n");
        }
        if (memcmp(ref, ucd, 40) != 0) {
            printf("    ucode  mismatch:");
            for (int i = 0; i < 5; i++)
                printf(" %016" PRIx64, ucd[i]);
            printf("\n");
        }
        printf("    reference:      ");
        for (int i = 0; i < 5; i++) printf(" %016" PRIx64, ref[i]);
        printf("\n");
        return 0;
    }
    return 1;
}

#define RANDOM_TESTS 10000
#define CHAIN_ITERS  1000

static int verify_all(void) {
    int pass = 0, fail = 0;

    /* ── known test vectors ──────────────────────────────────── */
    printf("--- Known test vectors ---\n");
    for (int i = 0; i < (int)N_VECS; i++) {
        if (verify_one(&test_vectors[i])) pass++; else fail++;
    }

    /* ── random stress test ──────────────────────────────────── */
    printf("\n--- Random stress test (%d vectors) ---\n", RANDOM_TESTS);
    uint64_t rng = 0xDEADBEEFCAFE1234ULL;
    int rpass = 0;
    for (int i = 0; i < RANDOM_TESTS; i++) {
        uint64_t in[5];
        for (int j = 0; j < 5; j++)
            in[j] = splitmix64(&rng) & MASK51;
        if (verify_random_quiet(in)) rpass++;
    }
    printf("  %d / %d PASS\n", rpass, RANDOM_TESTS);
    if (rpass < RANDOM_TESTS) fail += (RANDOM_TESTS - rpass);
    pass += rpass;

    /* ── iterated chain: ref vs native vs ucode ──────────────── */
    printf("\n--- Iterated chain (%d sq from basepoint) ---\n", CHAIN_ITERS);
    uint64_t bp[5] = { 0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                        0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                        0x000216936D3CD6E5ULL };
    uint64_t ri[5], ni[5], ui[5];
    memcpy(ri, bp, 40); memcpy(ni, bp, 40); memcpy(ui, bp, 40);
    for (int i = 0; i < CHAIN_ITERS; i++) {
        fe_sq_reference(ri, ri);
        fe_sq_native(ni, ni);
        fe_sq_ucode(ui, ui);
    }
    int ref_nat = memcmp(ri, ni, 40) == 0;
    int ref_ucd = memcmp(ri, ui, 40) == 0;
    printf("  ref==native: %s   ref==ucode: %s   → %s\n",
           ref_nat ? "yes" : "NO", ref_ucd ? "yes" : "NO",
           (ref_nat && ref_ucd) ? "PASS" : "FAIL");
    if (ref_nat && ref_ucd) {
        pass++;
    } else {
        fail++;
        printf("  reference:");
        for (int i = 0; i < 5; i++) printf(" %016" PRIx64, ri[i]);
        printf("\n  native:   ");
        for (int i = 0; i < 5; i++) printf(" %016" PRIx64, ni[i]);
        printf("\n  ucode:    ");
        for (int i = 0; i < 5; i++) printf(" %016" PRIx64, ui[i]);
        printf("\n");
    }

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

    printf("=== fe_sq: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_fe_sq_patch();

    /* ── correctness ──────────────────────────────────────────── */
    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED (%d errors), skipping benchmark.\n", failures);
        init_match_and_patch();
        do_fix_IN_patch();
        return 1;
    }

    uint64_t state[5] = { 0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                          0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                          0x000216936D3CD6E5ULL };

    /* ── benchmark ────────────────────────────────────────────── */
    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t tmp[5];

    /* native C -O3 */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_native(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Naive -O3:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* fiat-crypto (the real GCC baseline CryptOpt compares against) */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_fiat(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Fiat-crypto: min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* microcode */
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

    /* clean up */
    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
