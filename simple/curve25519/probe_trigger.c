/*
 * probe_trigger.c — is fe_sq's 30-cycle deficit the PATCH or the TRIGGER?
 *
 * probe_sq_lat found two dependency-carrying 1-triad floors that should have
 * been identical and were not:
 *     vmwrite hook (fe_mul path)   15.7 cyc
 *     vmread  hook (fe_sq  path)   45.6 cyc
 * Same 1-triad body, same ping-pong, both carrying a real input->output
 * dependency. The two arms differ in TWO ways at once, though: the trigger
 * instruction (vmwrite rcx,rdx vs .byte 0f 78 ca) AND the wrapper around it
 * (fe_mul marshals 10 limbs, fe_sq does 4 LEAs and 2 IMULs). This binary
 * separates them with a full 2x2 cross, then prices the obvious fix.
 *
 *   floor arms   {sq wrapper, mul wrapper} x {vmwrite, vmread}, 1-triad patch
 *   swap arms    the REAL fe_sq fired by vmwrite, the REAL fe_mul by vmread
 *   index arm    vmread hooked through match entry 7 instead of 1, to rule
 *                out the match/patch entry index as the variable
 *
 * Patch RAM layout (128 triads at U7c00, 4 address units per triad):
 *     U7c00  fe_mul   66 triads   .. U7d04
 *     U7d08  fe_sq    42 triads   .. U7dac
 *     U7db0  1-triad dependency probe, sq  flavour (ZEROEXT rdi,rdi)
 *     U7db4  1-triad dependency probe, mul flavour (ZEROEXT r15,rdi)
 * Only the HOOKS move; the patches stay put, so no arm can be explained by
 * a patch landing at a different address.
 *
 * Build: make PROG=probe_trigger
 * Run:   sudo taskset -c 0 ./probe_trigger_static
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"
#include <unistd.h>

#define K_INNER  8
#define K_REPS   200
#define K_PHASES 4
#define K_OPS    (K_INNER * 16)

#define REP2(x)  x x
#define REP4(x)  REP2(x) REP2(x)
#define REP8(x)  REP4(x) REP4(x)

#define TRIG_VMW "vmwrite rcx, rdx\n\t"
#define TRIG_VMR ".byte 0x0f, 0x78, 0xca\n\t"

/* The production wrappers, with the trigger instruction as a parameter. */
#define FE_MUL_T(out, a, b, TRIG) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  "mov rsi, [rbp + " S(a) " + 8]\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t" "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t" "mov r15, [rbp + " S(b) " + 0]\n\t" \
    "mov r13, [rbp + " S(b) " + 8]\n\t"  "mov r9,  [rbp + " S(b) " + 16]\n\t" \
    "mov r10, [rbp + " S(b) " + 24]\n\t" "mov rbx, [rbp + " S(b) " + 32]\n\t" \
    "xor eax, eax\n\t" "xor r8d, r8d\n\t" TRIG \
    "mov [rbp + " S(out) " + 0],  r15\n\t" "mov [rbp + " S(out) " + 8],  r13\n\t" \
    "mov [rbp + " S(out) " + 16], r9\n\t"  "mov [rbp + " S(out) " + 24], r10\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

#define FE_SQ_T(out, a, TRIG) \
    "mov r14, [rbp + " S(a) " + 32]\n\t" "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t" "mov rsi, [rbp + " S(a) " + 8]\n\t" \
    "mov rdi, [rbp + " S(a) " + 0]\n\t" \
    "lea r15, [rdi + rdi]\n\t" "lea r13, [rsi + rsi]\n\t" \
    "lea r9,  [r12 + r12]\n\t" "lea r10, [r11 + r11]\n\t" \
    "imul rbx, r14, 19\n\t"    "imul rdx, r11, 19\n\t" \
    "xor eax, eax\n\t" "xor r8d, r8d\n\t" TRIG \
    "mov [rbp + " S(out) " + 0],  rdi\n\t" "mov [rbp + " S(out) " + 8],  r9\n\t" \
    "mov [rbp + " S(out) " + 16], r10\n\t" "mov [rbp + " S(out) " + 24], rbx\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

static ladder_state_t g_st;
static void init_operands(void) {
    uint64_t *p = (uint64_t *)&g_st;
    for (size_t i = 0; i < sizeof(g_st) / 8; i++)
        p[i] = (0x123456789ABCDULL * (i + 1)) & MASK51;
}

#define SQ_LAT(T)  REP8(FE_SQ_T(B_OFF, A_OFF, T)  FE_SQ_T(A_OFF, B_OFF, T))
#define MUL_LAT(T) REP8(FE_MUL_T(B_OFF, A_OFF, Z2_OFF, T) FE_MUL_T(A_OFF, B_OFF, Z2_OFF, T))
#define SQ_TPUT(T) REP4(FE_SQ_T(B_OFF, A_OFF, T)   FE_SQ_T(BB_OFF, AA_OFF, T) \
                        FE_SQ_T(D_OFF, C_OFF, T)   FE_SQ_T(DA_OFF, CB_OFF, T))

