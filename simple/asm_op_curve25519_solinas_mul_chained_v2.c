/*
 * asm_op_curve25519_solinas_mul_chained_v2.c — Solinas mul, chained ADC, packed.
 *
 * v2 = chained_v1 + two safe packing wins:
 *   (A) 4 MULs packed into 5 triads (was 8) by interleaving save_lo / save_hi
 *       / RDX reload into MUL triads' slots 1-2 (uses confirmed slot-1/2 MUL
 *       placement + RAW 0->1, 1->2, WAR 0->2 hazards).
 *   (B) writeback+SHIFT merged into 2 triads (was 4) — chain results in TMPs
 *       move directly to their shifted arch positions (e.g. new acc[0] =
 *       chain TMP1, since SHIFT advances arch index by one).
 *
 * Both depend on already-confirmed mechanics in CLAUDE.md (mul_slot2.c,
 * raw_war_waw.c). No new flag-bridging assumptions.
 *
 * Build:  make PROG=asm_op_curve25519_solinas_mul_chained_v2
 * Run:    sudo taskset -c 0 ./asm_op_curve25519_solinas_mul_chained_v2_static
 *
 * Triad budget compared to v1 (113 triads):
 *   PREP:                              3   (unchanged)
 *   row 0,1,2:  START + MUL(5) + COL(4) + ACC(6) + SHIFT/WB(2) =  18 each (= 54)
 *   row 3:      START + MUL(5) + COL(4) + ACC(6) + WB(2)       =  18
 *   reduction:                        20   (unchanged)
 *   ------------------------------------------------
 *   total:                            95
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* p = 2^255 - 19 */
static const uint64_t CURVE25519_P[4] = {
    UINT64_C(0xFFFFFFFFFFFFFFED), UINT64_C(0xFFFFFFFFFFFFFFFF),
    UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x7FFFFFFFFFFFFFFF)
};

#include "../curvesC/curve25519_solinas_mul.c"

static void fe_mul_fiat(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    fiat_curve25519_solinas_mul(out, a, b);
}

/* native reference (unchanged from v1) */
static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    __uint128_t t;
    uint64_t prod[8] = {0};
    for (int i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 4; j++) {
            t = (__uint128_t)a[i] * b[j] + prod[i+j] + carry;
            prod[i+j] = (uint64_t)t;
            carry = (uint64_t)(t >> 64);
        }
        prod[i+4] += carry;
    }
    uint64_t r[4]; uint64_t c = 0;
    for (int i = 0; i < 4; i++) {
        t = (__uint128_t)38 * prod[i+4] + prod[i] + c;
        r[i] = (uint64_t)t;
        c = (uint64_t)(t >> 64);
    }
    t = (__uint128_t)38 * c + r[0];
    r[0] = (uint64_t)t;
    c = (uint64_t)(t >> 64);
    r[1] += c; c = (r[1] < c);
    r[2] += c; c = (r[2] < c);
    r[3] += c;
    uint64_t diff[4]; __uint128_t b128;
    b128 = (__uint128_t)r[0] - CURVE25519_P[0];                                  diff[0] = (uint64_t)b128;
    b128 = (__uint128_t)r[1] - CURVE25519_P[1] - ((uint64_t)(b128 >> 64) & 1);   diff[1] = (uint64_t)b128;
    b128 = (__uint128_t)r[2] - CURVE25519_P[2] - ((uint64_t)(b128 >> 64) & 1);   diff[2] = (uint64_t)b128;
    b128 = (__uint128_t)r[3] - CURVE25519_P[3] - ((uint64_t)(b128 >> 64) & 1);   diff[3] = (uint64_t)b128;
    uint64_t borrow = (uint64_t)(b128 >> 64) & 1;
    uint64_t mask = (uint64_t)0 - borrow;
    out[0] = (r[0] & mask) | (diff[0] & ~mask);
    out[1] = (r[1] & mask) | (diff[1] & ~mask);
    out[2] = (r[2] & mask) | (diff[2] & ~mask);
    out[3] = (r[3] & mask) | (diff[3] & ~mask);
}

