/* probe_mul_isolate.c — is the microcode field-op win the MUL primitive, or
 * not? Companion to probe_carry_isolate.c.
 *
 * Against amd64-51/asm the same-ladder microcode swap is ~6.9% (RESULTS.md).
 * The amd64-51 asm and our patch use the SAME hardware multiplier — amd64-51
 * issues `mulq` (64x64->128, rdx:rax), we issue MUL_DSZ64_DRR. If the patch
 * MUL is no cheaper than mulq, the field-op edge is NOT the MUL and (by
 * elimination, with probe_carry_isolate) points at carry handling.
 *
 * This probe measures, all with the same rdtsc harness:
 *   MUL_DEP   — M MULs in a dependent chain (acc = acc*K) -> per-MUL LATENCY.
 *   MUL_INDEP — M independent MULs                          -> per-MUL THROUGHPUT.
 *   NOMUL     — same triad count / same depth, MUL replaced by ADD
 *               (acc = acc+K)                                -> ALU baseline.
 *               => MUL marginal cost = MUL_DEP - NOMUL.
 *   mulq REF  — M dependent `mulq` in plain inline asm (no patch), same harness
 *               -> the amd64-51 multiplier cost to beat.
 *
 * Read the result:
 *   patch MUL_DEP/M  ~=  mulq REF/M   => MUL is not the lever (same silicon);
 *                                        the ~6.9% is elsewhere (see carry probe).
 *   patch MUL_DEP/M  <   mulq REF/M   => the patch MUL is genuinely cheaper and
 *                                        contributes to the field-op edge.
 *
 * CAVEAT: per-firing latency may be overhead/triad-bound (see probe_sq_latency.c
 * — RUN IT FIRST). If MUL_DEP ~= NOMUL despite MUL being a heavier op, the MUL
 * cost is hidden under firing overhead at this granularity and you must measure
 * inside a full fe_mul instead.
 *
 * Build: make PROG=probe_mul_isolate CFLAGS="-O3 -march=native -masm=intel -fwrapv -fPIE -I include/"
 * Run:   sudo taskset -c 0 ./probe_mul_isolate_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define M  30          /* chain length (~ a fe_mul's MUL count) */
#define K  3           /* multiplier / addend constant */

static ucode_t patch[256];

/* Dependent chain on R9 (acc). MUL_DSZ64_DRR(hi, src0, src1): src0 preserved,
 * src1 gets lo, hi gets hi. So MUL(RCX, RDX, R9): RDX(=K) preserved, R9 <- lo,
 * RCX <- hi. Next MUL reads R9 => depth M. acc_final = K^M mod 2^64. */
static int build_mul_dep(void) {
    int n = 0;
    for (int i = 0; i < M; i++)
        patch[n++] = (ucode_t){ MUL_DSZ64_DRR(RCX, RDX, R9), NOP, NOP, NOP_SEQWORD };
    patch[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(RDI, R9), NOP, NOP, END_SEQWORD };
    return n;
}

/* Same triad count / same depth, MUL -> ADD. ADD(R9, R9, RDX): acc += K,
 * depth M. acc_final = acc0 + M*K = 1 + M*K. Isolates MUL's marginal cost. */
static int build_nomul(void) {
    int n = 0;
    for (int i = 0; i < M; i++)
        patch[n++] = (ucode_t){ ADD_DSZ64_DRR(R9, R9, RDX), NOP, NOP, NOP_SEQWORD };
    patch[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(RDI, R9), NOP, NOP, END_SEQWORD };
    return n;
}

/* M independent MULs: each reads RDX(=K) and a fresh srcB, writes a distinct
 * dst — no cross-MUL dependency -> throughput. We feed K into a rotating set of
 * scratch arch regs and discard. Result reg RDI is set to a fixed sentinel so
 * verify just checks "ran" (independent MULs have no single chained value). */
static int build_mul_indep(void) {
    int n = 0;
    /* sources r8..r15-ish that the trigger seeds to K; each MUL: dst=hi sink,
     * src0=that reg, src1=RDX(=K) gets clobbered with lo (RDX reloaded? no — to
     * keep them independent we use src1 = a per-iter reg too). Simplest: use a
     * pool of regs as src1 so no MUL depends on another's output. */
    const int pool[6] = { R8, R10, R11, R12, R14, R15 };
    for (int i = 0; i < M; i++) {
        int s = pool[i % 6];
        patch[n++] = (ucode_t){ MUL_DSZ64_DRR(RCX, RDX, s), NOP, NOP, NOP_SEQWORD };
    }
    patch[n++] = (ucode_t){ ADD_DSZ64_DRI(RDI, RDX, 0), NOP, NOP, END_SEQWORD };
    return n;
}

/* ---- trigger + timing (mirrors probe_sq_latency.c) -------------------- */

