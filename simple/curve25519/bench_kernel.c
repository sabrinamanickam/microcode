/*
 * bench_kernel.c — LEVEL 1 of the three-level evaluation: isolated
 * fe_mul / fe_sq kernel latency, every backend, one process, pinned.
 *
 * Levels 2 and 3 already exist:
 *   level 2  controlled X25519 integration   (bench_table.c, CONTROLS.md)
 *   level 3  complete implementations        (full_curve25519_inline2.c)
 * This binary supplies level 1 so the paper's per-op claim rests on a
 * measurement in the SAME units, the SAME process and the SAME harness as
 * its native competitors, instead of a standalone number.
 *
 * WHAT IS MEASURED
 *
 * (1) Latency, memory-to-memory, dependent chain. This is the quantity the
 *     Montgomery ladder actually pays: every field op consumes the previous
 *     op's result out of memory. Operands PING-PONG between two distinct
 *     field slots, so the chain is a true data dependency without the
 *     same-address store-then-load STLF stall that inflated earlier
 *     fe_sq microbenchmarks.
 *
 * (2) The microcode wrapper / patch-redirection cost, broken out. A microcode
 *     field op is not just the patch body; the caller must marshal 5+5 limbs
 *     into fixed architectural registers, execute the hooked instruction,
 *     and store 5 result limbs back. We separate three layers:
 *
 *       wrapper movs only  no firing at all: the loads/LEA/IMUL/stores alone
 *       dispatch floor     the same wrapper, but the hook runs a 1-triad
 *                          no-op patch (ZEROEXT rdi,rdi) instead of the real
 *                          body -> wrapper + redirection + sequencer
 *                          entry/exit + result turnaround
 *       full patch         the production fe_mul / fe_sq patch
 *
 *     so  redirection = floor - wrapper   and   patch body = full - floor.
 *
 * (3) Throughput, independent ops. Native code has limb-level ILP that a
 *     dependent chain hides, and it answers "did you measure the natives in
 *     their worst case?". Four independent chains are interleaved.
 *
 * (4) The C/asm backends are measured twice: with a memory clobber after
 *     each op (so they pay the same mandatory memory round-trip the microcode
 *     wrapper can never avoid -- the apples-to-apples column) and without one
 *     (compiler free to keep limbs in registers -- their best case). Both are
 *     reported so the barrier cannot be accused of manufacturing the result.
 *
 * BACKENDS: microcode (this work), fiat-crypto, CryptOpt, Bernstein-Schwabe
 * amd64-51 asm, hand-written __uint128_t C. All five come from
 * full_curve25519_inline2.c, so they are byte-identical to the ones benched
 * end-to-end in the paper's tables.
 *
 * Build: make PROG=bench_kernel
 * Run:   sudo taskset -c 0 ./bench_kernel_static      (all arms)
 *        taskset -c 0 ./bench_kernel_static           (native arms only,
 *                                                      no root needed)
 * Pin the core first: ../pin_cpu.sh / lib/freq_guard.sh (no_turbo=1,
 * userspace governor). An unpinned run reports RDTSC ticks while the core
 * bursts to 2.4 GHz and understates every cycle count by ~2.2x.
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"
#include <unistd.h>

/* ── harness geometry ────────────────────────────────────────────────
 * one sample = K_INNER asm blocks x K_UNROLL ops = 128 ops, so the ~15-tick
 * rdtsc overhead is <0.1% of a sample. K_REPS samples feed bench_stats;
 * K_PHASES passes in round-robin order guard against slow drift.        */
#define K_UNROLL 16
#define K_INNER  8
#define K_REPS   200
#define K_PHASES 4
#define K_OPS    (K_INNER * K_UNROLL)

#define REP2(x)  x x
#define REP4(x)  REP2(x) REP2(x)
#define REP8(x)  REP4(x) REP4(x)

/* ── operand storage ────────────────────────────────────────────────── */
static ladder_state_t g_st;          /* microcode operands, via [rbp+off] */
static uint64_t pa[5], pb[5], pk[5]; /* C-backend ping-pong + multiplier   */
static uint64_t qa[5], qb[5];        /* second independent chain (tput)    */
static uint64_t ra[5], rb[5];        /* third                              */
static uint64_t sa[5], sb[5];        /* fourth                             */

/* ── additional native backends ──────────────────────────────────────
 * OpenSSL x25519_fe51_{mul,sqr}: hand written 5x51 assembly, generated from
 * crypto/ec/asm/x25519-x86_64.pl. This build emits the fe51 path only; the
 * fe64 entry points assemble to ud2 stubs because they need ADX, which this
 * core does not have (no bmi2, no adx in /proc/cpuinfo), so OpenSSL would
 * select the fe51 path on this machine anyway.
 *
 * s2n-bignum bignum_{mul,sqr}_p25519_alt: formally verified assembly, but a
 * SATURATED 4x64 representation rather than 5x51, so it is reported in its
 * own group. The _alt variants are the ones that run here; the base variants
 * use MULX/ADCX/ADOX and would fault on Goldmont.
 * ──────────────────────────────────────────────────────────────────── */
void x25519_fe51_mul(uint64_t h[5], const uint64_t f[5], const uint64_t g[5]);
void x25519_fe51_sqr(uint64_t h[5], const uint64_t f[5]);
void bignum_mul_p25519_alt(uint64_t z[4], const uint64_t x[4], const uint64_t y[4]);
void bignum_sqr_p25519_alt(uint64_t z[4], const uint64_t x[4]);