/* SCHOOLBOOK_ROW_START — load RDI and RDX with a_src (unchanged) */
#define SCHOOLBOOK_ROW_START(a_src) \
    { ZEROEXT_DSZ64_DR(RDX, a_src), ZEROEXT_DSZ64_DR(RDI, a_src), \
      NOP, NOP_SEQWORD }

/*
 * SCHOOLBOOK_ROW_PACKED — 5-triad MUL block + 4-triad col combine + 6-triad acc add.
 *
 * MUL block packing:
 *   T0: MUL_0(RCX,TMP10,RDX) ; save_lo_0(TMP0=RDX) ; save_hi_0(TMP1=RCX)
 *   T1: reload RDX=RDI       ; MUL_1(RCX,TMP11,RDX) ; save_lo_1(TMP2=RDX)
 *   T2: save_hi_1(TMP3=RCX)  ; reload RDX=RDI       ; MUL_2(RCX,TMP12,RDX)
 *   T3: save_lo_2(TMP4=RDX)  ; save_hi_2(TMP5=RCX)  ; reload RDX=RDI
 *   T4: MUL_3(RCX,TMP13,RDX) ; save_lo_3(TMP6=RDX)  ; save_hi_3(TMP7=RCX)
 *
 * Hazards (all already confirmed in CLAUDE.md):
 *   T0 slot 0->1 RAW (MUL writes RDX, slot 1 reads RDX)
 *   T0 slot 0->2 RAW (MUL writes RCX, slot 2 reads RCX)
 *   T1 slot 0->1 RAW (reload writes RDX, MUL reads RDX); MUL writes RDX, slot 2 reads RDX
 *   T2 slot 1->2 RAW (reload writes RDX, MUL reads RDX); slot 0 reads OLD RCX (T1's hi1)
 *   T3 slot 0 reads OLD RDX (T2's lo2), slot 2 writes NEW RDX (WAR 0->2)
 *   T4 slot 0->1 and 0->2 RAW (MUL writes RDX/RCX, slot 1/2 reads them)
 *
 * Inputs after T4: TMP0..TMP7 = lo0,hi0,lo1,hi1,lo2,hi2,lo3,hi3.
 *
 * Col combine and acc add are unchanged from v1.
 */
