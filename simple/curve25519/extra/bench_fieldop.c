/*
 * bench_fieldop.c — attribute ours/ucode's win to the MICROCODE FIELD OP.
 *
 * Times one 5x51 field mul (and one sq) per backend in an IDENTICAL
 * dependent-chain harness (latency-bound, matching the ladder, where each
 * field op feeds the next). All operate memory-to-memory:
 *   - microcode : the fe_mul / fe_sq patches (FE_MUL / FE_SQ macros)
 *   - fiat      : fiat-crypto autogen C (the SUPERCOP-style reference baseline)
 *   - native    : hand-written C with __uint128_t
 *   - cryptopt  : CryptOpt Goldmont-tuned x86-64 asm
 * For the C backends a compiler barrier after each op forces the same
 * memory round-trip the microcode macro always pays, so the comparison is fair.
 *
 * The point: an X25519 is ~2560 dependent field ops and ~89% of its cycles
 * are in the ladder (firing-bound), so the per-op delta below is what drives
 * the end-to-end win. Per-op delta x ~2560 ≈ the field-op contribution.
 *
 * Build: make PROG=bench_fieldop   Run: sudo taskset -c 0 ./bench_fieldop_static
 */
#define _GNU_SOURCE
#define INLINE2_LIB
#include "full_curve25519_inline2.c"            /* microcode FE_MUL/FE_SQ + patches + rdtsc */

/* fiat-crypto field ops (autogen C). */
#include "../../curvesC/curve25519_mul.c"
#include "../../curvesC/curve25519_square.c"

/* CryptOpt-tuned asm (linked from cryptopt_{mul,sq}.o). */
extern void cryptopt_carry_mul(uint64_t out[5], const uint64_t a[5], const uint64_t b[5]);
extern void cryptopt_carry_square(uint64_t out[5], const uint64_t a[5]);

/* native __uint128_t field ops (copied from full_curve25519.c). */
static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t a0=a[0],a1=a[1],a2=a[2],a3=a[3],a4=a[4];
    uint64_t b0=b[0],b1=b[1],b2=b[2],b3=b[3],b4=b[4];
    uint64_t r1=19*b1,r2=19*b2,r3=19*b3,r4=19*b4;
    __uint128_t c0=(__uint128_t)a0*b0+(__uint128_t)a1*r4+(__uint128_t)a2*r3+(__uint128_t)a3*r2+(__uint128_t)a4*r1;
    __uint128_t c1=(__uint128_t)a0*b1+(__uint128_t)a1*b0+(__uint128_t)a2*r4+(__uint128_t)a3*r3+(__uint128_t)a4*r2;
    __uint128_t c2=(__uint128_t)a0*b2+(__uint128_t)a1*b1+(__uint128_t)a2*b0+(__uint128_t)a3*r4+(__uint128_t)a4*r3;
    __uint128_t c3=(__uint128_t)a0*b3+(__uint128_t)a1*b2+(__uint128_t)a2*b1+(__uint128_t)a3*b0+(__uint128_t)a4*r4;
    __uint128_t c4=(__uint128_t)a0*b4+(__uint128_t)a1*b3+(__uint128_t)a2*b2+(__uint128_t)a3*b1+(__uint128_t)a4*b0;
    uint64_t cy;
    cy=(uint64_t)(c0>>51); out[0]=(uint64_t)c0&MASK51; c1+=cy;
    cy=(uint64_t)(c1>>51); out[1]=(uint64_t)c1&MASK51; c2+=cy;
    cy=(uint64_t)(c2>>51); out[2]=(uint64_t)c2&MASK51; c3+=cy;
    cy=(uint64_t)(c3>>51); out[3]=(uint64_t)c3&MASK51; c4+=cy;
    cy=(uint64_t)(c4>>51); out[4]=(uint64_t)c4&MASK51;
    out[0]+=cy*19; cy=out[0]>>51; out[0]&=MASK51; out[1]+=cy;
}
static void fe_sq_native(const uint64_t *a, uint64_t *out) { fe_mul_native(a, a, out); }