/* fe_mul_ossl / fe_sq_ossl now come from full_curve25519_inline2.c, which
 * gained an end-to-end OpenSSL contender; defining them here too is a
 * redefinition error. */
static inline void fe_mul_s2n(const uint64_t *a, const uint64_t *b, uint64_t *out)
{ bignum_mul_p25519_alt(out, a, b); }
static inline void fe_sq_s2n(const uint64_t *a, uint64_t *out)
{ bignum_sqr_p25519_alt(out, a); }

/* ── native invocation floor ─────────────────────────────────────────
 * The symmetric counterpart of floor_mul_lat / floor_sq_lat.
 *
 * The microcode floor is the real wrapper with a 1-triad no-op patch at the
 * hook: call-equivalent + operand marshalling + redirection, minus the
 * arithmetic. Subtracting it from the full number gives the patch body. That
 * subtraction is only meaningful against a native backend if the native
 * number has ITS OWN invocation cost removed the same way -- a native routine
 * also pays a call/ret and the same 10-load / 5-store round trip. Without
 * this arm, "patch body vs OpenSSL full op" compares a stripped quantity with
 * an unstripped one and flatters the patch.
 *
 * These are the same call shape and the same memory traffic as a real field
 * op, with no field arithmetic. noinline forces the call; the XORs keep both
 * operand streams live so the dependent chain through `out` is real (5 ALU
 * ops of ILP against a ~10-cycle floor).
 *
 * NOT symmetric in one respect, and it runs IN MICROCODE'S FAVOUR for fe_sq:
 * the microcode floor carries FE_SQ's lea/imul precompute (2*a_i and 19*a_i),
 * so subtracting the floor removes that arithmetic from the microcode side,
 * while OpenSSL's fe51_sqr does its own doubling inside the routine and keeps
 * it. The derived fe_sq rows therefore flatter microcode by roughly the cost
 * of 4 LEA + 2 IMUL. fe_mul has no such asymmetry -- its 19*b_j precompute is
 * inside the patch (IMUL64L in PREP), not in the wrapper -- so the fe_mul
 * column is the clean comparison and the one to quote. */
__attribute__((noinline, noclone))
static void fe_mul_nfloor(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t t0=a[0]^b[0], t1=a[1]^b[1], t2=a[2]^b[2], t3=a[3]^b[3], t4=a[4]^b[4];
    out[0]=t0; out[1]=t1; out[2]=t2; out[3]=t3; out[4]=t4;
    asm volatile("" ::: "memory");
}

__attribute__((noinline, noclone))
static void fe_sq_nfloor(const uint64_t *a, uint64_t *out) {
    uint64_t t0=a[0], t1=a[1], t2=a[2], t3=a[3], t4=a[4];
    out[0]=t0; out[1]=t1; out[2]=t2; out[3]=t3; out[4]=t4;
    asm volatile("" ::: "memory");
}

/* 4x64 operand storage, mirroring the 5x51 ping-pong above */
static uint64_t xa[4], xb[4], xk[4];
static uint64_t ya[4], yb[4], za4[4], zb4[4], wa[4], wb[4];

static void bytes_to_4x64(uint64_t o[4], const uint8_t in[32]) {
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;
        for (int j = 7; j >= 0; j--) v = (v << 8) | in[i*8 + j];
        o[i] = v;
    }
}
static void bytes_from_4x64(uint8_t out[32], const uint64_t in[4]) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) out[i*8 + j] = (uint8_t)(in[i] >> (8*j));
}

static void init_operands(void) {
    uint64_t *p = (uint64_t *)&g_st;
    for (size_t i = 0; i < sizeof(g_st) / 8; i++)
        p[i] = (0x123456789ABCDULL * (i + 1)) & MASK51;
    for (int i = 0; i < 5; i++) {
        pa[i] = (0x13579BDF02468ULL * (i + 1)) & MASK51;
        pb[i] = (0x2468ACE013579ULL * (i + 1)) & MASK51;
        pk[i] = (0x0FEDCBA987654ULL * (i + 1)) & MASK51;
        qa[i] = pa[i] ^ 0x11; qb[i] = pb[i] ^ 0x11;
        ra[i] = pa[i] ^ 0x22; rb[i] = pb[i] ^ 0x22;
        sa[i] = pa[i] ^ 0x33; sb[i] = pb[i] ^ 0x33;
    }
    /* 4x64 operands: the same field elements, re-encoded, so the saturated
     * backend multiplies values of the same magnitude. */
    uint8_t t[32];
    fe_tobytes(t, pa); bytes_to_4x64(xa, t);
    fe_tobytes(t, pb); bytes_to_4x64(xb, t);
    fe_tobytes(t, pk); bytes_to_4x64(xk, t);
    for (int i = 0; i < 4; i++) {
        ya[i] = xa[i]; yb[i] = xb[i];
        za4[i] = xa[i]; zb4[i] = xb[i];
        wa[i] = xa[i]; wb[i] = xb[i];
    }
}

/* Both new backends are checked against fiat-crypto before they are timed.
 * The saturated one is compared through the 32 byte encoding, since it does
 * not share the 5x51 limb layout. Returns the number of mismatches. */
