/* probe_mul_critpath.c — Is the looped fe_mul's 1.64 cyc/triad set by a SERIAL
 * critical path (reschedulable -> mul can beat native 100c) or by the sequencer's
 * per-triad THROUGHPUT (stuck -> only fewer triads help)?
 *
 * Context: probe_looped_fieldop showed looped fe_mul = 118c (66 triads, 1.64
 * c/triad), losing to native amd64-64 mul (100c). The 5x51 mul body is ~25
 * MOSTLY-INDEPENDENT 64x64 partial products + a serial carry/reduction chain.
 * If independent ops overlap in a loop (throughput < latency), the body's cost
 * is critical-path-bound and rescheduling the carry chain could approach the
 * throughput floor (~66c) -> beat native. If independent ops DON'T overlap
 * (the in-order sequencer issues ~1 triad / 1.64c regardless), the body is
 * triad-throughput-bound and rescheduling can't help -> looped mul ties at best.
 *
 * Method: a single-firing backward loop (SEQ_GOTO0; backward UJMPCC crashes) whose
 * body is K copies of a chosen triad. cyc/iter = (cyc(N=32)-cyc(N=2))/30 removes
 * the firing tax; per-triad rate = (cyc/iter(K=30) - cyc/iter(K=10))/20 removes
 * loop overhead. Four bodies:
 *   ADD_DEP    1 dependent ADD/triad  (TMP0 chain)     -> cheap-op latency
 *   ADD_INDEP  1 independent ADD/triad (rotating dst)  -> cheap-op throughput
 *   MUL_DEP    1 dependent MUL/triad  (TMP0 chain)     -> MUL latency
 *   MUL_INDEP  1 independent MUL/triad (rotating pair) -> MUL throughput
 * MUL_INDEP << MUL_DEP  => big reschedulable headroom in the mul body.
 * MUL_INDEP ~= MUL_DEP  => sequencer-bound, looped mul can't beat native.
 *
 * NOTE: MUL_DSZ64_DRR(hi,srcA,srcB) is two-output: srcB=lo, hi=hi, srcA preserved.
 *
 * Build: make PROG=probe_mul_critpath CFLAGS="-O3 -march=native -masm=intel -fwrapv -fPIE -I include/"
 * Run:   sudo taskset -c 0 ./probe_mul_critpath_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define REGION 0x7c00
#define T(n)   (REGION + (n)*4)

enum { ADD_DEP, ADD_INDEP, MUL_DEP, MUL_INDEP };
static const char *MODE_NAME[] = { "ADD_DEP", "ADD_INDEP", "MUL_DEP", "MUL_INDEP" };

/* rotating independent destinations / (hi,srcB) pairs -- avoid counter TMP12,
 * test TMP13, multiplicand TMP1, add-const TMP15/TMP14. */
static const int IDST[]   = { TMP0, TMP2, TMP3, TMP4, TMP6, TMP7, TMP10, TMP11 };
static const int IHI[]    = { TMP2, TMP5, TMP8, TMP9 };
static const int ISRCB[]  = { TMP3, TMP4, TMP6, TMP7 };

static ucode_t p[128];

/* Build the loop patch: init, loop_top(increment), K body triads, forward CONDZ
 * exit, SEQ_GOTO0 back, exit. */
static int build(int mode, int K, int N) {
    int n = 0;
    p[n++] = (ucode_t){ ZEROEXT_DSZ32_DI(TMP12, 0),     NOP, NOP, NOP_SEQWORD };  /* counter */
    p[n++] = (ucode_t){ ZEROEXT_DSZ32_DI(TMP1, 0x12345), NOP, NOP, NOP_SEQWORD }; /* multiplicand/addend */
    p[n++] = (ucode_t){ ZEROEXT_DSZ32_DI(TMP0, 3),      NOP, NOP, NOP_SEQWORD };  /* chain seed */
    p[n++] = (ucode_t){ ZEROEXT_DSZ32_DI(TMP15, 7), ZEROEXT_DSZ32_DI(TMP14, 9), NOP, NOP_SEQWORD };
    int loop_top = n;
    p[n++] = (ucode_t){ ADD_DSZ64_DRI(TMP12, TMP12, 1), NOP, NOP, NOP_SEQWORD };  /* benign re-entry */
    for (int i = 0; i < K; i++) {
        switch (mode) {
        case ADD_DEP:
            p[n++] = (ucode_t){ ADD_DSZ64_DRR(TMP0, TMP0, TMP1), NOP, NOP, NOP_SEQWORD };
            break;
        case ADD_INDEP:
            p[n++] = (ucode_t){ ADD_DSZ64_DRR(IDST[i % 8], TMP1, TMP15), NOP, NOP, NOP_SEQWORD };
            break;
        case MUL_DEP:
            /* srcB=TMP0 gets lo -> RAW chain on TMP0; hi=TMP2 dead; srcA=TMP1 kept */
            p[n++] = (ucode_t){ MUL_DSZ64_DRR(TMP2, TMP1, TMP0), NOP, NOP, NOP_SEQWORD };
            break;
        case MUL_INDEP:
            /* 4 independent (hi,srcB) pairs cycle -> consecutive triads independent */
            p[n++] = (ucode_t){ MUL_DSZ64_DRR(IHI[i % 4], TMP1, ISRCB[i % 4]), NOP, NOP, NOP_SEQWORD };
            break;
        }
    }
    int exit_idx = n + 2;
    p[n++] = (ucode_t){ XOR_DSZ64_DRI(TMP13, TMP12, N), NOP,
                        UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(TMP13, T(exit_idx)), NOP_SEQWORD };
    p[n++] = (ucode_t){ NOP, NOP, NOP, SEQ_GOTO0(T(loop_top)) };
    p[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(RAX, TMP12), NOP, NOP, END_SEQWORD };
    return n;
}

