/*
 * probe_sched.c — what is the residual made of? (PLAN open questions O5, O6)
 *
 * Three census-based cost models have now been fitted and refuted against
 * measured fe_mul variants:
 *
 *   r as exposed parallelism   depth fell 2.95x, r fell 13%
 *   cost per triad             fit on 3 points, predicted 77.8, measured 87.8
 *   any census model           two schedules, IDENTICAL census, 5.7 cyc apart
 *
 * That last one is the reason this probe exists. 174 ops / 58 triads /
 * depth 25.4 measured 102.8 one way and 108.5 the other; the only
 * difference was op order and which scratch registers the allocator handed
 * out. And probe_issue says a synthetic patch with fe_mul's op mix runs at
 * 0.58 cyc/triad where production runs at 1.49. So per-op cost is a
 * function of something neither op counts nor dependency depth captures.
 *
 * Four families, each sweeping the triad count so the invocation floor
 * cancels in the slope, each holding a census fixed and varying ONE
 * structural dimension:
 *
 *   width  W independent dependency chains, W = 1..16.  probe_issue's arms
 *          are effectively W = 16 (all ops read the same two sources,
 *          destinations rotate over the whole TMP file). Production's five
 *          accumulators give it W = 5. If cyc/op is still falling at W = 5
 *          then production is ILP limited and the lever is MORE
 *          accumulators, not fewer operations -- and probe_issue's 0.153
 *          was measuring an ILP level production cannot reach.
 *
 *   regs   maximal ILP but destinations cycle through only R distinct
 *          registers, R = 1..16. Flat in R means the register file is
 *          renamed and false dependencies are free. Rising as R shrinks
 *          means WAW/WAR serialises, scratch rotation is load-bearing, and
 *          register assignment alone can explain the 5.7 cyc.
 *
 *   mulpack same triads, same op count, SAME NUMBER OF MULTIPLIES, but
 *          either <=1 per triad or 3 per triad. The 108.5 schedule packed
 *          two multiplies into single triads where the 102.8 one spread
 *          them, so this is the leading suspect for O6. If clustered is
 *          much worse, "never put two MULs in one triad" is an actionable
 *          scheduling rule.
 *
 *   dist   dependent MUL -> ADD pairs with the consumer 2D ops after its
 *          producer, D = 1..12. Same census, same register count, only the
 *          order changes. Measures whether the out-of-order window is
 *          already hiding the ~5.9 cyc multiply latency or whether the
 *          schedule has to.
 *
 * METHOD, per PLAN section 6 O3: every arm ends with one triad that moves a
 * touched register into R15, which the wrapper stores. That makes each
 * firing's result depend on the patch, so firings serialise instead of
 * overlapping into an aggregate-throughput measurement, and R15 changing at
 * all is evidence the flow reached the last triad. The headline is the
 * SLOPE over the triad sweep, which is immune to any error in the floor; a
 * flat slope would mean the triads are not executing.
 *
 * Every arm also prints its own census, so the claim "these arms differ in
 * exactly one dimension" is checked rather than asserted. probe_issue's
 * first version measured the wrong thing for want of that.
 *
 * Opcodes used: ADD_DSZ64_DRR, MUL_DSZ64_DRR, ZEROEXT_DSZ64_DR only -- all
 * long since verified. Nothing here fires an unverified encoding.
 *
 * Build: make PROG=probe_sched
 * Run:   sudo taskset -c 0 ./probe_sched_static
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

/* The production fe_mul wrapper, unmodified, so this probe's floor is the
 * same 16.18 cyc bench_kernel measures. 16 firings per asm block. */
#define ARM REP8(FE_MUL(B_OFF, A_OFF, Z2_OFF) FE_MUL(A_OFF, B_OFF, Z2_OFF))

static uint64_t g_smp[K_REPS];