static int verify_new_backends(void) {
    uint64_t a[5], b[5], got[5], want[5], ra[5], rb[5];
    uint64_t a4[4], b4[4], g4[4];
    uint8_t t[32], u[32];
    int bad = 0;
    srandom(20260908);
    for (int n = 0; n < 200; n++) {
        for (int i = 0; i < 5; i++) {
            a[i] = ((uint64_t)random() << 32 ^ (uint64_t)random()) & MASK51;
            b[i] = ((uint64_t)random() << 32 ^ (uint64_t)random()) & MASK51;
        }
        fe_mul_ossl(a, b, got); fe_mul_fiat(a, b, want);
        fe_reduce(ra, got); fe_reduce(rb, want);
        if (memcmp(ra, rb, 40)) bad++;
        fe_sq_ossl(a, got); fe_sq_fiat(a, want);
        fe_reduce(ra, got); fe_reduce(rb, want);
        if (memcmp(ra, rb, 40)) bad++;

        fe_tobytes(t, a); bytes_to_4x64(a4, t);
        fe_tobytes(t, b); bytes_to_4x64(b4, t);
        fe_mul_s2n(a4, b4, g4); bytes_from_4x64(u, g4);
        fe_mul_fiat(a, b, want); fe_tobytes(t, want);
        if (memcmp(t, u, 32)) bad++;
        fe_sq_s2n(a4, g4); bytes_from_4x64(u, g4);
        fe_sq_fiat(a, want); fe_tobytes(t, want);
        if (memcmp(t, u, 32)) bad++;
    }
    return bad;
}

/* ── wrapper-only variants: the FE_MUL / FE_SQ instruction streams with
 * the hooked instruction deleted. Isolates native-side operand marshalling
 * from redirection. Needs no patch, so it runs without root.          */
#define FE_MUL_NOFIRE(out, a, b) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  \
    "mov rsi, [rbp + " S(a) " + 8]\n\t"  \
    "mov r12, [rbp + " S(a) " + 16]\n\t" \
    "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t" \
    "mov r15, [rbp + " S(b) " + 0]\n\t"  \
    "mov r13, [rbp + " S(b) " + 8]\n\t"  \
    "mov r9,  [rbp + " S(b) " + 16]\n\t" \
    "mov r10, [rbp + " S(b) " + 24]\n\t" \
    "mov rbx, [rbp + " S(b) " + 32]\n\t" \
    "xor eax, eax\n\t"                   \
    "xor r8d, r8d\n\t"                   \
    "mov rcx, 0x7FFFFFFFFFFFF\n\t"       \
    "mov [rbp + " S(out) " + 0],  r15\n\t" \
    "mov [rbp + " S(out) " + 8],  r13\n\t" \
    "mov [rbp + " S(out) " + 16], r9\n\t"  \
    "mov [rbp + " S(out) " + 24], r10\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

#define FE_SQ_NOFIRE(out, a) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  \
    "mov rsi, [rbp + " S(a) " + 8]\n\t"  \
    "mov r12, [rbp + " S(a) " + 16]\n\t" \
    "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t" \
    "lea r15, [rdi + rdi]\n\t"           \
    "lea r13, [rsi + rsi]\n\t"           \
    "lea r9,  [r12 + r12]\n\t"           \
    "lea r10, [r11 + r11]\n\t"           \
    "imul rbx, r14, 19\n\t"              \
    "imul rdx, r11, 19\n\t"              \
    "xor eax, eax\n\t"                   \
    "xor r8d, r8d\n\t"                   \
    "mov [rbp + " S(out) " + 0],  rdi\n\t" \
    "mov [rbp + " S(out) " + 8],  r9\n\t"  \
    "mov [rbp + " S(out) " + 16], r10\n\t" \
    "mov [rbp + " S(out) " + 24], rbx\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

/* Ping-pong pairs: A<->B, each op consumes the previous op's output from a
 * DIFFERENT address than it writes. 16 ops per expansion. */
#define UC_MUL_LAT  REP8(FE_MUL(B_OFF, A_OFF, Z2_OFF) FE_MUL(A_OFF, B_OFF, Z2_OFF))
#define UC_SQ_LAT   REP8(FE_SQ(B_OFF, A_OFF)          FE_SQ(A_OFF, B_OFF))
/* Wrapper-only arms use DISJOINT slots, never ping-pong. With the firing
 * deleted the wrapper stores raw register contents, so a ping-pong would
 * manufacture a load->LEA->IMUL->store chain that the real op does not have
 * (the patch consumes those registers). Disjoint slots make this an honest
 * issue-bandwidth figure: the cost of the marshalling instructions, which in
 * situ overlap with the firing rather than adding to it. */
#define UC_MUL_NOF  REP4(FE_MUL_NOFIRE(B_OFF, A_OFF, Z2_OFF)   FE_MUL_NOFIRE(BB_OFF, AA_OFF, Z2_OFF) \
                         FE_MUL_NOFIRE(D_OFF, C_OFF, Z2_OFF)   FE_MUL_NOFIRE(DA_OFF, CB_OFF, Z2_OFF))
#define UC_SQ_NOF   REP4(FE_SQ_NOFIRE(B_OFF, A_OFF)   FE_SQ_NOFIRE(BB_OFF, AA_OFF) \
                         FE_SQ_NOFIRE(D_OFF, C_OFF)   FE_SQ_NOFIRE(DA_OFF, CB_OFF))
/* Independent: four disjoint ping-pong pairs, 4 ops per group, 16 per block. */
#define UC_MUL_TPUT REP4(FE_MUL(B_OFF, A_OFF, Z2_OFF)   FE_MUL(BB_OFF, AA_OFF, Z2_OFF) \
                         FE_MUL(D_OFF, C_OFF, Z2_OFF)   FE_MUL(DA_OFF, CB_OFF, Z2_OFF))