#define MAX_ARMS 32
typedef struct { const char *label; double med; } arm_t;
static arm_t g_arm[MAX_ARMS];
static int   g_narms;
static void arm_record(const char *l, double m) {
    for (int i = 0; i < g_narms; i++)
        if (g_arm[i].label == l) { if (m < g_arm[i].med) g_arm[i].med = m; return; }
    g_arm[g_narms].label = l; g_arm[g_narms].med = m; g_narms++;
}
static double arm_get(const char *l) {
    for (int i = 0; i < g_narms; i++) if (g_arm[i].label == l) return g_arm[i].med;
    return -1.0;
}
static uint64_t g_smp[K_REPS];

#define TIME_ASM(LABEL, BODY) do {                                            \
    for (int q = 0; q < K_REPS; q++) {                                        \
        register ladder_state_t *_st asm("rbp") = &g_st;                      \
        uint64_t _a = rdtsc_start();                                          \
        for (int _i = 0; _i < K_INNER; _i++)                                  \
            asm volatile(BODY : : "r"(_st)                                    \
                : "rax","rbx","rcx","rdx","rsi","rdi",                        \
                  "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc");\
        g_smp[q] = rdtsc_end() - _a;                                          \
    }                                                                         \
    uint64_t _mn, _md, _p10, _p90;                                            \
    bench_stats(g_smp, K_REPS, &_mn, &_md, &_p10, &_p90);                     \
    arm_record(LABEL, (double)_md / K_OPS);                                   \
} while (0)

#define MUL_ADDR   0x7c00UL
#define SQ_ADDR    0x7d08UL
#define PROBE_SQ   0x7db0UL
#define PROBE_MUL  0x7db4UL
#define UA_VMW     0x0cd8UL
#define UA_VMR     0x0618UL

static ucode_t probe_sq_body[]  = { { ZEROEXT_DSZ64_DR(RDI, RDI), NOP, NOP, END_SEQWORD } };
static ucode_t probe_mul_body[] = { { ZEROEXT_DSZ64_DR(R15, RDI), NOP, NOP, END_SEQWORD } };

/* one fe_sq through the vmwrite trigger, for the correctness check */
static void fe_sq_vmw(const uint64_t a[5], uint64_t out[5]) {
    memcpy(g_st.A, a, 40);
    register ladder_state_t *st asm("rbp") = &g_st;
    asm volatile(FE_SQ_T(B_OFF, A_OFF, TRIG_VMW) : : "r"(st)
        : "rax","rbx","rcx","rdx","rsi","rdi","r8","r9","r10","r11",
          "r12","r13","r14","r15","memory","cc");
    memcpy(out, g_st.B, 40);
}
static void fe_mul_vmr(const uint64_t a[5], const uint64_t b[5], uint64_t out[5]) {
    memcpy(g_st.A, a, 40); memcpy(g_st.z2, b, 40);
    register ladder_state_t *st asm("rbp") = &g_st;
    asm volatile(FE_MUL_T(B_OFF, A_OFF, Z2_OFF, TRIG_VMR) : : "r"(st)
        : "rax","rbx","rcx","rdx","rsi","rdi","r8","r9","r10","r11",
          "r12","r13","r14","r15","memory","cc");
    memcpy(out, g_st.B, 40);
}

static int canon_eq(const uint64_t x[5], const uint64_t y[5]) {
    uint64_t a[5], b[5];
    fe_reduce(a, x); fe_reduce(b, y);
    return memcmp(a, b, 40) == 0;
}

static int check_swapped(void) {
    uint64_t a[5], b[5], got[5], want[5];
    int bad = 0;
    srandom(12345);
    for (int t = 0; t < 200; t++) {
        for (int i = 0; i < 5; i++) {
            a[i] = ((uint64_t)random() << 32 ^ (uint64_t)random()) & MASK51;
            b[i] = ((uint64_t)random() << 32 ^ (uint64_t)random()) & MASK51;
        }
        fe_sq_vmw(a, got);   fe_sq_fiat(a, want);
        if (!canon_eq(got, want)) bad++;
        fe_mul_vmr(a, b, got); fe_mul_fiat(a, b, want);
        if (!canon_eq(got, want)) bad++;
    }
    return bad;
}