/* acc = R9 = 1, K = RDX = 3, independent pool regs = K. Fire, store RDI. */
#define FIRE \
    "mov r9, 1\n\t"               \
    "mov rdx, " #K "\n\t"         \
    "mov r8, "  #K "\n\t"  "mov r10, " #K "\n\t" "mov r11, " #K "\n\t" \
    "mov r12, " #K "\n\t"  "mov r14, " #K "\n\t" "mov r15, " #K "\n\t" \
    ".byte 0x0f, 0x78, 0xca\n\t"  /* vmread rdx,rcx -> patched */ \
    "mov [rbp + 0], rdi\n\t"

#define CLOBBERS \
    "rax","rbx","rcx","rdx","rsi","rdi", \
    "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"

/* mulq reference chain: rax=1, rcx=K; M dependent `mul rcx` (rax = lo). No
 * patch; this is what amd64-51's multiplier costs under the same harness. */
#define REPMULQ_1 "mul rcx\n\t"
#define REPMULQ_2  REPMULQ_1 REPMULQ_1
#define REPMULQ_4  REPMULQ_2 REPMULQ_2
#define REPMULQ_8  REPMULQ_4 REPMULQ_4
#define REPMULQ_16 REPMULQ_8 REPMULQ_8
#define REPMULQ_30 REPMULQ_16 REPMULQ_8 REPMULQ_4 REPMULQ_2   /* == M=30 */
#define FIRE_MULQ \
    "mov rax, 1\n\t"  "mov rcx, " #K "\n\t" \
    REPMULQ_30                              \
    "mov [rbp + 0], rax\n\t"

#define REP2(x)  x x
#define REP4(x)  REP2(x) REP2(x)
#define REP8(x)  REP4(x) REP4(x)
#define REP16(x) REP8(x) REP8(x)
#define UNROLL 16
#define REPS   1000
#define TRIALS 50

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

static void fire_once(uint64_t *v) {
    register uint64_t *p asm("rbp") = v;
    asm volatile(FIRE : : "r"(p) : CLOBBERS);
}

static uint64_t time_patch(uint64_t *v) {
    uint64_t best = ~0ULL;
    for (int t = 0; t < TRIALS; t++) {
        register uint64_t *p asm("rbp") = v;
        uint64_t a = rdtsc_start();
        for (int r = 0; r < REPS; r++)
            asm volatile(REP16(FIRE) : : "r"(p) : CLOBBERS);
        uint64_t c = rdtsc_end() - a;
        if (c < best) best = c;
    }
    return best;
}

static uint64_t time_mulq(uint64_t *v) {
    uint64_t best = ~0ULL;
    for (int t = 0; t < TRIALS; t++) {
        register uint64_t *p asm("rbp") = v;
        uint64_t a = rdtsc_start();
        for (int r = 0; r < REPS; r++)
            asm volatile(REP16(FIRE_MULQ) : : "r"(p) : CLOBBERS);
        uint64_t c = rdtsc_end() - a;
        if (c < best) best = c;
    }
    return best;
}

static void run_patch(const char *name, int (*build)(void), int verify, uint64_t exp) {
    int n = build();
    patch_ucode(0x7c00, patch, n);
    hook_match_and_patch(1, 0x0618, 0x7c00);

    uint64_t buf[1] = {0};
    fire_once(buf);
    const char *verdict = !verify ? "ran"
                        : (buf[0] == exp ? "OK" : "MISMATCH");

    uint64_t best = time_patch(buf);
    double per = (double)best / ((double)REPS * UNROLL);
    printf("  %-9s %6d   %9.2f   %8.3f   out=%016llx [%s]\n",
           name, n, per, per / (double)M,
           (unsigned long long)buf[0], verdict);
}

int main(void) {
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    /* expected acc for the dependent MUL chain: K^M mod 2^64 */
    uint64_t powKM = 1;
    for (int i = 0; i < M; i++) powKM *= (uint64_t)K;

    printf("=== MUL isolation (M=%d, K=%d) ===\n\n", M, K);
    printf("  %-9s %6s   %9s   %8s   %s\n",
           "variant", "triads", "cyc/fire", "cyc/op", "result");
    printf("  %-9s %6s   %9s   %8s   %s\n",
           "-------", "------", "--------", "------", "------");

    run_patch("MUL_DEP",   build_mul_dep,   1, powKM);
    run_patch("MUL_INDEP", build_mul_indep, 0, 0);
    run_patch("NOMUL",     build_nomul,     1, (uint64_t)(1 + (uint64_t)M * K));

    /* mulq reference (no patch installed matters; the trigger byte isn't used) */
    uint64_t buf[1] = {0};
    uint64_t best = time_mulq(buf);
    double per = (double)best / ((double)REPS * UNROLL);
    printf("  %-9s %6s   %9.2f   %8.3f   out=%016llx [%s]\n",
           "mulq REF", "-", per, per / (double)M,
           (unsigned long long)buf[0], (buf[0] == powKM ? "OK" : "chk"));

    printf("\nMUL marginal cost  = (MUL_DEP - NOMUL) cyc/fire, /%d per MUL.\n", M);
    printf("Compare patch MUL cyc/op to mulq REF cyc/op:\n");
    printf("  ~equal  => MUL is not the field-op lever (same multiplier).\n");
    printf("  patch<  => patch MUL is cheaper and contributes to the edge.\n");

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