#define NT_MAX 42
static ucode_t buf[NT_MAX + 2];
static const int NTS[] = { 12, 21, 30, 42 };
#define N_NTS ((int)(sizeof NTS / sizeof NTS[0]))
static const uint64_t TMP[16] = { TMP0,TMP1,TMP2,TMP3,TMP4,TMP5,TMP6,TMP7,
                                  TMP8,TMP9,TMP10,TMP11,TMP12,TMP13,TMP14,TMP15 };

/* ── census, recomputed from the built patch ─────────────────────────── */
typedef struct { int triads, ops, muls, maxmul_triad, nregs; } census_t;
static census_t census(int nt) {
    census_t c = { nt, 0, 0, 0, 0 };
    unsigned seen = 0;
    for (int t = 0; t < nt; t++) {
        uint64_t u[3] = { buf[t].uop0, buf[t].uop1, buf[t].uop2 };
        int m = 0;
        for (int s = 0; s < 3; s++) {
            if (u[s] == NOP) continue;
            c.ops++;
            int ismul = ((u[s] >> 32) & 0xfff) == (_MUL_DSZ64 >> 32)
                     || ((u[s] >> 32) & 0xfff) == (_IMUL64L_DSZ64 >> 32);
            int destructive = ((u[s] >> 32) & 0xfff) == (_MUL_DSZ64 >> 32);
            if (ismul) { c.muls++; m++; }
            uint64_t d = (u[s] >> 12) & 0x3f;          /* DST_ENCODE: bits 17:12 */
            if (d >= TMP0 && d <= TMP15) seen |= 1u << (d - TMP0);
            if (destructive) {              /* MUL also WRITES srcB (the low half) */
                uint64_t b = (u[s] >> 6) & 0x3f;       /* SRC1_ENCODE: bits 11:6 */
                if (b >= TMP0 && b <= TMP15) seen |= 1u << (b - TMP0);
            }
        }
        if (m > c.maxmul_triad) c.maxmul_triad = m;
    }
    for (int i = 0; i < 16; i++) if (seen >> i & 1) c.nregs++;
    return c;
}

static int seal(int nt, uint64_t touched) {
    buf[nt].uop0 = ZEROEXT_DSZ64_DR(R15, touched);
    buf[nt].uop1 = NOP; buf[nt].uop2 = NOP;
    buf[nt].seqw = END_SEQWORD;
    return nt + 1;
}

/* ── family: W independent dependency chains of ADDs ─────────────────── */
static int build_width(int nt, int W) {
    int k = 0;
    for (int t = 0; t < nt; t++) {
        uint64_t u[3];
        for (int s = 0; s < 3; s++, k++) {
            uint64_t c = TMP[k % W];
            u[s] = ADD_DSZ64_DRR(c, c, RDI);      /* RAW on c: a real chain */
        }
        buf[t].uop0 = u[0]; buf[t].uop1 = u[1]; buf[t].uop2 = u[2];
        buf[t].seqw = NOP_SEQWORD;
    }
    return seal(nt, TMP[0]);
}

/* ── family: maximal ILP, only R distinct destination registers ──────── */
static int build_regs(int nt, int R) {
    int k = 0;
    for (int t = 0; t < nt; t++) {
        uint64_t u[3];
        for (int s = 0; s < 3; s++, k++)
            u[s] = ADD_DSZ64_DRR(TMP[k % R], RDI, RSI);   /* no RAW at all */
        buf[t].uop0 = u[0]; buf[t].uop1 = u[1]; buf[t].uop2 = u[2];
        buf[t].seqw = NOP_SEQWORD;
    }
    return seal(nt, TMP[0]);
}

/* ── family: same multiply count, spread over triads vs clustered ────── */
/* Both arms: nt triads, 3*nt ops, nt multiplies. spread puts one multiply
 * in every triad; clustered puts three in every third triad. Identical
 * census except max multiplies per triad. */
