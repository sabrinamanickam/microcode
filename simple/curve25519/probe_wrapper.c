/*
 * probe_wrapper.c — which wrapper instruction costs fe_sq 30 cycles?
 *
 * probe_trigger settled that the trigger instruction is irrelevant (vmread
 * and vmwrite floors agree to 0.0 cyc) and that the fe_sq WRAPPER costs
 * +29.9 cyc over the fe_mul wrapper in a dependent chain, with the same
 * 1-triad patch behind it:
 *
 *     mul wrapper  15.7      sq wrapper  45.6
 *
 * Yet measured as pure issue bandwidth on disjoint slots, the sq wrapper is
 * the CHEAPER of the two (7.5 vs 10.2 cyc, bench_kernel Table K2). So the 30
 * cycles are a dependency effect, not instruction cost. The two wrappers
 * differ in exactly three things:
 *
 *     4x  lea  rX, [rY + rY]        the 2*a[i] precomputes
 *     2x  imul rX, rY, 19           the 19*a[i] precomputes
 *     one of those imuls targets RDX -- which is a source operand of the
 *     trigger instruction itself (vmwrite rcx,rdx / vmread rdx,rcx)
 *
 * If the microcode flow cannot begin until its architectural source operands
 * are ready, then writing RDX immediately before the trigger puts a load +
 * imul on the front of every firing. Nothing else in either wrapper feeds
 * the trigger's operands.
 *
 * Every arm below drives the SAME 1-triad dependency-carrying patch through
 * the SAME hooks; only the instruction stream around the trigger changes.
 * The M arms add one instruction to the (fast) mul wrapper; the S arms
 * remove or redirect one from the (slow) sq wrapper. Results are meaningless
 * as field arithmetic -- this measures dispatch behaviour only.
 *
 * Build: make PROG=probe_wrapper
 * Run:   sudo taskset -c 0 ./probe_wrapper_static
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

/* ---- fe_mul wrapper: loads, [EXTRA], trigger, stores ---- */
#define MUL_W(out, a, b, EXTRA) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  "mov rsi, [rbp + " S(a) " + 8]\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t" "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t" "mov r15, [rbp + " S(b) " + 0]\n\t" \
    "mov r13, [rbp + " S(b) " + 8]\n\t"  "mov r9,  [rbp + " S(b) " + 16]\n\t" \
    "mov r10, [rbp + " S(b) " + 24]\n\t" "mov rbx, [rbp + " S(b) " + 32]\n\t" \
    "xor eax, eax\n\t" "xor r8d, r8d\n\t" EXTRA TRIG_VMW \
    "mov [rbp + " S(out) " + 0],  r15\n\t" "mov [rbp + " S(out) " + 8],  r13\n\t" \
    "mov [rbp + " S(out) " + 16], r9\n\t"  "mov [rbp + " S(out) " + 24], r10\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

/* ---- fe_sq wrapper: loads, LEAS, IMULS, trigger, stores ---- */
#define SQ_W(out, a, LEAS, IMULS) \
    "mov r14, [rbp + " S(a) " + 32]\n\t" "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t" "mov rsi, [rbp + " S(a) " + 8]\n\t" \
    "mov rdi, [rbp + " S(a) " + 0]\n\t" LEAS IMULS \
    "xor eax, eax\n\t" "xor r8d, r8d\n\t" TRIG_VMR \
    "mov [rbp + " S(out) " + 0],  rdi\n\t" "mov [rbp + " S(out) " + 8],  r9\n\t" \
    "mov [rbp + " S(out) " + 16], r10\n\t" "mov [rbp + " S(out) " + 24], rbx\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

#define LEAS_STD "lea r15, [rdi + rdi]\n\t" "lea r13, [rsi + rsi]\n\t" \
                 "lea r9,  [r12 + r12]\n\t" "lea r10, [r11 + r11]\n\t"
#define LEAS_MOV "mov r15, rdi\n\t" "mov r13, rsi\n\t" \
                 "mov r9,  r12\n\t" "mov r10, r11\n\t"
#define IMUL_STD  "imul rbx, r14, 19\n\t" "imul rdx, r11, 19\n\t"
#define IMUL_NORDX "imul rbx, r14, 19\n\t" "imul r8,  r11, 19\n\t"   /* imul kept, RDX untouched */
#define IMUL_MOVDX "imul rbx, r14, 19\n\t" "mov  rdx, r11\n\t"       /* RDX written, no imul */
#define IMUL_NONE  "mov rbx, r14\n\t"      "mov r8,  r11\n\t"        /* neither */