#define UC_SQ_TPUT  REP4(FE_SQ(B_OFF, A_OFF)   FE_SQ(BB_OFF, AA_OFF) \
                         FE_SQ(D_OFF, C_OFF)   FE_SQ(DA_OFF, CB_OFF))

/* ── in-situ diagnostics ──────────────────────────────────────────────
 * The homogeneous arms above (16 identical ops, 2-3 field slots, one hook)
 * predict 5*uc_mul + 4*uc_sq = 840.9 cycles of field work per ladder step.
 * A real step measures ~106 cycles MORE non-field time than the same C
 * ladder running OpenSSL's field ops, i.e. ~12 cycles per firing that these
 * arms do not charge. Two candidate causes, one arm each:
 *
 *   ALT      alternate mul and sq, so consecutive firings hit DIFFERENT
 *            hooks (vmwrite 0x0cd8 -> vmread 0x0618) and different patch
 *            RAM. The homogeneous arms never switch hook; a ladder step
 *            switches three times. The OpenSSL control arm alternates two
 *            ordinary calls, so anything microcode-specific shows as a
 *            difference between the two ALT arms, not as a raw number.
 *
 *   SCAT     keep one hook but rotate the operands through five field slots
 *            instead of ping-ponging between two, matching the ladder's
 *            ~600-byte working set and its longer store-to-load distances.
 *
 * Both are dependent chains, same shape and same op count as UC_*_LAT, so
 * they are directly comparable with it. */
#define UC_ALT_LAT  REP8(FE_MUL(B_OFF, A_OFF, Z2_OFF) FE_SQ(A_OFF, B_OFF))
#define UC_SCAT_LAT REP4(FE_MUL(B_OFF,  A_OFF,  Z2_OFF) FE_MUL(C_OFF,  B_OFF,  Z2_OFF) \
                         FE_MUL(DA_OFF, C_OFF,  Z2_OFF) FE_MUL(A_OFF,  DA_OFF, Z2_OFF))

/* ── result table ──────────────────────────────────────────────────── */
/* 47 arms are registered: 4 uc + 2 wrapper + 24 native lat/nb/tput
 * + 6 OpenSSL + 6 s2n + 4 dispatch-floor + 1 recheck. This was 40, which
 * silently overflowed g_arm[] from phase 0 onward: the s2n throughput and
 * all four floor arms landed out of bounds (reported as n/a), and the final
 * raw-dump loop then read a garbage label and faulted in strlen. Keep the
 * headroom, and arm_idx() now refuses to overflow rather than corrupting
 * whatever static follows. */
#define MAX_ARMS 64
typedef struct { const char *label; double med, mn; int have; } arm_t;
static arm_t g_arm[MAX_ARMS];
static int   g_narms;

static int arm_slot(const char *label) {
    for (int i = 0; i < g_narms; i++) if (g_arm[i].label == label) return i;
    if (g_narms >= MAX_ARMS) {
        fprintf(stderr, "bench_kernel: more than %d arms; raise MAX_ARMS\n", MAX_ARMS);
        exit(1);
    }
    g_arm[g_narms].label = label; g_arm[g_narms].have = 0; return g_narms++;
}
/* Keep the best (lowest) median across phases, with its own min. */
static void arm_record(const char *label, double med, double mn) {
    int i = arm_slot(label);
    if (!g_arm[i].have || med < g_arm[i].med) { g_arm[i].med = med; g_arm[i].mn = mn; g_arm[i].have = 1; }
}
static double arm_get(const char *label) {
    for (int i = 0; i < g_narms; i++)
        if (g_arm[i].label == label && g_arm[i].have) return g_arm[i].med;
    return -1.0;
}

static uint64_t g_smp[K_REPS];

/* Time a microcode / inline-asm body: K_INNER blocks per sample. */
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
    arm_record(LABEL, (double)_md / K_OPS, (double)_mn / K_OPS);              \
} while (0)

/* Time a C/asm backend. GROUP is 4 calls; 32 groups = 128 ops per sample.
 * BARRIER forces each result to memory, matching the microcode wrapper. */
#define TIME_C(LABEL, GROUP, BARRIER) do {                                    \
    for (int q = 0; q < K_REPS; q++) {                                        \
        uint64_t _a = rdtsc_start();                                          \
        for (int _i = 0; _i < K_OPS / 4; _i++) { GROUP; BARRIER; }            \
        g_smp[q] = rdtsc_end() - _a;                                          \
    }                                                                         \
    uint64_t _mn, _md, _p10, _p90;                                            \
    bench_stats(g_smp, K_REPS, &_mn, &_md, &_p10, &_p90);                     \
    arm_record(LABEL, (double)_md / K_OPS, (double)_mn / K_OPS);              \
} while (0)

#define BAR asm volatile("" ::: "memory")
#define NOBAR ((void)0)