static int build_mulpack(int nt, int clustered) {
    int k = 0;
    for (int t = 0; t < nt; t++) {
        uint64_t u[3];
        for (int s = 0; s < 3; s++, k++) {
            uint64_t d = TMP[k % 8], e = TMP[8 + (k % 8)];
            int ismul = clustered ? (t % 3 == 0) : (s == 0);
            u[s] = ismul ? MUL_DSZ64_DRR(d, RDI, e)
                         : ADD_DSZ64_DRR(d, RDI, RSI);
        }
        buf[t].uop0 = u[0]; buf[t].uop1 = u[1]; buf[t].uop2 = u[2];
        buf[t].seqw = NOP_SEQWORD;
    }
    return seal(nt, TMP[0]);
}

/* ── family: dependent MUL -> ADD, consumer 2D ops after its producer ── */
/* 11 producer registers (TMP0-8, TMP14, TMP15) so that D up to 10 is a real
 * producer-to-consumer distance rather than aliasing onto a smaller one. */
#define NP 11
static const uint64_t PRD[NP] = { TMP0,TMP1,TMP2,TMP3,TMP4,TMP5,TMP6,TMP7,
                                  TMP8,TMP14,TMP15 };
static const uint64_t ACC[5] = { TMP9, TMP10, TMP11, TMP12, TMP13 };
static int build_dist(int nt, int D) {
    int k = 0, nop = 3 * nt;
    uint64_t acc = TMP[0];      /* sealed register; the five below feed it */
    for (int t = 0; t < nt; t++) {
        uint64_t u[3];
        for (int s = 0; s < 3; s++, k++) {
            if (k >= nop) { u[s] = NOP; continue; }
            int i = k / 2;
            if (k % 2 == 0) {                      /* produce into r[i%DK] */
                uint64_t h = PRD[i % NP];
                /* IMUL64L, not MUL: MUL writes its srcB, so 63 multiplies
                 * sharing one srcB register formed a single 63-deep RAW
                 * chain, 63 * 5.9 = 372 cyc, which is what the first
                 * version of this arm actually measured (376). IMUL64L is
                 * non-destructive (probe_opsem [2]), so the producers are
                 * genuinely independent and D is the only variable. */
                u[s] = IMUL64L_DSZ64_DRR(h, RDI, RSI);
            } else {                               /* consume r[(i-D)%DK] */
                int j = i - D; if (j < 0) j += NP;
                uint64_t h = PRD[j % NP];
                /* five accumulators, as production has, so the consumers are
                 * five chains 1/5 as deep rather than one serial chain that
                 * would mask the effect of D */
                u[s] = ADD_DSZ64_DRR(ACC[i % 5], ACC[i % 5], h);
            }
        }
        buf[t].uop0 = u[0]; buf[t].uop1 = u[1]; buf[t].uop2 = u[2];
        buf[t].seqw = NOP_SEQWORD;
    }
    (void)acc;
    return seal(nt, ACC[0]);
}

/* ── arm table ───────────────────────────────────────────────────────── */
enum fam { F_WIDTH, F_REGS, F_MULPACK, F_DIST };
typedef struct { const char *label; enum fam f; int p; } armdef_t;
static const armdef_t ARMS[] = {
    { "width  W=1  (fully serial)", F_WIDTH,   1 },
    { "width  W=2",                 F_WIDTH,   2 },
    { "width  W=3",                 F_WIDTH,   3 },
    { "width  W=5  (production)",   F_WIDTH,   5 },
    { "width  W=8",                 F_WIDTH,   8 },
    { "width  W=16 (probe_issue)",  F_WIDTH,  16 },
    { "regs   R=1  (all same dst)", F_REGS,    1 },
    { "regs   R=2",                 F_REGS,    2 },
    { "regs   R=4",                 F_REGS,    4 },
    { "regs   R=8",                 F_REGS,    8 },
    { "regs   R=16 (full TMP file)",F_REGS,   16 },
    { "mulpack spread  (1/triad)",  F_MULPACK, 0 },
    { "mulpack clustered (3/triad)",F_MULPACK, 1 },
    { "dist   D=1  (adjacent)",     F_DIST,    1 },
    { "dist   D=3",                 F_DIST,    3 },
    { "dist   D=6",                 F_DIST,    6 },
    { "dist   D=10",                F_DIST,   10 },
};
#define N_ARMS ((int)(sizeof ARMS / sizeof ARMS[0]))

