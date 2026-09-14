/*
 * probe_issue.c — is 1.61 cyc/triad a sequencer issue rate, or our op mix?
 *
 * Both production patches cost the same per triad (fe_mul 107.1/66 = 1.62,
 * fe_sq 67.6/42 = 1.61) and neither gets faster when four independent ops are
 * interleaved, so both are throughput bound. But the two have nearly the same
 * multiply density (30/196 and 16/123), so those two points cannot separate
 *
 *   (a) the sequencer retires a triad every ~1.61 cycles regardless of content
 *   (b) MUL is the throughput limiter and ALU-only triads are much cheaper
 *
 * The difference decides whether fe_mul can be made competitive. Under (a) a
 * 66-triad patch cannot go below ~106 cycles whatever we remove, and OpenSSL's
 * 97.1 is out of reach. Under (b) the 25 multiplies cost ~62 cycles on either
 * implementation, our remaining ~45 cycles of carry emulation is the entire
 * deficit, and removing it is worth doing.
 *
 * Each arm is a patch of NT triads of MUTUALLY INDEPENDENT operations, so
 * nothing measures latency. Destinations rotate through the TMP file to keep
 * write-after-write distance large. Cost per triad = (cycles - floor) / NT.
 *
 *   alu3      3 ALU ops per triad, no multiplies    -> pure issue rate
 *   mul3      3 multiplies per triad                -> multiplier throughput
 *   mix       1 multiply per 2 triads               -> the fe_mul op mix
 *   setcc3    3 SETCC per triad                     -> is SETCC issue-cheap
 *
 * Build: make PROG=probe_issue
 * Run:   sudo taskset -c 0 ./probe_issue_static
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"
#include <unistd.h>

#define K_INNER  8
#define K_REPS   200
#define K_PHASES 3
#define K_OPS    (K_INNER * 16)
#define REP4(x)  x x x x
#define REP8(x)  REP4(x) REP4(x)

static ladder_state_t g_st;
static void init_operands(void) {
    uint64_t *p = (uint64_t *)&g_st;
    for (size_t i = 0; i < sizeof(g_st) / 8; i++)
        p[i] = (0x123456789ABCDULL * (i + 1)) & MASK51;
}

/* fe_mul's wrapper, whose floor we measured at 15.7 with a 1-triad patch. */
#define W(out, a, b) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  "mov rsi, [rbp + " S(a) " + 8]\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t" "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t" "mov r15, [rbp + " S(b) " + 0]\n\t" \
    "mov r13, [rbp + " S(b) " + 8]\n\t"  "mov r9,  [rbp + " S(b) " + 16]\n\t" \
    "mov r10, [rbp + " S(b) " + 24]\n\t" "mov rbx, [rbp + " S(b) " + 32]\n\t" \
    "xor eax, eax\n\t" "xor r8d, r8d\n\t" "vmwrite rcx, rdx\n\t" \
    "mov [rbp + " S(out) " + 0],  r15\n\t" "mov [rbp + " S(out) " + 8],  r13\n\t" \
    "mov [rbp + " S(out) " + 16], r9\n\t"  "mov [rbp + " S(out) " + 24], r10\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"
#define ARM REP8(W(B_OFF, A_OFF, Z2_OFF) W(A_OFF, B_OFF, Z2_OFF))

#define MAX_ARMS 16
typedef struct { const char *label; double med; } arm_t;
static arm_t g_arm[MAX_ARMS]; static int g_narms;
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
#define TIME_ASM(LABEL) do {                                                  \
    for (int q = 0; q < K_REPS; q++) {                                        \
        register ladder_state_t *_st asm("rbp") = &g_st;                      \
        uint64_t _a = rdtsc_start();                                          \
        for (int _i = 0; _i < K_INNER; _i++)                                  \
            asm volatile(ARM : : "r"(_st)                                     \
                : "rax","rbx","rcx","rdx","rsi","rdi",                        \
                  "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc");\
        g_smp[q] = rdtsc_end() - _a;                                          \
    }                                                                         \
    uint64_t _mn,_md,_p10,_p90;                                               \
    bench_stats(g_smp, K_REPS, &_mn,&_md,&_p10,&_p90);                        \
    arm_record(LABEL, (double)_md / K_OPS);                                   \
} while (0)

#define NT_MAX 40
static ucode_t buf[NT_MAX + 2];
static const int NTS[] = { 5, 10, 20, 30, 40 };
#define N_NTS ((int)(sizeof NTS / sizeof NTS[0]))
static const uint64_t TMP[16] = { TMP0,TMP1,TMP2,TMP3,TMP4,TMP5,TMP6,TMP7,
                                  TMP8,TMP9,TMP10,TMP11,TMP12,TMP13,TMP14,TMP15 };
enum { A_ALU3, A_MUL3, A_MIX, A_SETCC3, A_N };
static const char *nm[A_N] = { "alu3   (3 ALU, no MUL)", "mul3   (3 MUL)",
                               "mix    (fe_mul op mix)", "setcc3 (3 SETCC)" };

