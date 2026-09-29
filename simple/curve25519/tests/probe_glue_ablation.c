/*
 * probe_glue_ablation.c — how much does the native glue between firings cost
 * inside the real ladder step?
 *
 * Question: would moving the ladder's native adds/subs and mul121665 into the
 * microcode firings (fall-through prologue entries) be worth building? That
 * can only save what the glue costs IN SITU, and the old "add->sq fusion is a
 * no-go" verdict was measured when fe_sq still carried the ~30 cyc load-order
 * stall. This probe measures the ceiling directly, with no new microcode:
 * every variant keeps the production patches and the same 9 firings with the
 * same operand loads/stores, and removes only glue.
 *
 *   V0 full        production ladder_step (unchanged)
 *   V1 -m121665    V0 without fe_mul121665_native
 *   V2 -addsub     every FE_ADD/FE_SUB replaced by 5 loads of its first
 *                  operand into the same registers (so FROM_REGS consumers
 *                  still see a value); mul121665 kept
 *   V3 -glue       V2 without mul121665: firings + their own I/O only
 *   V4 -glue+ld    V3, but each removed add/sub keeps BOTH operands' 10 loads
 *                  -- what a fused prologue would still have to load
 *
 * V0-V3 is the upper bound on what fusion can save per step; V0-V4 is the
 * more realistic bound (the fused firing still loads both operands) before
 * charging anything for the prologue triads themselves. x255 per X25519.
 * Target to beat OpenSSL: ~13k per X25519 (~51 cyc/step).
 *
 * Results are WRONG by design in V1-V4 (limbs stay reduced sq/mul outputs, so
 * the patches' input bounds still hold). Only V0 is correctness-checked, via
 * RFC 7748 vector 1 before and after timing.
 *
 * Build: make PROG=tests/probe_glue_ablation
 * Run:   sudo taskset -c 0 ./tests/probe_glue_ablation_static
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"

/* 5 ascending loads into the add/sub result registers (keeps the
 * store-to-load ordering rule from FE_SQ's comment). */
#define LD5(a) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  \
    "mov rsi, [rbp + " S(a) " + 8]\n\t"  \
    "mov r12, [rbp + " S(a) " + 16]\n\t" \
    "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t"
#define LD10(a, b) LD5(b) LD5(a)

#define GCLOB "rax","rbx","rcx","rdx","rsi","rdi", \
              "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"

/* Ladder body with the glue swapped for G(a,b); same firing sequence as
 * ladder_step. FE_SUB(E) has no firing consumer in-block, so it is G too. */
#define LADDER_BODY(G)                          \
        G(X2_OFF, Z2_OFF)                       \
        FE_SQ_FROM_REGS(AA_OFF)                 \
        G(X2_OFF, Z2_OFF)                       \
        FE_SQ_FROM_REGS(BB_OFF)                 \
        G(AA_OFF, BB_OFF)                       \
        G(X3_OFF, Z3_OFF)                       \
        FE_MUL_FROM_REGS_A(DA_OFF, A_OFF)       \
        G(X3_OFF, Z3_OFF)                       \
        FE_MUL_FROM_REGS_A(CB_OFF, B_OFF)       \
        G(DA_OFF, CB_OFF)                       \
        FE_SQ_FROM_REGS(X3_OFF)                 \
        G(DA_OFF, CB_OFF)                       \
        FE_SQ_FROM_REGS(Z3_OFF)                 \
        FE_MUL(Z3_OFF, X1_OFF, Z3_OFF)          \
        FE_MUL(X2_OFF, AA_OFF, BB_OFF)
#define G1(a, b) LD5(a)
#define G2(a, b) LD10(a, b)

static void step_no_m121665(ladder_state_t *st) {
    register ladder_state_t *_st asm("rbp") = st;
    asm volatile(
        FE_ADD(A_OFF, X2_OFF, Z2_OFF)
        FE_SQ_FROM_REGS(AA_OFF)
        FE_SUB(B_OFF, X2_OFF, Z2_OFF)
        FE_SQ_FROM_REGS(BB_OFF)
        FE_SUB(E_OFF, AA_OFF, BB_OFF)
        FE_SUB_NOSTORE(X3_OFF, Z3_OFF)
        FE_MUL_FROM_REGS_A(DA_OFF, A_OFF)
        FE_ADD_NOSTORE(X3_OFF, Z3_OFF)
        FE_MUL_FROM_REGS_A(CB_OFF, B_OFF)
        FE_ADD_NOSTORE(DA_OFF, CB_OFF)
        FE_SQ_FROM_REGS(X3_OFF)
        FE_SUB_NOSTORE(DA_OFF, CB_OFF)
        FE_SQ_FROM_REGS(Z3_OFF)
        FE_MUL(Z3_OFF, X1_OFF, Z3_OFF)
        FE_MUL(X2_OFF, AA_OFF, BB_OFF)
        : : "r"(_st) : GCLOB);
    register ladder_state_t *_st2 asm("rbp") = st;
    asm volatile(
        FE_ADD_NOSTORE(AA_OFF, T0_OFF)
        FE_MUL_FROM_REGS_A(Z2_OFF, E_OFF)
        : : "r"(_st2) : GCLOB);
}