static int build(const armdef_t *a, int nt) {
    switch (a->f) {
    case F_WIDTH:   return build_width(nt, a->p);
    case F_REGS:    return build_regs(nt, a->p);
    case F_MULPACK: return build_mulpack(nt, a->p);
    default:        return build_dist(nt, a->p);
    }
}

int main(void) {
    printf("=== probe_sched: O5/O6 — what is the residual made of? ===\n");
    printf("sweeping triads %d..%d; slope is the answer and is floor-independent.\n",
           NTS[0], NTS[N_NTS - 1]);
    printf("every arm ends with ZEROEXT(R15, touched) so firings serialise and\n");
    printf("the flow is observably reaching the last triad.\n\n");

    /* Census self-check FIRST, and it needs no root: the arms are supposed to
     * differ in exactly one structural dimension each, and that is a claim
     * about the built patch, so check it rather than asserting it. */
    printf("  %-30s %7s %5s %5s %8s %6s\n",
           "arm (census at 42 triads)", "triads", "ops", "MULs", "max/triad", "regs");
    for (int k = 0; k < N_ARMS; k++) {
        build(&ARMS[k], NT_MAX);
        census_t c = census(NT_MAX);
        printf("  %-30s %7d %5d %5d %8d %6d\n",
               ARMS[k].label, c.triads, c.ops, c.muls, c.maxmul_triad, c.nregs);
    }
    printf("\n");

    if (geteuid() != 0) {
        printf("census only; re-run under sudo for the timing sweep\n");
        return 0;
    }
    assign_to_core(0);
    init_operands();
    init_match_and_patch();
    do_fix_IN_patch();

    static double t[N_ARMS][N_NTS];
    for (int ph = 0; ph < K_PHASES; ph++) {
        for (int k = 0; k < N_ARMS; k++) {
            for (int j = 0; j < N_NTS; j++) {
                int n = build(&ARMS[k], NTS[j]);
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

    printf("  %-30s", "triads ->");
    for (int j = 0; j < N_NTS; j++) printf(" %7d", NTS[j]);
    printf("   cyc/triad  cyc/op\n");
    for (int k = 0; k < N_ARMS; k++) {
        if (k && ARMS[k].f != ARMS[k-1].f) printf("\n");
        printf("  %-30s", ARMS[k].label);
        for (int j = 0; j < N_NTS; j++) printf(" %7.1f", t[k][j]);
        double sx=0, sy=0, sxx=0, sxy=0;
        for (int j = 0; j < N_NTS; j++) {
            sx += NTS[j]; sy += t[k][j];
            sxx += (double)NTS[j]*NTS[j]; sxy += (double)NTS[j]*t[k][j];
        }
        double slope = (N_NTS*sxy - sx*sy) / (N_NTS*sxx - sx*sx);
        printf("   %8.3f %7.3f\n", slope, slope / 3.0);
    }

    printf("\n  Reference points from bench_kernel, same wrapper and floor:\n");
    printf("    production fe_mul   58 triads, 1.493 cyc/triad, 0.498 cyc/op\n");
    printf("    the 108.5 variant   58 triads, 1.591 cyc/triad, same census\n");
    printf("    probe_issue mix     synthetic,  0.58 cyc/triad\n");
    printf("  width and regs deliberately overlap on register count, so\n");
    printf("  width(W=k) vs regs(R=k) isolates the cost of a REAL dependency\n");
    printf("  chain at matched register pressure. regs R=16 should reproduce\n");
    printf("  probe_issue's alu3 (~0.153 cyc/op): that is the harness check.\n");
    printf("  If the width sweep is still falling at W=5, production is ILP\n");
    printf("  bound and probe_issue's 0.153 cyc/op was an unreachable ILP level.\n");
    printf("  If the regs sweep is flat, register assignment cannot explain the\n");
    printf("  5.7 cyc and mulpack/dist must; if it is not flat, scratch\n");
    printf("  rotation is load-bearing and must stay in gen_mul_patch.py.\n");

    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