int main(void) {
    if (geteuid() != 0) { printf("needs root\n"); return 1; }
    printf("=== probe_trigger: trigger instruction vs wrapper ===\n\n");
    assign_to_core(0);
    init_operands();
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();          /* mul at U7c00, sq at U7d08 */
    patch_ucode(PROBE_SQ,  probe_sq_body,  1);
    patch_ucode(PROBE_MUL, probe_mul_body, 1);
    printf("\n");
    if (test_rfc7748()) { printf("baseline RFC 7748 FAILED - abort\n");
                          init_match_and_patch(); do_fix_IN_patch(); return 1; }
    printf("baseline verified.\n\n");

    /* ---- 2x2 floor cross: {sq,mul} wrapper x {vmwrite,vmread} trigger ---- */
    for (int ph = 0; ph < K_PHASES; ph++) {
        init_operands();
        hook_match_and_patch(0, UA_VMW, PROBE_SQ);
        hook_match_and_patch(1, UA_VMR, PROBE_MUL);
        TIME_ASM("floor sq-wrapper  x vmwrite", SQ_LAT(TRIG_VMW));
        TIME_ASM("floor mul-wrapper x vmread ", MUL_LAT(TRIG_VMR));

        init_operands();
        hook_match_and_patch(0, UA_VMW, PROBE_MUL);
        hook_match_and_patch(1, UA_VMR, PROBE_SQ);
        TIME_ASM("floor mul-wrapper x vmwrite", MUL_LAT(TRIG_VMW));
        TIME_ASM("floor sq-wrapper  x vmread ", SQ_LAT(TRIG_VMR));
    }

    /* ---- rule out the match/patch entry index: give entry 0 the vmread
     * match and entry 1 the vmwrite match, so each instruction is served by
     * the OTHER index. Both entries stay live on distinct addresses. ---- */
    for (int ph = 0; ph < K_PHASES; ph++) {
        init_operands();
        hook_match_and_patch(0, UA_VMR, PROBE_SQ);
        hook_match_and_patch(1, UA_VMW, PROBE_MUL);
        TIME_ASM("floor sq-wrapper  x vmread  [idx0]", SQ_LAT(TRIG_VMR));
        TIME_ASM("floor mul-wrapper x vmwrite [idx1]", MUL_LAT(TRIG_VMW));
    }
    /* restore the normal index->instruction assignment */
    hook_match_and_patch(0, UA_VMW, MUL_ADDR);
    hook_match_and_patch(1, UA_VMR, SQ_ADDR);

    /* ---- real patches, both hook assignments ---- */
    for (int ph = 0; ph < K_PHASES; ph++) {
        init_operands();
        hook_match_and_patch(0, UA_VMW, MUL_ADDR);
        hook_match_and_patch(1, UA_VMR, SQ_ADDR);
        TIME_ASM("fe_mul via vmwrite (production)", MUL_LAT(TRIG_VMW));
        TIME_ASM("fe_sq  via vmread  (production)", SQ_LAT(TRIG_VMR));

        init_operands();
        hook_match_and_patch(0, UA_VMW, SQ_ADDR);
        hook_match_and_patch(1, UA_VMR, MUL_ADDR);
        TIME_ASM("fe_sq  via vmwrite (swapped)",    SQ_LAT(TRIG_VMW));
        TIME_ASM("fe_mul via vmread  (swapped)",    MUL_LAT(TRIG_VMR));
        TIME_ASM("fe_sq  via vmwrite, tput",        SQ_TPUT(TRIG_VMW));
    }

    /* correctness in the swapped configuration */
    hook_match_and_patch(0, UA_VMW, SQ_ADDR);
    hook_match_and_patch(1, UA_VMR, MUL_ADDR);
    int bad = check_swapped();
    printf("swapped-hook correctness vs fiat-crypto: %s (%d mismatches / 400 ops)\n\n",
           bad ? "FAIL" : "OK", bad);

    printf("  %-38s %8s\n", "arm", "cyc/op");
    printf("  %-38s %8s\n", "--------------------------------------", "--------");
    const char *rows[] = {
        "floor sq-wrapper  x vmwrite", "floor sq-wrapper  x vmread ",
        "floor mul-wrapper x vmwrite", "floor mul-wrapper x vmread ",
        "floor sq-wrapper  x vmread  [idx0]", "floor mul-wrapper x vmwrite [idx1]",
        "fe_mul via vmwrite (production)", "fe_sq  via vmread  (production)",
        "fe_sq  via vmwrite (swapped)",    "fe_mul via vmread  (swapped)",
        "fe_sq  via vmwrite, tput", NULL };
    for (int i = 0; rows[i]; i++) printf("  %-38s %8.1f\n", rows[i], arm_get(rows[i]));

    double sw = arm_get("floor sq-wrapper  x vmwrite"), sr = arm_get("floor sq-wrapper  x vmread ");
    double mw = arm_get("floor mul-wrapper x vmwrite"), mr = arm_get("floor mul-wrapper x vmread ");
    printf("\n  trigger effect (vmread - vmwrite): sq wrapper %+.1f, mul wrapper %+.1f\n", sr-sw, mr-mw);
    printf("  wrapper effect (sq - mul):         vmwrite %+.1f, vmread %+.1f\n", sw-mw, sr-mr);
    double sqp = arm_get("fe_sq  via vmread  (production)"), sqs = arm_get("fe_sq  via vmwrite (swapped)");
    double mlp = arm_get("fe_mul via vmwrite (production)"), mls = arm_get("fe_mul via vmread  (swapped)");
    printf("\n  swapping the hooks: fe_sq %.1f -> %.1f (%+.1f), fe_mul %.1f -> %.1f (%+.1f)\n",
           sqp, sqs, sqs-sqp, mlp, mls, mls-mlp);
    printf("  per ladder step (4 sq + 5 mul): %+.0f cyc\n", 4*(sqs-sqp) + 5*(mls-mlp));

    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