#define P_REPS   2000
#define P_TRIALS 80
#define P_UNROLL 16
#define REP2(x)  x x
#define REP4(x)  REP2(x) REP2(x)
#define REP8(x)  REP4(x) REP4(x)
#define REP16(x) REP8(x) REP8(x)

static ladder_state_t g;       /* microcode operand store, addressed via rbp+offset */
static uint64_t ca[5], cb[5];  /* C-backend operands */

static void init_ops(void) {
    uint64_t *p = (uint64_t *)&g;
    for (size_t i = 0; i < sizeof(g)/8; i++) p[i] = (0x123456789ABCDULL * (i+1)) & MASK51;
    for (int i = 0; i < 5; i++) { ca[i] = (0x13579BDF02468ULL * (i+1)) & MASK51;
                                  cb[i] = (0x2468ACE013579ULL * (i+1)) & MASK51; }
}

/* microcode op chained P_UNROLL x per asm block (dependent). */
#define TIME_UCODE(LABEL, BODY) do {                                          \
    uint64_t best = ~0ULL;                                                    \
    for (int t = 0; t < P_TRIALS; t++) {                                      \
        register ladder_state_t *_st asm("rbp") = &g;                         \
        uint64_t a = rdtsc_start();                                           \
        for (int r = 0; r < P_REPS; r++)                                      \
            asm volatile(BODY : : "r"(_st)                                    \
                : "rax","rbx","rcx","rdx","rsi","rdi",                        \
                  "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"); \
        uint64_t c = rdtsc_end() - a;                                         \
        if (c < best) best = c;                                               \
    }                                                                         \
    printf("  %-22s %8.2f cyc/op\n", LABEL, (double)best/((double)P_REPS*P_UNROLL)); \
    last = (double)best/((double)P_REPS*P_UNROLL);                            \
} while (0)

/* C op chained dependently; barrier forces the memory round-trip. */
#define TIME_C(LABEL, CALL) do {                                              \
    uint64_t best = ~0ULL;                                                    \
    for (int t = 0; t < P_TRIALS; t++) {                                      \
        uint64_t a = rdtsc_start();                                           \
        for (int r = 0; r < P_REPS*P_UNROLL; r++) { CALL; asm volatile("":::"memory"); } \
        uint64_t c = rdtsc_end() - a;                                         \
        if (c < best) best = c;                                               \
    }                                                                         \
    printf("  %-22s %8.2f cyc/op", LABEL, (double)best/((double)P_REPS*P_UNROLL)); \
    printf("   (%.2fx microcode)\n", ((double)best/((double)P_REPS*P_UNROLL))/last); \
} while (0)

int main(void) {
    printf("=== Field-op cost: microcode vs C backends (dependent chain) ===\n\n");
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();
    printf("\n");

    double last = 0;   /* last microcode cyc/op, for the ratio column */
    init_ops();
    printf("-- MUL (min of %d trials, %d ops each) --\n", P_TRIALS, P_REPS*P_UNROLL);
    TIME_UCODE("microcode mul", REP16(FE_MUL(DA_OFF, DA_OFF, A_OFF)));
    TIME_C    ("fiat mul",     fiat_curve25519_carry_mul(ca, ca, cb));
    TIME_C    ("native mul",   fe_mul_native(ca, cb, ca));
    TIME_C    ("cryptopt mul", cryptopt_carry_mul(ca, ca, cb));

    init_ops();
    printf("\n-- SQ (min of %d trials, %d ops each) --\n", P_TRIALS, P_REPS*P_UNROLL);
    TIME_UCODE("microcode sq", REP16(FE_SQ(AA_OFF, AA_OFF)));
    TIME_C    ("fiat sq",     fiat_curve25519_carry_square(ca, ca));
    TIME_C    ("native sq",   fe_sq_native(ca, ca));
    TIME_C    ("cryptopt sq", cryptopt_carry_square(ca, ca));

    /* sink so the chains can't be optimised away */
    volatile uint64_t sink = ca[0]^ca[1]^ca[2]^ca[3]^ca[4];
    for (size_t i=0;i<sizeof(g)/8;i++) sink ^= ((uint64_t*)&g)[i];
    printf("\n(checksum %llu)\n", (unsigned long long)sink);

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
