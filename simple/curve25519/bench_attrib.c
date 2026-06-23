/*
 * bench_attrib.c — attribute the speed-up of ours/ucode to its two tricks.
 *
 * Includes full_curve25519_inline2.c as a library (INLINE2_LIB strips its
 * mains/contenders/profiler) to reuse the EXACT patches + the inline-asm field
 * macros + ladder_step / fe_invert. We then define "plain" variants per trick
 * DISABLED and diff the cycles:
 *
 *   Trick 1 — ladder register-chaining (FE_SQ_FROM_REGS / FE_MUL_FROM_REGS_A):
 *             a sq/mul reuses the add/sub result already in registers,
 *             skipping 5 input reloads. ladder_step_plain reloads from memory.
 *
 *   Trick 2 — invert squaring-chaining (INV_SQ_RENAME): the 254 squarings of
 *             the inversion stay in registers between sqs. fe_invert_plain
 *             stores+reloads every squaring.
 *
 * Build: make PROG=bench_attrib    Run: sudo taskset -c 0 ./bench_attrib_static
 */
#define _GNU_SOURCE
#define INLINE2_LIB
#include "full_curve25519_inline2.c"

/* ── Trick 1 OFF: ladder step with every chained op reloading from memory ── */
static void ladder_step_plain(ladder_state_t *st) {
    register ladder_state_t *_st asm("rbp") = st;
    asm volatile(
        FE_ADD(A_OFF, X2_OFF, Z2_OFF)   FE_SQ(AA_OFF, A_OFF)
        FE_SUB(B_OFF, X2_OFF, Z2_OFF)   FE_SQ(BB_OFF, B_OFF)
        FE_SUB(E_OFF, AA_OFF, BB_OFF)
        FE_SUB(D_OFF, X3_OFF, Z3_OFF)   FE_MUL(DA_OFF, D_OFF, A_OFF)
        FE_ADD(C_OFF, X3_OFF, Z3_OFF)   FE_MUL(CB_OFF, C_OFF, B_OFF)
        FE_ADD(T0_OFF, DA_OFF, CB_OFF)  FE_SQ(X3_OFF, T0_OFF)
        FE_SUB(T0_OFF, DA_OFF, CB_OFF)  FE_SQ(Z3_OFF, T0_OFF)
        FE_MUL(Z3_OFF, X1_OFF, Z3_OFF)
        FE_MUL(X2_OFF, AA_OFF, BB_OFF)
        : : "r"(_st)
        : "rax","rbx","rcx","rdx","rsi","rdi",
          "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"
    );
    fe_mul121665_native(st->t0, st->E);
    register ladder_state_t *_st2 asm("rbp") = st;
    asm volatile(
        FE_ADD(T0_OFF, AA_OFF, T0_OFF)
        FE_MUL(Z2_OFF, E_OFF, T0_OFF)
        : : "r"(_st2)
        : "rax","rbx","rcx","rdx","rsi","rdi",
          "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"
    );
}

/* ── Trick 2 OFF: invert with every squaring stored+reloaded (no INV chain) ── */
static void fe_invert_plain(uint64_t out[5], const uint64_t z[5]) {
    invert_state_t st;
    st.z[0]=z[0]; st.z[1]=z[1]; st.z[2]=z[2]; st.z[3]=z[3]; st.z[4]=z[4];
    register invert_state_t *_st asm("rbp") = &st;
    asm volatile(
        INV_SQ(IZ2_OFF, IZ_OFF)                                   /* z2 = z^2      */
        INV_SQ(IT_OFF, IZ2_OFF)  INV_SQ(IT_OFF, IT_OFF)           /* t  = z2^(2^2) */
        INV_MUL(IZ9_OFF, IT_OFF, IZ_OFF)                          /* z9 = t*z      */
        INV_MUL(IZ11_OFF, IZ9_OFF, IZ2_OFF)                       /* z11= z9*z2    */
        INV_SQ(IT_OFF, IZ11_OFF)                                  /* t  = z11^2    */
        INV_MUL(IT0_OFF, IT_OFF, IZ9_OFF)                         /* t0 = t*z9     */
        INV_SQ(IT1_OFF, IT0_OFF) ".rept 4\n\t"  INV_SQ(IT1_OFF, IT1_OFF) ".endr\n\t"   /* t1 = t0^(2^5)   */
        INV_MUL(IT1_OFF, IT1_OFF, IT0_OFF)
        INV_SQ(IT2_OFF, IT1_OFF) ".rept 9\n\t"  INV_SQ(IT2_OFF, IT2_OFF) ".endr\n\t"   /* t2 = t1^(2^10)  */
        INV_MUL(IT2_OFF, IT2_OFF, IT1_OFF)
        INV_SQ(IT3_OFF, IT2_OFF) ".rept 19\n\t" INV_SQ(IT3_OFF, IT3_OFF) ".endr\n\t"   /* t3 = t2^(2^20)  */
        INV_MUL(IT3_OFF, IT3_OFF, IT2_OFF)
        INV_SQ(IT3_OFF, IT3_OFF) ".rept 9\n\t"  INV_SQ(IT3_OFF, IT3_OFF) ".endr\n\t"   /* t3 = t3^(2^10)  */
        INV_MUL(IT1_OFF, IT3_OFF, IT1_OFF)
        INV_SQ(IT2_OFF, IT1_OFF) ".rept 49\n\t" INV_SQ(IT2_OFF, IT2_OFF) ".endr\n\t"   /* t2 = t1^(2^50)  */
        INV_MUL(IT2_OFF, IT2_OFF, IT1_OFF)
        INV_SQ(IT3_OFF, IT2_OFF) ".rept 99\n\t" INV_SQ(IT3_OFF, IT3_OFF) ".endr\n\t"   /* t3 = t2^(2^100) */
        INV_MUL(IT3_OFF, IT3_OFF, IT2_OFF)
        INV_SQ(IT3_OFF, IT3_OFF) ".rept 49\n\t" INV_SQ(IT3_OFF, IT3_OFF) ".endr\n\t"   /* t3 = t3^(2^50)  */
        INV_MUL(IT1_OFF, IT3_OFF, IT1_OFF)
        INV_SQ(IT1_OFF, IT1_OFF) ".rept 4\n\t"  INV_SQ(IT1_OFF, IT1_OFF) ".endr\n\t"   /* t1 = t1^(2^5)   */
        INV_MUL(IT1_OFF, IT1_OFF, IZ11_OFF)                       /* out = t1*z11  */
        : : "r"(_st)
        : "rax","rbx","rcx","rdx","rsi","rdi",
          "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"
    );
    out[0]=st.t1[0]; out[1]=st.t1[1]; out[2]=st.t1[2]; out[3]=st.t1[3]; out[4]=st.t1[4];
}

