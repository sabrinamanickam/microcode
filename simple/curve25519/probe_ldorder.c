/*
 * probe_ldorder.c — the last difference between the two wrappers.
 *
 * The fe_sq wrapper costs 45.6 cyc where fe_mul's costs 15.7, with the same
 * stub patch and the same trigger. Already excluded: the trigger instruction,
 * the match/patch entry index, the 4 LEAs, the 2 IMULs, writing RDX before
 * the trigger, and the store-source cascade. What is left is the ORDER of the
 * memory operations:
 *
 *   fe_mul   loads [a+0],[a+8],[a+16],[a+24],[a+32] then [b+*]  (ascending)
 *            stores [out+0] .. [out+32]                          (ascending)
 *   fe_sq    loads [a+32],[a+24],[a+16],[a+8],[a+0]              (DESCENDING)
 *            stores [out+0] .. [out+32]                          (ascending)
 *
 * The chain pings A and B, so iteration k's [out] is iteration k+1's [a].
 * With fe_sq's ordering the LAST store of one op writes [X+32] and the FIRST
 * load of the next op reads [X+32] -- one instruction later. fe_mul's first
 * load reads [X+0], five stores after its producer. A store-forwarding stall
 * or a memory-order replay at that distance would cost tens of cycles and
 * would be invisible in the issue-bound throughput arm, which is exactly the
 * signature we have (wrapper measured on disjoint slots: 7.5 cyc, cheaper
 * than fe_mul's 10.2).
 *
 * PART A bisects the wrapper from fe_mul's shape to fe_sq's, one change at a
 * time, all arms sharing the same stub patch (ZEROEXT r15,rdi), the same
 * trigger, and the same stores, so only the loads differ.
 *
 * PART B is the payoff: the REAL fe_sq patch driven by wrappers that differ
 * only in memory-op order. Reordering the loads is semantically free -- the
 * LEAs and IMULs just need their sources first -- so if this is the cause,
 * fe_sq drops without touching the patch at all. Checked against fiat-crypto.
 *
 * Build: make PROG=probe_ldorder
 * Run:   sudo taskset -c 0 ./probe_ldorder_static
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"
#include "sq_fast_patch.h"
#include <unistd.h>

#define K_INNER  8
#define K_REPS   200
#define K_PHASES 3
#define K_OPS    (K_INNER * 16)
#define REP2(x)  x x
#define REP4(x)  REP2(x) REP2(x)
#define REP8(x)  REP4(x) REP4(x)

#define TRIG_VMW "vmwrite rcx, rdx\n\t"
#define TRIG_VMR ".byte 0x0f, 0x78, 0xca\n\t"

/* ---- load blocks ---- */
#define LD_ASC(a) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  "mov rsi, [rbp + " S(a) " + 8]\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t" "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t"
#define LD_DESC(a) \
    "mov r14, [rbp + " S(a) " + 32]\n\t" "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t" "mov rsi, [rbp + " S(a) " + 8]\n\t" \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"
#define LD_B(b) \
    "mov r15, [rbp + " S(b) " + 0]\n\t"  "mov r13, [rbp + " S(b) " + 8]\n\t" \
    "mov r9,  [rbp + " S(b) " + 16]\n\t" "mov r10, [rbp + " S(b) " + 24]\n\t" \
    "mov rbx, [rbp + " S(b) " + 32]\n\t"
#define ARITH \
    "lea r15, [rdi + rdi]\n\t" "lea r13, [rsi + rsi]\n\t" \
    "lea r9,  [r12 + r12]\n\t" "lea r10, [r11 + r11]\n\t" \
    "imul rbx, r14, 19\n\t"    "imul rdx, r11, 19\n\t"
#define ZEROS "xor eax, eax\n\t" "xor r8d, r8d\n\t"

/* stub arms: only [out+0] is live, everything else stores the zero in rax */
#define ST_STUB(out) \
    "mov [rbp + " S(out) " + 0],  r15\n\t" "mov [rbp + " S(out) " + 8],  rax\n\t" \
    "mov [rbp + " S(out) " + 16], rax\n\t" "mov [rbp + " S(out) " + 24], rax\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