#define SCHOOLBOOK_ROW_PACKED \
    /* ─── 4 MULs in 5 triads ─── */ \
    /* T0: MUL_0, save_lo_0, save_hi_0 */ \
    { MUL_DSZ64_DRR(RCX, TMP10, RDX), \
      ZEROEXT_DSZ64_DR(TMP0, RDX), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    /* T1: reload RDX, MUL_1, save_lo_1 */ \
    { ZEROEXT_DSZ64_DR(RDX, RDI), \
      MUL_DSZ64_DRR(RCX, TMP11, RDX), \
      ZEROEXT_DSZ64_DR(TMP2, RDX), NOP_SEQWORD }, \
    /* T2: save_hi_1, reload RDX, MUL_2 */ \
    { ZEROEXT_DSZ64_DR(TMP3, RCX), \
      ZEROEXT_DSZ64_DR(RDX, RDI), \
      MUL_DSZ64_DRR(RCX, TMP12, RDX), NOP_SEQWORD }, \
    /* T3: save_lo_2, save_hi_2, reload RDX */ \
    { ZEROEXT_DSZ64_DR(TMP4, RDX), \
      ZEROEXT_DSZ64_DR(TMP5, RCX), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    /* T4: MUL_3, save_lo_3, save_hi_3 */ \
    { MUL_DSZ64_DRR(RCX, TMP13, RDX), \
      ZEROEXT_DSZ64_DR(TMP6, RDX), \
      ZEROEXT_DSZ64_DR(TMP7, RCX), NOP_SEQWORD }, \
    /* ─── form column values T0..T4 with chained carries ─── */ \
    /* T1 := hi0 + lo1, GFL */ \
    { ADD_DSZ64_DRR(TMP1, TMP1, TMP2), GENARITHFLAGS_RR(TMP1, TMP1), \
      NOP, NOP_SEQWORD }, \
    /* T2 := hi1 + lo2 + CF, GFL */ \
    { ADC_DSZ64_DRR(TMP3, TMP3, TMP4), GENARITHFLAGS_RR(TMP3, TMP3), \
      NOP, NOP_SEQWORD }, \
    /* T3 := hi2 + lo3 + CF, GFL */ \
    { ADC_DSZ64_DRR(TMP5, TMP5, TMP6), GENARITHFLAGS_RR(TMP5, TMP5), \
      NOP, NOP_SEQWORD }, \
    /* T4 := hi3 + CF */ \
    { ADC_DSZ64_DRR(TMP7, TMP7, TMP9), NOP, NOP, NOP_SEQWORD }, \
    /* ─── add T0..T4 to acc[0..4] with chained ADC ─── */ \
    { ADD_DSZ64_DRR(TMP0, R15, TMP0), GENARITHFLAGS_RR(TMP0, TMP0), \
      NOP, NOP_SEQWORD }, \
    { ADC_DSZ64_DRR(TMP1, R9, TMP1), GENARITHFLAGS_RR(TMP1, TMP1), \
      NOP, NOP_SEQWORD }, \
    { ADC_DSZ64_DRR(TMP3, R10, TMP3), GENARITHFLAGS_RR(TMP3, TMP3), \
      NOP, NOP_SEQWORD }, \
    { ADC_DSZ64_DRR(TMP5, R13, TMP5), GENARITHFLAGS_RR(TMP5, TMP5), \
      NOP, NOP_SEQWORD }, \
    { ADC_DSZ64_DRR(TMP7, RAX, TMP7), GENARITHFLAGS_RR(TMP7, TMP7), \
      NOP, NOP_SEQWORD }, \
    /* acc[5] overflow word — TMP14 = 0 + 0 + CF (fresh per row) */ \
    { ADC_DSZ64_DRR(TMP14, TMP9, TMP9), NOP, NOP, NOP_SEQWORD }

/*
 * SHIFT_WRITEBACK_MERGED — combined writeback + arch-reg shift for rows 0-2.
 *
 * After the chain, new acc[0..5] live in (TMP0,TMP1,TMP3,TMP5,TMP7,TMP14).
 * After the SHIFT the next row sees them as (new acc[0..4] in R15,R9,R10,R13,
 * RAX) with acc[0] saved to `save_reg`. Map TMP→arch directly:
 *
 *   save_reg ← TMP0   (was acc[0], stays as final p[row])
 *   R15      ← TMP1   (was acc[1], becomes new acc[0])
 *   R9       ← TMP3   (was acc[2], becomes new acc[1])
 *   R10      ← TMP5   (was acc[3], becomes new acc[2])
 *   R13      ← TMP7   (was acc[4], becomes new acc[3])
 *   RAX      ← TMP14  (was acc[5], becomes new acc[4])
 *
 * 6 ZEROEXTs in 2 triads.  TMP14 stays alive (will be overwritten by the
 * next row's chain ADC), so we don't need an explicit reset.
 */
#define SHIFT_WRITEBACK_MERGED(save_reg) \
    { ZEROEXT_DSZ64_DR(save_reg, TMP0), ZEROEXT_DSZ64_DR(R15, TMP1), \
      ZEROEXT_DSZ64_DR(R9, TMP3), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(R10, TMP5), ZEROEXT_DSZ64_DR(R13, TMP7), \
      ZEROEXT_DSZ64_DR(RAX, TMP14), NOP_SEQWORD }

/* ROW3_WRITEBACK — no shift, just write final p[3..7] into arch.
 * TMP14 still holds p[8] (= row 3's overflow) — used directly by reduction. */
#define ROW3_WRITEBACK \
    { ZEROEXT_DSZ64_DR(R15, TMP0), ZEROEXT_DSZ64_DR(R9, TMP1), \
      ZEROEXT_DSZ64_DR(R10, TMP3), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(R13, TMP5), ZEROEXT_DSZ64_DR(RAX, TMP7), \
      NOP, NOP_SEQWORD }

static void install_solinas_mul_chained_v2_patch(void) {
    ucode_t patch[] = {

    /* ─── PREP ─── */
    { ZEROEXT_DSZ64_DR(TMP10, RSI), ZEROEXT_DSZ64_DR(TMP11, R12),
      ZEROEXT_DSZ64_DR(TMP12, R11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R14), ZEROEXT_DSZ64_DR(R14, RDX),
      ZEROEXT_DSZ64_DR(TMP15, RBP), NOP_SEQWORD },
    /* TMP9 = 0 (chain "+0" source); TMP14 = 0 (acc[5] overflow start) */
    { ZEROEXT_DSZ32_DI(TMP9, 0), ZEROEXT_DSZ32_DI(TMP14, 0),
      NOP, NOP_SEQWORD },

    /* ─── ROW 0 ─── */
    SCHOOLBOOK_ROW_START(RDI),
    SCHOOLBOOK_ROW_PACKED,
    SHIFT_WRITEBACK_MERGED(RSI),

    /* ─── ROW 1 ─── */
    SCHOOLBOOK_ROW_START(R14),
    SCHOOLBOOK_ROW_PACKED,
    SHIFT_WRITEBACK_MERGED(R12),

    /* ─── ROW 2 ─── */
    SCHOOLBOOK_ROW_START(TMP15),
    SCHOOLBOOK_ROW_PACKED,
    SHIFT_WRITEBACK_MERGED(R11),

    /* ─── ROW 3 ─── */
    SCHOOLBOOK_ROW_START(RBX),
    SCHOOLBOOK_ROW_PACKED,
    ROW3_WRITEBACK,

    /*
     * Post-schoolbook state:
     *   R15=p[3], R9=p[4], R10=p[5], R13=p[6], RAX=p[7], TMP14=p[8]
     *   RSI=p[0], R12=p[1], R11=p[2]
     */

    /* ─── SOLINAS REDUCTION (unchanged from v1) ─── */
    /* Step 1: multiply p[4..7] by 38, save his to TMPs */
    { ADD_DSZ64_DRR(RAX, RAX, TMP14), MUL_DSZ64_DIR(RCX, 38, R9),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP0, RCX), MUL_DSZ64_DIR(RCX, 38, R10),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP1, RCX), MUL_DSZ64_DIR(RCX, 38, R13),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP2, RCX), MUL_DSZ64_DIR(RCX, 38, RAX),
      NOP, NOP_SEQWORD },

    /* Form 5-limb (38*p_upper) col 0..4 */
    { ADD_DSZ64_DRR(TMP0, TMP0, R10), GENARITHFLAGS_RR(TMP0, TMP0),
      NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP1, TMP1, R13), GENARITHFLAGS_RR(TMP1, TMP1),
      NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP2, TMP2, RAX), GENARITHFLAGS_RR(TMP2, TMP2),
      NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(RCX, RCX, TMP9), NOP, NOP, NOP_SEQWORD },

    /* r[0..3] = p[0..3] + col[0..3], chained through TMPs */
    { ADD_DSZ64_DRR(TMP3, RSI, R9), GENARITHFLAGS_RR(TMP3, TMP3),
      NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP4, R12, TMP0), GENARITHFLAGS_RR(TMP4, TMP4),
      NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP5, R11, TMP1), GENARITHFLAGS_RR(TMP5, TMP5),
      NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP6, R15, TMP2), GENARITHFLAGS_RR(TMP6, TMP6),
      NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP7, RCX, TMP9), NOP, NOP, NOP_SEQWORD },

    /* Final fold: 38*total_carry → r[0], chain */
    { MUL_DSZ64_DIR(RCX, 38, TMP7), NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP7), GENARITHFLAGS_RR(TMP3, TMP3),
      NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP4, TMP4, TMP9), GENARITHFLAGS_RR(TMP4, TMP4),
      NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP5, TMP5, TMP9), GENARITHFLAGS_RR(TMP5, TMP5),
      NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP6, TMP6, TMP9), NOP, NOP, NOP_SEQWORD },

    /* Writeback r[0..3] → arch */
    { ZEROEXT_DSZ64_DR(R15, TMP3), ZEROEXT_DSZ64_DR(R9, TMP4),
      ZEROEXT_DSZ64_DR(R10, TMP5), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R13, TMP6), NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("solinas_mul_chained_v2: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* fe_mul via microcode (wrapper unchanged from v1) */
static void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t r[4];
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_b   asm("rbx") = (uint64_t *)b;
    register uint64_t *_out asm("r15") = r;
    asm volatile(
        "push r15\n\t"
        "push rbp\n\t"
        "push rcx\n\t"
        "mov rsi, [rbx]\n\t"
        "mov r12, [rbx + 8]\n\t"
        "mov r11, [rbx + 16]\n\t"
        "mov r14, [rbx + 24]\n\t"
        "mov rdi, [rcx]\n\t"
        "mov rdx, [rcx + 8]\n\t"
        "mov rbp, [rcx + 16]\n\t"
        "mov rbx, [rcx + 24]\n\t"
        "mov r8, 38\n\t"
        "xor r15d, r15d\n\t"
        "xor r9d, r9d\n\t"
        "xor r10d, r10d\n\t"
        "xor r13d, r13d\n\t"
        "xor eax, eax\n\t"
        "vmwrite rcx, rdx\n\t"
        "pop rcx\n\t"
        "pop rbp\n\t"
        "pop rcx\n\t"
        "mov [rcx],      r15\n\t"
        "mov [rcx + 8],  r9\n\t"
        "mov [rcx + 16], r10\n\t"
        "mov [rcx + 24], r13\n\t"
        : "+r"(_a), "+r"(_b), "+r"(_out)
        :
        : "rax", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14",
          "memory", "cc"
    );
    uint64_t diff[4]; __uint128_t b128;
    b128 = (__uint128_t)r[0] - CURVE25519_P[0];                                  diff[0] = (uint64_t)b128;
    b128 = (__uint128_t)r[1] - CURVE25519_P[1] - ((uint64_t)(b128 >> 64) & 1);   diff[1] = (uint64_t)b128;
    b128 = (__uint128_t)r[2] - CURVE25519_P[2] - ((uint64_t)(b128 >> 64) & 1);   diff[2] = (uint64_t)b128;
    b128 = (__uint128_t)r[3] - CURVE25519_P[3] - ((uint64_t)(b128 >> 64) & 1);   diff[3] = (uint64_t)b128;
    uint64_t borrow = (uint64_t)(b128 >> 64) & 1;
    uint64_t mask = (uint64_t)0 - borrow;
    out[0] = (r[0] & mask) | (diff[0] & ~mask);
    out[1] = (r[1] & mask) | (diff[1] & ~mask);
    out[2] = (r[2] & mask) | (diff[2] & ~mask);
    out[3] = (r[3] & mask) | (diff[3] & ~mask);
}