/* Build `nt` triads of mutually independent ops, then ONE final triad that
 * moves a touched TMP into R15.
 *
 * That last triad does two jobs. It makes the wrapper's stored value depend on
 * the patch, so consecutive firings serialise instead of overlapping and the
 * measurement is per firing rather than aggregate throughput. And because the
 * sequencer is in order, R15 arriving changed at all is evidence the flow
 * reached the end rather than terminating early.
 *
 * The headline number is the SLOPE across the triad sweep, not any single
 * point. A slope is immune to any error in the invocation floor, and a flat
 * slope would mean the triads are not executing. */
static int build(int kind, int nt) {
    int r = 0;
    for (int t = 0; t < nt; t++) {
        uint64_t u[3];
        for (int s = 0; s < 3; s++) {
            uint64_t d = TMP[r++ & 15], e = TMP[r & 15];
            switch (kind) {
            case A_ALU3:   u[s] = ADD_DSZ64_DRR(d, RDI, RSI); break;
            case A_MUL3:   u[s] = MUL_DSZ64_DRR(d, RDI, e);   break;
            case A_SETCC3: u[s] = SETCC_CONDB_DR(d, e);       break;
            default:
                /* fe_mul is 30 MUL in 196 ops: one multiply every ~2 triads */
                u[s] = (s == 0 && (t & 1) == 0) ? MUL_DSZ64_DRR(d, RDI, e)
                                                : ADD_DSZ64_DRR(d, RDI, RSI);
                break;
            }
        }
        buf[t].uop0 = u[0]; buf[t].uop1 = u[1]; buf[t].uop2 = u[2];
        buf[t].seqw = NOP_SEQWORD;
    }
    buf[nt].uop0 = ZEROEXT_DSZ64_DR(R15, TMP0);
    buf[nt].uop1 = NOP; buf[nt].uop2 = NOP;
    buf[nt].seqw = END_SEQWORD;
    return nt + 1;
}

int main(void) {
    if (geteuid() != 0) { printf("needs root\n"); return 1; }
    printf("=== probe_issue: what limits a patch body to 1.61 cyc/triad? ===\n");
    printf("sweeping the triad count; all operations mutually independent,\n");
    printf("one final triad moves a touched TMP into R15 so firings serialise\n");
    printf("and the flow is observably reaching the end\n\n");
    assign_to_core(0);
    init_operands();
    init_match_and_patch();
    do_fix_IN_patch();

    static double t[A_N][N_NTS];
    for (int ph = 0; ph < K_PHASES; ph++) {
        for (int k = 0; k < A_N; k++) {
            for (int j = 0; j < N_NTS; j++) {
                int n = build(k, NTS[j]);
                patch_ucode(0x7c00, buf, n);
                hook_match_and_patch(0, 0x0cd8, 0x7c00);
                init_operands();
                for (int q = 0; q < K_REPS; q++) {
                    register ladder_state_t *_st asm("rbp") = &g_st;
                    uint64_t _a = rdtsc_start();
                    for (int _i = 0; _i < K_INNER; _i++)
                        asm volatile(ARM : : "r"(_st)
                            : "rax","rbx","rcx","rdx","rsi","rdi","r8","r9","r10",
                              "r11","r12","r13","r14","r15","memory","cc");
                    g_smp[q] = rdtsc_end() - _a;
                }
                uint64_t mn, md, p10, p90;
                bench_stats(g_smp, K_REPS, &mn, &md, &p10, &p90);
                double v = (double)md / K_OPS;
                if (ph == 0 || v < t[k][j]) t[k][j] = v;
            }
        }
    }

    printf("  %-24s", "triads ->");
    for (int j = 0; j < N_NTS; j++) printf(" %7d", NTS[j]);
    printf("   slope(cyc/triad)\n");
    for (int k = 0; k < A_N; k++) {
        printf("  %-24s", nm[k]);
        for (int j = 0; j < N_NTS; j++) printf(" %7.1f", t[k][j]);
        /* least squares slope over the sweep */
        double sx=0, sy=0, sxx=0, sxy=0;
        for (int j = 0; j < N_NTS; j++) {
            sx += NTS[j]; sy += t[k][j];
            sxx += (double)NTS[j]*NTS[j]; sxy += (double)NTS[j]*t[k][j];
        }
        double slope = (N_NTS*sxy - sx*sy) / (N_NTS*sxx - sx*sx);
        double icept = (sy - slope*sx) / N_NTS;
        printf("   %6.2f   (intercept %.1f)\n", slope, icept);
    }
    printf("\n  The slope is the answer; it does not depend on the floor.\n");
    printf("  Production patches average 1.61-1.62 cyc/triad overall.\n");
    printf("  A flat slope would mean the triads are not executing at all.\n");
    printf("  If alu3's slope is well under mul3's, multiplies are the limiter\n");
    printf("  and the carry emulation is the deficit worth attacking.\n");

    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