#define W_MULLIKE(out,a,b)   LD_ASC(a)  LD_B(b) ZEROS TRIG_VMW ST_STUB(out)
#define W_ASC5(out,a,b)      LD_ASC(a)          ZEROS TRIG_VMW ST_STUB(out)
#define W_DESC5(out,a,b)     LD_DESC(a)         ZEROS TRIG_VMW ST_STUB(out)
#define W_DESC5_AR(out,a,b)  LD_DESC(a)         ARITH ZEROS TRIG_VMW ST_STUB(out)
#define W_ASC5_AR(out,a,b)   LD_ASC(a)          ARITH ZEROS TRIG_VMW ST_STUB(out)
#define W_DESC10(out,a,b)    LD_DESC(a) LD_B(b) ZEROS TRIG_VMW ST_STUB(out)

/* ---- real fe_sq wrappers: production vs reordered memory ops ---- */
#define SQ_ST_ASC(out) \
    "mov [rbp + " S(out) " + 0],  rdi\n\t" "mov [rbp + " S(out) " + 8],  r9\n\t" \
    "mov [rbp + " S(out) " + 16], r10\n\t" "mov [rbp + " S(out) " + 24], rbx\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"
#define SQ_ST_DESC(out) \
    "mov [rbp + " S(out) " + 32], rax\n\t" "mov [rbp + " S(out) " + 24], rbx\n\t" \
    "mov [rbp + " S(out) " + 16], r10\n\t" "mov [rbp + " S(out) " + 8],  r9\n\t" \
    "mov [rbp + " S(out) " + 0],  rdi\n\t"

#define FE_SQ_PROD(out,a)  LD_DESC(a) ARITH ZEROS TRIG_VMR SQ_ST_ASC(out)   /* production */
#define FE_SQ_ASCLD(out,a) LD_ASC(a)  ARITH ZEROS TRIG_VMR SQ_ST_ASC(out)   /* loads ascending */
#define FE_SQ_DESCST(out,a) LD_DESC(a) ARITH ZEROS TRIG_VMR SQ_ST_DESC(out) /* stores descending */

static ladder_state_t g_st;
static void init_operands(void) {
    uint64_t *p = (uint64_t *)&g_st;
    for (size_t i = 0; i < sizeof(g_st) / 8; i++)
        p[i] = (0x123456789ABCDULL * (i + 1)) & MASK51;
}

#define STUB_ARM(W)  REP8(W(B_OFF, A_OFF, Z2_OFF) W(A_OFF, B_OFF, Z2_OFF))
#define SQ_ARM(W)    REP8(W(B_OFF, A_OFF)         W(A_OFF, B_OFF))
#define SQ_TPUT(W)   REP4(W(B_OFF, A_OFF)   W(BB_OFF, AA_OFF) \
                          W(D_OFF, C_OFF)   W(DA_OFF, CB_OFF))

#define MAX_ARMS 32
typedef struct { const char *label; double med; } arm_t;
static arm_t g_arm[MAX_ARMS];
static int g_narms;
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

static ucode_t stub_mul[] = { { ZEROEXT_DSZ64_DR(R15, RDI), NOP, NOP, END_SEQWORD } };
#define SQ_ADDR 0x7d08UL

/* one fe_sq with ascending loads, for the correctness check */
static void fe_sq_ascld(const uint64_t a[5], uint64_t out[5]) {
    memcpy(g_st.A, a, 40);
    register ladder_state_t *st asm("rbp") = &g_st;
    asm volatile(FE_SQ_ASCLD(B_OFF, A_OFF) : : "r"(st)
        : "rax","rbx","rcx","rdx","rsi","rdi","r8","r9","r10","r11",
          "r12","r13","r14","r15","memory","cc");
    memcpy(out, g_st.B, 40);
}