/* Dependent: ping-pong through memory, 4 chained ops per group. */
#define C_MUL_LAT(F)  F(pa, pk, pb); F(pb, pk, pa); F(pa, pk, pb); F(pb, pk, pa)
#define C_SQ_LAT(F)   F(pa, pb);     F(pb, pa);     F(pa, pb);     F(pb, pa)
/* Alternating mul/sq, dependent — the native control for UC_ALT_LAT. */
#define C_ALT_LAT(FM,FS) FM(pa, pk, pb); FS(pb, pa); FM(pa, pk, pb); FS(pb, pa)
/* Independent: four disjoint chains. */
#define C_MUL_TPUT(F) F(pa, pk, pb); F(qa, pk, qb); F(ra, pk, rb); F(sa, pk, sb)
#define C_SQ_TPUT(F)  F(pa, pb);     F(qa, qb);     F(ra, rb);     F(sa, sb)
/* 4x64 equivalents for the saturated backend */
#define C4_MUL_LAT(F)  F(xa, xk, xb); F(xb, xk, xa); F(xa, xk, xb); F(xb, xk, xa)
#define C4_SQ_LAT(F)   F(xa, xb);     F(xb, xa);     F(xa, xb);     F(xb, xa)
#define C4_MUL_TPUT(F) F(xa, xk, xb); F(ya, xk, yb); F(za4, xk, zb4); F(wa, xk, wb)
#define C4_SQ_TPUT(F)  F(xa, xb);     F(ya, yb);     F(za4, zb4);     F(wa, wb)

/* ── the 1-triad no-op probe patches (dispatch floor) ───────────────── */
static void install_probe_patches(void) {
    ucode_t probe_mul[] = { { ZEROEXT_DSZ64_DR(RDI, RDI), NOP, NOP, END_SEQWORD } };
    ucode_t probe_sq[]  = { { ZEROEXT_DSZ64_DR(RDI, RDI), NOP, NOP, END_SEQWORD } };
    patch_ucode(0x7c00, probe_mul, 1);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c04, probe_sq, 1);
    hook_match_and_patch(1, 0x0618, 0x7c04);
}

static void print_row(const char *name, const char *lat_mul, const char *lat_sq) {
    double m = arm_get(lat_mul), s = arm_get(lat_sq);
    printf("  %-38s", name);
    if (m >= 0) printf(" %8.1f", m); else printf("      n/a");
    if (s >= 0) printf(" %8.1f", s); else printf("      n/a");
    printf("\n");
}