#define STEP_GLUELESS(name, G, KEEP_M121665)                        \
static void name(ladder_state_t *st) {                              \
    register ladder_state_t *_st asm("rbp") = st;                   \
    asm volatile(LADDER_BODY(G) : : "r"(_st) : GCLOB);              \
    if (KEEP_M121665) fe_mul121665_native(st->t0, st->E);           \
    register ladder_state_t *_st2 asm("rbp") = st;                  \
    asm volatile(G(AA_OFF, T0_OFF) FE_MUL_FROM_REGS_A(Z2_OFF, E_OFF)\
                 : : "r"(_st2) : GCLOB);                            \
}
STEP_GLUELESS(step_no_addsub, G1, 1)
STEP_GLUELESS(step_no_glue,   G1, 0)
STEP_GLUELESS(step_no_glue_ld, G2, 0)

static ladder_state_t g_s;
static void state_init(void) {
    uint64_t *p = (uint64_t *)&g_s;
    for (size_t i = 0; i < sizeof(g_s) / 8; i++)
        p[i] = (0x123456789ABCDULL * (i + 1)) & MASK51;
}

#define ITERS  2000
#define TRIALS 50
static double time_step(void (*f)(ladder_state_t *)) {
    uint64_t best = ~0ULL;
    for (int t = 0; t < TRIALS; t++) {
        state_init();
        uint64_t a = rdtsc_start();
        for (int r = 0; r < ITERS; r++) f(&g_s);
        uint64_t c = rdtsc_end() - a;
        if (c < best) best = c;
    }
    return (double)best / ITERS;
}

static int rfc1(void) {
    uint8_t sc[32], pt[32], r[32];
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", sc, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", pt, 32);
    x25519(r, sc, pt);
    return !memcmp_hex(r, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32);
}

#define NV 5
#define ROUNDS 3
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== probe_glue_ablation: native glue cost inside ladder_step ===\n\n");
    if (freq_guard()) return 2;
    bench_pin();
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();
    if (!rfc1()) { printf("RFC 7748 vector 1 FAILED; abort\n"); init_match_and_patch(); do_fix_IN_patch(); return 1; }
    printf("RFC 7748 vector 1 OK\n\n");

    const char *name[NV] = { "V0 full (production)", "V1 -mul121665",
                             "V2 -add/sub", "V3 -glue (firings only)",
                             "V4 -glue, keep 10 loads" };
    void (*fn[NV])(ladder_state_t *) = { ladder_step, step_no_m121665,
                                         step_no_addsub, step_no_glue,
                                         step_no_glue_ld };
    double c[ROUNDS][NV];
    for (int r = 0; r < ROUNDS; r++)
        for (int v = 0; v < NV; v++) c[r][v] = time_step(fn[v]);

    printf("-- cyc per ladder_step (TSC, min of %d x %d steps), %d interleaved rounds --\n",
           TRIALS, ITERS, ROUNDS);
    printf("  %-26s", "variant");
    for (int r = 0; r < ROUNDS; r++) printf("  round%d", r);
    printf("     best  saved/step  x255/X25519\n");
    double b[NV];
    for (int v = 0; v < NV; v++) {
        b[v] = c[0][v];
        for (int r = 1; r < ROUNDS; r++) if (c[r][v] < b[v]) b[v] = c[r][v];
    }
    for (int v = 0; v < NV; v++) {
        printf("  %-26s", name[v]);
        for (int r = 0; r < ROUNDS; r++) printf(" %7.1f", c[r][v]);
        printf("  %7.1f", b[v]);
        if (v) printf("  %10.1f  %11.0f", b[0] - b[v], 255 * (b[0] - b[v]));
        printf("\n");
    }
    printf("\n  V0 drift across rounds: %+.2f%%\n",
           100.0 * (c[ROUNDS - 1][0] - c[0][0]) / c[0][0]);
    printf("  target to reach OpenSSL: ~13k/X25519 = ~51 cyc/step\n");
    printf("  ceiling (V0-V3) %.1f cyc/step; realistic (V0-V4) %.1f cyc/step, before prologue cost\n",
           b[0] - b[3], b[0] - b[4]);

    int ok = rfc1();
    printf("\nRFC 7748 vector 1 after timing: %s\n", ok ? "OK" : "FAIL");
    init_match_and_patch();
    do_fix_IN_patch();
    return ok ? 0 : 1;
}