static ladder_state_t g_st;
static void init_operands(void) {
    uint64_t *p = (uint64_t *)&g_st;
    for (size_t i = 0; i < sizeof(g_st) / 8; i++)
        p[i] = (0x123456789ABCDULL * (i + 1)) & MASK51;
}

#define MUL_ARM(EXTRA) REP8(MUL_W(B_OFF, A_OFF, Z2_OFF, EXTRA) MUL_W(A_OFF, B_OFF, Z2_OFF, EXTRA))
#define SQ_ARM(L, I)   REP8(SQ_W(B_OFF, A_OFF, L, I)           SQ_W(A_OFF, B_OFF, L, I))

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

static ucode_t probe_sq_body[]  = { { ZEROEXT_DSZ64_DR(RDI, RDI), NOP, NOP, END_SEQWORD } };
static ucode_t probe_mul_body[] = { { ZEROEXT_DSZ64_DR(R15, RDI), NOP, NOP, END_SEQWORD } };

int main(void) {
    if (geteuid() != 0) { printf("needs root\n"); return 1; }
    printf("=== probe_wrapper: locating fe_sq's 30-cycle wrapper penalty ===\n");
    printf("all arms: same 1-triad dependency-carrying patch, only the wrapper varies\n\n");
    assign_to_core(0);
    init_operands();
    init_match_and_patch();
    do_fix_IN_patch();
    patch_ucode(0x7c00, probe_mul_body, 1);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c04, probe_sq_body, 1);
    hook_match_and_patch(1, 0x0618, 0x7c04);

    for (int ph = 0; ph < K_PHASES; ph++) {
        init_operands();
        TIME_ASM("M0 mul wrapper, unchanged",        MUL_ARM(""));
        TIME_ASM("M1 mul + imul rdx, r11, 19",       MUL_ARM("imul rdx, r11, 19\n\t"));
        TIME_ASM("M2 mul + mov  rdx, r11",           MUL_ARM("mov rdx, r11\n\t"));
        TIME_ASM("M3 mul + imul rbx, r11, 19",       MUL_ARM("imul rbx, r11, 19\n\t"));
        TIME_ASM("M4 mul + mov  rcx, r11",           MUL_ARM("mov rcx, r11\n\t"));
        TIME_ASM("M5 mul + xor  edx, edx",           MUL_ARM("xor edx, edx\n\t"));

        init_operands();
        TIME_ASM("S0 sq wrapper, unchanged",         SQ_ARM(LEAS_STD, IMUL_STD));
        TIME_ASM("S1 sq, imul into r8 not rdx",      SQ_ARM(LEAS_STD, IMUL_NORDX));
        TIME_ASM("S2 sq, mov rdx (no imul)",         SQ_ARM(LEAS_STD, IMUL_MOVDX));
        TIME_ASM("S3 sq, no imul and no rdx write",  SQ_ARM(LEAS_STD, IMUL_NONE));
        TIME_ASM("S4 sq, leas->movs, imuls kept",    SQ_ARM(LEAS_MOV, IMUL_STD));
        TIME_ASM("S5 sq, leas->movs, no rdx write",  SQ_ARM(LEAS_MOV, IMUL_NONE));
    }

    printf("  %-38s %8s\n", "arm", "cyc/op");
    printf("  %-38s %8s\n", "--------------------------------------", "--------");
    const char *rows[] = {
        "M0 mul wrapper, unchanged", "M1 mul + imul rdx, r11, 19",
        "M2 mul + mov  rdx, r11",    "M3 mul + imul rbx, r11, 19",
        "M4 mul + mov  rcx, r11",    "M5 mul + xor  edx, edx",
        "S0 sq wrapper, unchanged",  "S1 sq, imul into r8 not rdx",
        "S2 sq, mov rdx (no imul)",  "S3 sq, no imul and no rdx write",
        "S4 sq, leas->movs, imuls kept", "S5 sq, leas->movs, no rdx write", NULL };
    for (int i = 0; rows[i]; i++) printf("  %-38s %8.1f\n", rows[i], arm_get(rows[i]));

    double m0 = arm_get("M0 mul wrapper, unchanged");
    double s0 = arm_get("S0 sq wrapper, unchanged");
    double s1 = arm_get("S1 sq, imul into r8 not rdx");
    printf("\n  writing RDX before the trigger costs: mul wrapper %+.1f, sq wrapper %+.1f\n",
           arm_get("M1 mul + imul rdx, r11, 19") - m0, s0 - s1);
    printf("  if S1 ~ %.1f, moving 19*a3 inside the patch recovers %.1f cyc of fe_sq\n",
           m0, s0 - s1);
    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