/* Full X25519 using the plain (un-chained) ladder + invert. */
static void x25519_plain(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t e[32]; memcpy(e, scalar, 32); scalar_clamp(e);
    ladder_state_t st;
    fe_frombytes(st.x1, point);
    memcpy(st.x2, (const uint64_t[]){1,0,0,0,0}, 40);
    memset(st.z2, 0, 40);
    memcpy(st.x3, st.x1, 40);
    memcpy(st.z3, (const uint64_t[]){1,0,0,0,0}, 40);
    uint64_t swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (e[pos>>3] >> (pos&7)) & 1;
        swap ^= bit; fe_cswap(st.x2, st.x3, swap); fe_cswap(st.z2, st.z3, swap); swap = bit;
        ladder_step_plain(&st);
    }
    fe_cswap(st.x2, st.x3, swap); fe_cswap(st.z2, st.z3, swap);
    fe_invert_plain(st.z2, st.z2);
    register ladder_state_t *_st asm("rbp") = &st;
    asm volatile(FE_MUL(X2_OFF, X2_OFF, Z2_OFF) : : "r"(_st)
        : "rax","rbx","rcx","rdx","rsi","rdi",
          "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc");
    fe_tobytes(out, st.x2);
}

/* ── timing: min over TRIALS of (rdtsc around ITERS calls)/ITERS ── */
#define TIME_FN(LABEL, CALL, ITERS, TRIALS) do {                 \
    uint64_t _best = ~0ULL;                                      \
    for (int _t = 0; _t < (TRIALS); _t++) {                      \
        uint64_t _a = rdtsc_start();                             \
        for (int _i = 0; _i < (ITERS); _i++) { CALL; }           \
        uint64_t _c = rdtsc_end() - _a;                          \
        if (_c < _best) _best = _c;                              \
    }                                                            \
    printf("  %-30s %10.1f cyc\n", LABEL, (double)_best/(ITERS));\
} while (0)

static ladder_state_t g_st;
static void init_state(void) {
    uint64_t *p = (uint64_t *)&g_st;
    for (size_t i = 0; i < sizeof(g_st)/8; i++)
        p[i] = (0x123456789ABCDULL * (i + 1)) & MASK51;
}

int main(void) {
    printf("=== ours/ucode trick attribution ===\n\n");
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();
    printf("\n");

    /* Correctness: chained and plain must agree, and match RFC 7748 TV1. */
    uint8_t sc[32], pt[32], r_chained[32], r_plain[32];
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", sc, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", pt, 32);
    x25519(r_chained, sc, pt);
    x25519_plain(r_plain, sc, pt);
    int ok_kat   = (memcmp_hex(r_chained, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32) == 0);
    int ok_match = (memcmp(r_chained, r_plain, 32) == 0);
    printf("correctness: chained matches RFC TV1: %s ;  plain == chained: %s\n\n",
           ok_kat ? "PASS" : "FAIL", ok_match ? "PASS" : "FAIL");
    if (!ok_kat || !ok_match) {
        printf("ABORT: variants disagree, attribution would be meaningless.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    /* ── per-component cost: chained vs plain ── */
    printf("-- ladder_step (min of 50 trials, 2000 iters) --\n");
    init_state(); TIME_FN("ladder_step (chained)", ladder_step(&g_st),       2000, 50);
    init_state(); TIME_FN("ladder_step (plain)",   ladder_step_plain(&g_st), 2000, 50);
    printf("\n-- fe_invert (min of 50 trials, 200 iters) --\n");
    init_state(); TIME_FN("fe_invert (chained)", fe_invert(g_st.z2, g_st.z2),       200, 50);
    init_state(); TIME_FN("fe_invert (plain)",   fe_invert_plain(g_st.z2, g_st.z2), 200, 50);
    printf("\n-- full x25519 (min of 200 trials) --\n");
    TIME_FN("x25519 (chained)", x25519(r_chained, sc, pt), 1, 200);
    TIME_FN("x25519 (plain)",   x25519_plain(r_plain, sc, pt), 1, 200);

    printf("\n(Attribution: ladder saving per X25519 = (plain-chained)_ladder x 255;\n"
           " invert saving = (plain-chained)_invert; total = x25519 plain-chained.)\n");

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