int main(void) {
    if (geteuid() != 0) { printf("needs root\n"); return 1; }
    printf("=== probe_ldorder: memory-op order in the fe_sq wrapper ===\n\n");
    assign_to_core(0);
    init_operands();
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();
    printf("\n");
    if (test_rfc7748()) { printf("baseline FAILED\n"); init_match_and_patch(); do_fix_IN_patch(); return 1; }

    /* PART A: stub bisect on the vmwrite hook (mul patch is displaced; the
     * fe_sq patch at U7d08 stays put for part B). */
    patch_ucode(0x7c00, stub_mul, 1);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    for (int ph = 0; ph < K_PHASES; ph++) {
        init_operands();
        TIME_ASM("A1 10 loads, a ascending  (fe_mul)", STUB_ARM(W_MULLIKE));
        TIME_ASM("A2  5 loads, a ascending",           STUB_ARM(W_ASC5));
        TIME_ASM("A3  5 loads, a DESCENDING",          STUB_ARM(W_DESC5));
        TIME_ASM("A4  5 desc + lea/imul  (fe_sq)",     STUB_ARM(W_DESC5_AR));
        TIME_ASM("A5  5 asc  + lea/imul",              STUB_ARM(W_ASC5_AR));
        TIME_ASM("A6 10 loads, a descending",          STUB_ARM(W_DESC10));
    }

    /* PART B: the real fe_sq patch, three memory-op orderings */
    hook_match_and_patch(1, 0x0618, SQ_ADDR);
    for (int ph = 0; ph < K_PHASES; ph++) {
        init_operands();
        TIME_ASM("B1 fe_sq, production order",   SQ_ARM(FE_SQ_PROD));
        TIME_ASM("B2 fe_sq, loads ascending",    SQ_ARM(FE_SQ_ASCLD));
        TIME_ASM("B3 fe_sq, stores descending",  SQ_ARM(FE_SQ_DESCST));
        TIME_ASM("B4 fe_sq, asc loads, tput",    SQ_TPUT(FE_SQ_ASCLD));
    }
    /* and with the carry-merged-last patch, in case the two compose */
    patch_ucode(SQ_ADDR, sq_fast_src, (int)ARRAY_SZ(sq_fast_src));
    hook_match_and_patch(1, 0x0618, SQ_ADDR);
    for (int ph = 0; ph < K_PHASES; ph++) {
        init_operands();
        TIME_ASM("B5 sq_fast patch, asc loads",  SQ_ARM(FE_SQ_ASCLD));
    }

    /* correctness of the reordered wrapper (production patch) */
    patch_ucode(SQ_ADDR, sq_fast_src, 0);           /* no-op, keep layout clear */
    install_field_patches();
    int bad = 0;
    srandom(999);
    for (int t = 0; t < 300; t++) {
        uint64_t a[5], got[5], want[5], ra[5], rb[5];
        for (int i = 0; i < 5; i++)
            a[i] = ((uint64_t)random() << 32 ^ (uint64_t)random()) & MASK51;
        fe_sq_ascld(a, got); fe_sq_fiat(a, want);
        fe_reduce(ra, got);  fe_reduce(rb, want);
        if (memcmp(ra, rb, 40)) bad++;
    }
    printf("\nascending-load wrapper vs fiat-crypto: %s (%d / 300)\n\n", bad ? "FAIL" : "OK", bad);

    printf("--- PART A: stub bisect (same patch, same trigger, same stores) ---\n");
    const char *pa[] = { "A1 10 loads, a ascending  (fe_mul)", "A2  5 loads, a ascending",
                         "A3  5 loads, a DESCENDING", "A4  5 desc + lea/imul  (fe_sq)",
                         "A5  5 asc  + lea/imul", "A6 10 loads, a descending", NULL };
    for (int i = 0; pa[i]; i++) printf("  %-38s %8.1f\n", pa[i], arm_get(pa[i]));

    printf("\n--- PART B: real fe_sq, memory order only ---\n");
    const char *pb[] = { "B1 fe_sq, production order", "B2 fe_sq, loads ascending",
                         "B3 fe_sq, stores descending", "B4 fe_sq, asc loads, tput",
                         "B5 sq_fast patch, asc loads", NULL };
    for (int i = 0; pb[i]; i++) printf("  %-38s %8.1f\n", pb[i], arm_get(pb[i]));
    printf("\n  reference: fe_mul 122.9, fiat fe_sq 117.5, hand-C fe_sq 114.8\n");
    double b1 = arm_get("B1 fe_sq, production order"), b2 = arm_get("B2 fe_sq, loads ascending");
    printf("  load reorder alone: %.1f -> %.1f (%+.1f cyc, %+.1f%%)\n", b1, b2, b2-b1, 100.0*(b2-b1)/b1);

    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