/* verification (same as v1) */
static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void rand_mod_p(uint64_t out[4], uint64_t *rng) {
    for (;;) {
        for (int j = 0; j < 4; j++) out[j] = splitmix64(rng);
        out[3] &= 0x7FFFFFFFFFFFFFFFULL;
        int lt = 0;
        for (int j = 3; j >= 0; j--) {
            if (out[j] < CURVE25519_P[j]) { lt = 1; break; }
            if (out[j] > CURVE25519_P[j]) break;
        }
        if (lt) break;
    }
}

static int verify_all(void) {
    int pass = 0, fail = 0;
    printf("--- Known vectors ---\n");
    struct { const char *name; uint64_t a[4]; uint64_t b[4]; } vecs[] = {
        { "0*0",  {0, 0, 0, 0}, {0, 0, 0, 0} },
        { "1*1",  {1, 0, 0, 0}, {1, 0, 0, 0} },
        { "0*1",  {0, 0, 0, 0}, {1, 0, 0, 0} },
        { "2*3",  {2, 0, 0, 0}, {3, 0, 0, 0} },
        { "38*1", {38, 0, 0, 0}, {1, 0, 0, 0} },
        { "(p-1)*2",
          {UINT64_C(0xFFFFFFFFFFFFFFEC), UINT64_C(0xFFFFFFFFFFFFFFFF),
           UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x7FFFFFFFFFFFFFFF)},
          {2, 0, 0, 0} },
        { "big*big",
          {UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0xFFFFFFFFFFFFFFFF),
           UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x3FFFFFFFFFFFFFFF)},
          {UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0xFFFFFFFFFFFFFFFF),
           UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x3FFFFFFFFFFFFFFF)} },
    };
    int nvecs = sizeof(vecs) / sizeof(vecs[0]);
    for (int i = 0; i < nvecs; i++) {
        uint64_t nat[4], ucd[4];
        fe_mul_native(vecs[i].a, vecs[i].b, nat);
        fe_mul_ucode(vecs[i].a, vecs[i].b, ucd);
        int ok = !memcmp(nat, ucd, 32);
        if (!ok) {
            printf("  FAIL [%s]\n", vecs[i].name);
            for (int j = 0; j < 4; j++)
                printf("    [%d] nat=%016"PRIx64" ucd=%016"PRIx64"%s\n",
                    j, nat[j], ucd[j], nat[j] != ucd[j] ? " ***" : "");
        } else printf("  PASS [%s]\n", vecs[i].name);
        if (ok) pass++; else fail++;
    }

    printf("\n--- Random (10000) ---\n");
    uint64_t rng = 0xDEADBEEFCAFE1234ULL;
    int rp = 0;
    for (int i = 0; i < 10000; i++) {
        uint64_t a[4], b[4], nat[4], ucd[4];
        rand_mod_p(a, &rng); rand_mod_p(b, &rng);
        fe_mul_native(a, b, nat); fe_mul_ucode(a, b, ucd);
        if (!memcmp(nat, ucd, 32)) rp++;
        else {
            printf("  FAIL #%d\n", i);
            printf("    a={%016"PRIx64",%016"PRIx64",%016"PRIx64",%016"PRIx64"}\n",
                   a[0], a[1], a[2], a[3]);
            printf("    b={%016"PRIx64",%016"PRIx64",%016"PRIx64",%016"PRIx64"}\n",
                   b[0], b[1], b[2], b[3]);
            for (int j = 0; j < 4; j++)
                printf("    [%d] nat=%016"PRIx64" ucd=%016"PRIx64"%s\n",
                    j, nat[j], ucd[j], nat[j] != ucd[j] ? " ***" : "");
            break;
        }
    }
    printf("  %d / 10000 PASS\n", rp);
    pass += rp; if (rp < 10000) fail += (10000 - rp);

    printf("\n=== %d passed, %d failed ===\n\n", pass, fail);
    return fail;
}

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

#define BATCH 10000
#define REPS  200

int main(void) {
    printf("=== curve25519 Solinas mul CHAINED-ADC v2 (packed MUL+SHIFT): microcode vs native ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_solinas_mul_chained_v2_patch();

    int failures = verify_all();
    if (failures) {
        printf("Verification FAILED, skipping benchmark.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    uint64_t state_a[4] = {9, 0, 0, 0};
    uint64_t state_b[4] = {7, 0, 0, 0};
    uint64_t tmp_a[4], tmp_b[4], t0, t1, min, sum;

    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, 32);
        memcpy(tmp_b, state_b, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_native(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Naive -O3:   min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, 32);
        memcpy(tmp_b, state_b, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_fiat(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Fiat-crypto: min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp_a, state_a, 32);
        memcpy(tmp_b, state_b, 32);
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_ucode(tmp_a, tmp_b, tmp_a);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0; sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode chained v2: min/op %4"PRIu64"  avg/op %4"PRIu64" cycles\n",
           min/BATCH, sum/REPS/BATCH);

    init_match_and_patch(); do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