int main(void) {
    /* Unbuffered: this binary is statically linked, so `stdbuf` cannot touch
     * it (no dynamic loader to preload libstdbuf into), and a crash mid-run
     * otherwise loses everything still sitting in the 4096-byte block buffer
     * glibc uses whenever stdout is a pipe. Costs nothing -- the timed
     * regions never print. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Every number below is RDTSC. On an unpinned core that is not cycles --
     * see freq_guard.h. This binary recorded a full unpinned arm set once
     * already, and the ratios looked plausible, which is what makes it
     * dangerous. */
    if (freq_guard()) return 2;

    int root = (geteuid() == 0);
    printf("=== LEVEL 1: isolated fe_mul / fe_sq kernel cost (5x51) ===\n");
    printf("harness: %d ops/sample, %d samples, %d phases, ping-pong dependent chain\n",
           K_OPS, K_REPS, K_PHASES);
    printf("microcode arms: %s\n\n", root ? "ENABLED (root)" : "SKIPPED (re-run under sudo)");

    assign_to_core(0);
    init_operands();

    {
        int bad = verify_new_backends();
        printf("OpenSSL fe51 asm and s2n-bignum alt vs fiat-crypto: %s (%d mismatches / 800)\n\n",
               bad ? "FAIL" : "OK", bad);
        if (bad) { printf("aborting: a backend under test is incorrect\n"); return 1; }
    }

    if (root) {
        init_match_and_patch();
        do_fix_IN_patch();
        install_field_patches();
        printf("\n");
        if (test_rfc7748()) {
            printf("RFC 7748 FAILED with the production patches - aborting.\n");
            init_match_and_patch(); do_fix_IN_patch();
            return 1;
        }
        printf("RFC 7748 OK - the patches being timed are the verified ones.\n\n");
    }

    /* ── phase loop: production patches + every native arm, round-robin ── */
    for (int ph = 0; ph < K_PHASES; ph++) {
        init_operands();
        if (root) {
            TIME_ASM("uc_mul_lat",  UC_MUL_LAT);
            TIME_ASM("uc_sq_lat",   UC_SQ_LAT);
            TIME_ASM("uc_mul_tput", UC_MUL_TPUT);
            TIME_ASM("uc_sq_tput",  UC_SQ_TPUT);
        }
        TIME_ASM("wrap_mul", UC_MUL_NOF);
        TIME_ASM("wrap_sq",  UC_SQ_NOF);
        if (root) {
            TIME_ASM("uc_alt_lat",  UC_ALT_LAT);
            TIME_ASM("uc_scat_lat", UC_SCAT_LAT);
        }
        TIME_C("ossl_alt_lat", C_ALT_LAT(fe_mul_ossl, fe_sq_ossl), BAR);
        TIME_C("fiat_alt_lat", C_ALT_LAT(fe_mul_fiat, fe_sq_fiat), BAR);

        TIME_C("fiat_mul_lat",  C_MUL_LAT(fe_mul_fiat),      BAR);
        TIME_C("fiat_sq_lat",   C_SQ_LAT(fe_sq_fiat),        BAR);
        TIME_C("copt_mul_lat",  C_MUL_LAT(fe_mul_cryptopt),  BAR);
        TIME_C("copt_sq_lat",   C_SQ_LAT(fe_sq_cryptopt),    BAR);
        TIME_C("a51_mul_lat",   C_MUL_LAT(fe_mul_a51),       BAR);
        TIME_C("a51_sq_lat",    C_SQ_LAT(fe_sq_a51),         BAR);
        TIME_C("hc_mul_lat",    C_MUL_LAT(fe_mul_native),    BAR);
        TIME_C("hc_sq_lat",     C_SQ_LAT(fe_sq_native),      BAR);

        TIME_C("fiat_mul_nb",   C_MUL_LAT(fe_mul_fiat),      NOBAR);
        TIME_C("fiat_sq_nb",    C_SQ_LAT(fe_sq_fiat),        NOBAR);
        TIME_C("copt_mul_nb",   C_MUL_LAT(fe_mul_cryptopt),  NOBAR);
        TIME_C("copt_sq_nb",    C_SQ_LAT(fe_sq_cryptopt),    NOBAR);
        TIME_C("a51_mul_nb",    C_MUL_LAT(fe_mul_a51),       NOBAR);
        TIME_C("a51_sq_nb",     C_SQ_LAT(fe_sq_a51),         NOBAR);
        TIME_C("hc_mul_nb",     C_MUL_LAT(fe_mul_native),    NOBAR);
        TIME_C("hc_sq_nb",      C_SQ_LAT(fe_sq_native),      NOBAR);

        TIME_C("fiat_mul_tput", C_MUL_TPUT(fe_mul_fiat),     BAR);
        TIME_C("fiat_sq_tput",  C_SQ_TPUT(fe_sq_fiat),       BAR);
        TIME_C("copt_mul_tput", C_MUL_TPUT(fe_mul_cryptopt), BAR);
        TIME_C("copt_sq_tput",  C_SQ_TPUT(fe_sq_cryptopt),   BAR);
        TIME_C("a51_mul_tput",  C_MUL_TPUT(fe_mul_a51),      BAR);
        TIME_C("a51_sq_tput",   C_SQ_TPUT(fe_sq_a51),        BAR);
        TIME_C("hc_mul_tput",   C_MUL_TPUT(fe_mul_native),   BAR);
        TIME_C("hc_sq_tput",    C_SQ_TPUT(fe_sq_native),     BAR);

        /* OpenSSL 5x51 assembly */
        TIME_C("ossl_mul_lat",  C_MUL_LAT(fe_mul_ossl),      BAR);
        TIME_C("ossl_sq_lat",   C_SQ_LAT(fe_sq_ossl),        BAR);
        TIME_C("ossl_mul_nb",   C_MUL_LAT(fe_mul_ossl),      NOBAR);
        TIME_C("ossl_sq_nb",    C_SQ_LAT(fe_sq_ossl),        NOBAR);
        TIME_C("ossl_mul_tput", C_MUL_TPUT(fe_mul_ossl),     BAR);
        TIME_C("ossl_sq_tput",  C_SQ_TPUT(fe_sq_ossl),       BAR);

        /* s2n-bignum, saturated 4x64 */
        TIME_C("s2n_mul_lat",   C4_MUL_LAT(fe_mul_s2n),      BAR);
        TIME_C("s2n_sq_lat",    C4_SQ_LAT(fe_sq_s2n),        BAR);
        TIME_C("s2n_mul_nb",    C4_MUL_LAT(fe_mul_s2n),      NOBAR);
        TIME_C("s2n_sq_nb",     C4_SQ_LAT(fe_sq_s2n),        NOBAR);
        TIME_C("s2n_mul_tput",  C4_MUL_TPUT(fe_mul_s2n),     BAR);
        TIME_C("s2n_sq_tput",   C4_SQ_TPUT(fe_sq_s2n),       BAR);

        /* native invocation floor: call + 10 loads / 5 stores, no arithmetic */
        TIME_C("nfloor_mul",    C_MUL_LAT(fe_mul_nfloor),    BAR);
        TIME_C("nfloor_sq",     C_SQ_LAT(fe_sq_nfloor),      BAR);
    }

    /* ── dispatch floor: same wrapper, 1-triad no-op patch at both hooks ── */
    if (root) {
        install_probe_patches();
        printf("dispatch-floor probe installed: 1 triad (ZEROEXT rdi,rdi) at both hooks\n\n");
        for (int ph = 0; ph < K_PHASES; ph++) {
            init_operands();
            TIME_ASM("floor_mul_lat",  UC_MUL_LAT);
            TIME_ASM("floor_sq_lat",   UC_SQ_LAT);
            TIME_ASM("floor_mul_tput", UC_MUL_TPUT);
            TIME_ASM("floor_sq_tput",  UC_SQ_TPUT);
        }

        /* Restore production patches and re-measure one arm: if it reproduces,
         * the floor numbers were taken under the same clock as the full ones. */
        install_field_patches();
        printf("\n");
        init_operands();
        TIME_ASM("uc_mul_recheck", UC_MUL_LAT);
        if (test_rfc7748()) {
            printf("RFC 7748 FAILED after restoring the patches.\n");
            init_match_and_patch(); do_fix_IN_patch();
            return 1;
        }
    }

    /* ── report ────────────────────────────────────────────────────────── */
    printf("\n");
    printf("=== Table K1 - kernel latency, memory-to-memory dependent chain ===\n");
    printf("  (median cycles per operation; every backend pays the memory round-trip)\n\n");
    printf("  %-38s %8s %8s\n", "backend", "fe_mul", "fe_sq");
    printf("  %-38s %8s %8s\n", "--------------------------------------", "--------", "--------");
    print_row("microcode (this work), full patch", "uc_mul_lat", "uc_sq_lat");
    print_row("fiat-crypto",              "fiat_mul_lat", "fiat_sq_lat");
    print_row("CryptOpt",                 "copt_mul_lat", "copt_sq_lat");
    print_row("amd64-51 asm (Bernstein-Schwabe)", "a51_mul_lat", "a51_sq_lat");
    print_row("hand-written __uint128_t C", "hc_mul_lat",  "hc_sq_lat");
    print_row("OpenSSL fe51 asm",           "ossl_mul_lat", "ossl_sq_lat");
    printf("  %-38s %8s %8s\n", "-- saturated 4x64 (other representation)", "", "");
    print_row("s2n-bignum (verified asm, 4x64)", "s2n_mul_lat", "s2n_sq_lat");

    printf("\n=== Table K2 - where the microcode cycles go ===\n");
    printf("  (median cycles per operation, dependent chain)\n\n");
    printf("  %-38s %8s %8s\n", "layer", "fe_mul", "fe_sq");
    printf("  %-38s %8s %8s\n", "--------------------------------------", "--------", "--------");
    print_row("wrapper instr. cost (issue-bound)", "wrap_mul",      "wrap_sq");
    print_row("wrapper + redirection (1-triad patch)", "floor_mul_lat", "floor_sq_lat");
    print_row("wrapper + redirection + patch body", "uc_mul_lat",    "uc_sq_lat");
    {
        double wm = arm_get("wrap_mul"),      ws = arm_get("wrap_sq");
        double fm = arm_get("floor_mul_lat"), fs = arm_get("floor_sq_lat");
        double tm = arm_get("uc_mul_lat"),    ts = arm_get("uc_sq_lat");
        if (fm >= 0 && tm >= 0) {
            printf("\n  derived:\n");
            printf("  %-38s %8.1f %8.1f\n", "patch body (full - floor)", tm - fm, ts - fs);
            printf("  %-38s %7.1f%% %7.1f%%\n", "wrapper+redirection, share of total",
                   100.0 * fm / tm, 100.0 * fs / ts);
            printf("\n  The wrapper row is issue-bound throughput, measured on disjoint\n"
                   "  slots; in situ it overlaps the firing, so it is context for the\n"
                   "  floor rather than a term to subtract from it. The floor IS the\n"
                   "  wrapper+redirection cost a caller cannot avoid (%.0f%%/%.0f%% of the op).\n",
                   100.0 * fm / tm, 100.0 * fs / ts);
            (void)wm; (void)ws;
            double rc = arm_get("uc_mul_recheck");
            if (rc >= 0) printf("\n  drift check: fe_mul re-measured after restore = %.1f "
                                "(first pass %.1f, %+.2f%%)\n", rc, tm, 100.0 * (rc - tm) / tm);
        }
    }

    printf("\n=== Table K6 - why a firing costs more inside the ladder ===\n");
    printf("  (median cycles per operation; each arm is a dependent chain of\n"
           "   16 ops, same shape as Table K1, differing only as noted)\n\n");
    {
        double um = arm_get("uc_mul_lat"),  us = arm_get("uc_sq_lat");
        double om = arm_get("ossl_mul_lat"), os_ = arm_get("ossl_sq_lat");
        double fm = arm_get("fiat_mul_lat"), fs = arm_get("fiat_sq_lat");
        double ua = arm_get("uc_alt_lat"),  oa = arm_get("ossl_alt_lat");
        double fa = arm_get("fiat_alt_lat"), usc = arm_get("uc_scat_lat");
        printf("  %-40s %9s %9s %9s\n", "arm", "measured", "expected", "excess");
        printf("  %-40s %9s %9s %9s\n",
               "----------------------------------------", "---------", "---------", "---------");
        if (ua > 0 && um > 0)
            printf("  %-40s %9.1f %9.1f %+9.1f\n",
                   "microcode, alternating mul/sq", ua, (um + us) / 2, ua - (um + us) / 2);
        if (oa > 0 && om > 0)
            printf("  %-40s %9.1f %9.1f %+9.1f\n",
                   "OpenSSL, alternating (control)", oa, (om + os_) / 2, oa - (om + os_) / 2);
        if (fa > 0 && fm > 0)
            printf("  %-40s %9.1f %9.1f %+9.1f\n",
                   "fiat-crypto, alternating (control)", fa, (fm + fs) / 2, fa - (fm + fs) / 2);
        if (usc > 0 && um > 0)
            printf("  %-40s %9.1f %9.1f %+9.1f\n",
                   "microcode, 5 field slots not 2", usc, um, usc - um);
        printf("\n  A ladder step issues 9 firings and switches hook 3 times. If the\n"
               "  alternating excess is microcode-specific (large for the first row,\n"
               "  ~0 for the controls) the cost is hook/patch-RAM switching. If the\n"
               "  scatter row carries it instead, it is the working set. Roughly 12\n"
               "  cycles per firing has to be found here to reconcile Table K1 with\n"
               "  the measured ladder.\n");
    }

    printf("\n=== Table K5 - arithmetic only, both floors removed ===\n");
    printf("  (median cycles per operation, dependent chain, invocation cost\n"
           "   subtracted from BOTH sides: microcode loses wrapper+redirection,\n"
           "   the natives lose call + the same 10-load / 5-store round trip)\n\n");
    {
        double fm = arm_get("floor_mul_lat"), fs = arm_get("floor_sq_lat");
        double nm = arm_get("nfloor_mul"),    ns = arm_get("nfloor_sq");
        if (nm < 0 || ns < 0) {
            printf("  native floor arms missing - skipped.\n");
        } else {
            printf("  %-38s %8.1f %8.1f\n", "microcode floor (wrapper+redirect)", fm, fs);
            printf("  %-38s %8.1f %8.1f\n", "native floor (call + mem round trip)", nm, ns);
            printf("\n  %-38s %8s %8s\n", "backend, arithmetic only", "fe_mul", "fe_sq");
            printf("  %-38s %8s %8s\n",
                   "--------------------------------------", "--------", "--------");
            struct { const char *lbl, *m, *s; int uc; } r[] = {
                { "microcode (this work)",   "uc_mul_lat",   "uc_sq_lat",   1 },
                { "OpenSSL fe51 asm",        "ossl_mul_lat", "ossl_sq_lat", 0 },
                { "fiat-crypto",             "fiat_mul_lat", "fiat_sq_lat", 0 },
                { "CryptOpt",                "copt_mul_lat", "copt_sq_lat", 0 },
                { "amd64-51 asm",            "a51_mul_lat",  "a51_sq_lat",  0 },
                { "hand-written C",          "hc_mul_lat",   "hc_sq_lat",   0 },
            };
            double om = -1, os_ = -1;
            for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++) {
                double vm = arm_get(r[i].m), vs = arm_get(r[i].s);
                if (vm < 0 || vs < 0) continue;
                vm -= r[i].uc ? fm : nm;
                vs -= r[i].uc ? fs : ns;
                if (i == 1) { om = vm; os_ = vs; }
                printf("  %-38s %8.1f %8.1f\n", r[i].lbl, vm, vs);
            }
            double um = arm_get("uc_mul_lat") - fm, us = arm_get("uc_sq_lat") - fs;
            if (om > 0)
                printf("\n  microcode arithmetic vs OpenSSL arithmetic: "
                       "mul %.3fx  sq %.3fx  (<1 = microcode faster)\n", um / om, us / os_);
            printf("  CAVEAT: the fe_sq column flatters microcode. FE_SQ's lea/imul\n"
                   "  precompute sits in the microcode floor and is subtracted away,\n"
                   "  while OpenSSL's fe51_sqr keeps its doubling inside the routine.\n"
                   "  fe_mul has no such asymmetry (19*b_j is inside the patch), so the\n"
                   "  fe_mul column is the clean comparison and the one to quote.\n");
        }
    }

    printf("\n=== Table K3 - independent-op throughput (ILP exposed) ===\n");
    printf("  (median cycles per operation, four independent chains)\n\n");
    printf("  %-38s %8s %8s\n", "backend", "fe_mul", "fe_sq");
    printf("  %-38s %8s %8s\n", "--------------------------------------", "--------", "--------");
    print_row("microcode (this work)",      "uc_mul_tput",   "uc_sq_tput");
    print_row("microcode dispatch floor",   "floor_mul_tput","floor_sq_tput");
    print_row("fiat-crypto",                "fiat_mul_tput", "fiat_sq_tput");
    print_row("CryptOpt",                   "copt_mul_tput", "copt_sq_tput");
    print_row("amd64-51 asm",               "a51_mul_tput",  "a51_sq_tput");
    print_row("hand-written __uint128_t C", "hc_mul_tput",   "hc_sq_tput");
    print_row("OpenSSL fe51 asm",           "ossl_mul_tput", "ossl_sq_tput");
    print_row("s2n-bignum (verified asm, 4x64)", "s2n_mul_tput", "s2n_sq_tput");

    printf("\n=== Table K4 - C/asm backends without the memory barrier ===\n");
    printf("  (their best case: compiler may keep limbs in registers)\n\n");
    printf("  %-38s %8s %8s\n", "backend", "fe_mul", "fe_sq");
    printf("  %-38s %8s %8s\n", "--------------------------------------", "--------", "--------");
    print_row("fiat-crypto",                "fiat_mul_nb", "fiat_sq_nb");
    print_row("CryptOpt",                   "copt_mul_nb", "copt_sq_nb");
    print_row("amd64-51 asm",               "a51_mul_nb",  "a51_sq_nb");
    print_row("hand-written __uint128_t C", "hc_mul_nb",   "hc_sq_nb");
    print_row("OpenSSL fe51 asm",           "ossl_mul_nb", "ossl_sq_nb");
    print_row("s2n-bignum (verified asm, 4x64)", "s2n_mul_nb", "s2n_sq_nb");

    /* Machine-readable dump, so the table can be transcribed verbatim. */
    {
        FILE *f = fopen("bench_kernel_out.txt", "w");
        if (f) {
            fprintf(f, "# bench_kernel raw arms: label median_cyc_per_op min_cyc_per_op\n");
            fprintf(f, "# root=%d ops_per_sample=%d samples=%d phases=%d\n",
                    root, K_OPS, K_REPS, K_PHASES);
            for (int i = 0; i < g_narms; i++)
                if (g_arm[i].have)
                    fprintf(f, "%-16s %8.2f %8.2f\n", g_arm[i].label, g_arm[i].med, g_arm[i].mn);
            fclose(f);
            printf("\nraw arms written to bench_kernel_out.txt\n");
        }
    }

    /* Sinks so no chain can be optimised away. */
    volatile uint64_t sink = 0;
    for (int i = 0; i < 5; i++) sink ^= pa[i]^pb[i]^qa[i]^qb[i]^ra[i]^rb[i]^sa[i]^sb[i];
    for (size_t i = 0; i < sizeof(g_st)/8; i++) sink ^= ((uint64_t *)&g_st)[i];
    printf("\n(checksum %llu)\n", (unsigned long long)sink);

    if (root) { init_match_and_patch(); do_fix_IN_patch(); }
    return 0;
}