static inline uint64_t rdtsc_start(void){uint32_t lo,hi;asm volatile("cpuid\n\trdtsc":"=a"(lo),"=d"(hi)::"rbx","rcx","memory");return((uint64_t)hi<<32)|lo;}
static inline uint64_t rdtsc_end(void){uint32_t lo,hi;asm volatile("rdtscp":"=a"(lo),"=d"(hi)::"rcx","memory");asm volatile("cpuid":::"rax","rbx","rcx","rdx","memory");return((uint64_t)hi<<32)|lo;}

#define REP2(x) x x
#define REP4(x) REP2(x) REP2(x)
#define REP8(x) REP4(x) REP4(x)
#define REP16(x) REP8(x) REP8(x)
#define UNROLL 16
#define REPS 2000
#define TRIALS 50
#define FIRE "vmwrite rcx, rdx\n\t"
#define CL "rbx","rcx","rdx","rsi","rdi","rbp","r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"

/* install mode/K/N, return min cyc per firing (firing does N internal iters) */
static double time_cfg(int mode, int K, int N, uint64_t *checkout) {
    int n = build(mode, K, N);
    init_match_and_patch(); do_fix_IN_patch();
    hook_match_and_patch(0, 0x0cd8, REGION);
    patch_ucode(REGION, p, n);
    /* correctness sanity: RAX should == N (loop ran N times) */
    uint64_t chk; asm volatile(FIRE : "=a"(chk) :: CL);
    if (checkout) *checkout = chk;
    uint64_t best = ~0ULL;
    for (int t = 0; t < TRIALS; t++) {
        uint64_t a = rdtsc_start();
        for (int r = 0; r < REPS; r++) asm volatile(REP16(FIRE) ::: "rax", CL);
        uint64_t c = rdtsc_end() - a;
        if (c < best) best = c;
    }
    return (double)best / ((double)REPS * UNROLL);
}

/* cyc per internal iteration for given mode/K (tax removed via N-slope) */
static double cyc_per_iter(int mode, int K) {
    uint64_t c2chk, c32chk;
    double c2  = time_cfg(mode, K, 2,  &c2chk);
    double c32 = time_cfg(mode, K, 32, &c32chk);
    if (c2chk != 2 || c32chk != 32)
        printf("    [warn] %s K=%d loop count wrong (N=2->%" PRIu64 ", N=32->%" PRIu64 ")\n",
               MODE_NAME[mode], K, c2chk, c32chk);
    return (c32 - c2) / 30.0;
}

int main(void) {
    assign_to_core(0);
    printf("=== probe_mul_critpath: looped per-triad latency vs throughput ===\n\n");
    printf("  %-10s %12s %12s %14s\n", "body", "cyc/iter K=10", "cyc/iter K=30", "cyc/TRIAD");
    printf("  %-10s %12s %12s %14s\n", "----", "------------", "------------", "---------");

    double rate[4];
    for (int mode = 0; mode < 4; mode++) {
        double i10 = cyc_per_iter(mode, 10);
        double i30 = cyc_per_iter(mode, 30);
        rate[mode] = (i30 - i10) / 20.0;
        printf("  %-10s %12.2f %12.2f %14.3f\n", MODE_NAME[mode], i10, i30, rate[mode]);
    }

    printf("\nINTERPRET:\n");
    printf("  MUL throughput (MUL_INDEP) = %.3f c/triad ; MUL latency (MUL_DEP) = %.3f\n",
           rate[MUL_INDEP], rate[MUL_DEP]);
    printf("  looped fe_mul body measured 1.64 c/triad; native amd64-64 mul = 100c (66 triads).\n");
    if (rate[MUL_INDEP] > 0.01) {
        double floor_c = rate[MUL_INDEP] * 66.0;
        printf("  => ideal-schedule mul floor ~ %.0f c (66 triads x MUL throughput).\n", floor_c);
        printf("     %s native 100c. ratio latency/throughput = %.2fx (headroom for rescheduling).\n",
               floor_c < 100 ? "BEATS" : "above", rate[MUL_DEP]/rate[MUL_INDEP]);
    }
    printf("  If MUL_INDEP << MUL_DEP: independent partial-products overlap -> the serial\n");
    printf("  carry chain is the bottleneck -> rescheduling the mul body can cut latency.\n");
    printf("  If MUL_INDEP ~= MUL_DEP: sequencer issues ~1 triad/rate regardless -> stuck.\n");

    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
