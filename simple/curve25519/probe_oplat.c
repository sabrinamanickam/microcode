/*
 * probe_oplat.c — three questions left open by probe_sq_lat / probe_trigger.
 *
 * Where we are. fe_sq costs 122.3 cyc chained but 84.0 independent, and the
 * gap is NOT the trigger (vmread == vmwrite to 0.0 cyc) and NOT the wrapper's
 * LEA/IMUL/RDX instruction mix (all worth ~0.5 cyc). Two facts still need
 * explaining, and one design decision depends on them.
 *
 * PART 1 — micro-op latencies. Every model of this patch has been guesswork
 * because nobody has measured what a microcode op actually costs in a
 * dependent chain. Each arm is a patch that is ONE serial chain from RDI to
 * R15, three ops per triad, driven by the cheap (15.7 cyc) fe_mul wrapper.
 * The chain is long enough that latency, not the 1.61 cyc/triad issue rate,
 * is what is measured:  latency = (cycles - 15.7) / ops.
 * SETCC is the one that matters: it reads flag domain #1, and both the
 * production and the carry-merged-last fe_sq have exactly ONE SETCC on the
 * cross-limb critical path per limb. Five limbs x one SETCC would explain
 * the 25.7 cyc that severing the chain recovers, and would explain why
 * shortening the chain from 9 ops to 5 (sq_fast) recovered almost nothing.
 *
 * PART 2 — is the 45.6 fe_sq "dispatch floor" real, or a probe artifact?
 * With a 1-triad patch the fe_sq wrapper still stores r9, r10 and rbx, which
 * the patch never wrote -- they hold 2*a[2], 2*a[3] and 19*a[4] from THIS
 * iteration's loads. That makes out.limb1 depend on in.limb2, out.limb2 on
 * in.limb3, out.limb3 on in.limb4: a limb-to-limb cascade that exists only
 * because the stub patch does not overwrite those registers. The real fe_sq
 * patch writes all five. The fe_mul stub has no such cascade -- its four
 * other stores come from the fixed Z2 slot. M6 gives the mul wrapper the
 * same cascade; S6 takes it away from the sq wrapper. If they swap places,
 * the 45.6 floor is an artifact and fe_sq's real dispatch cost is ~16.
 *
 * PART 3 — sq_fast with the merge SETCC taken off the cross-limb chain
 * (sourced from TMP9, which is not carry-dependent). That leaves a 3-op,
 * SETCC-free chain: OR -> ADD -> SHR -> OR. Results are WRONG by
 * construction; this prices the correct restructure before I write it.
 * If it lands near sq_cut's 96.6, the SETCC-free scheme is worth building.
 *
 * Build: make PROG=probe_oplat
 * Run:   sudo taskset -c 0 ./probe_oplat_static
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

/* fe_mul wrapper (cheap floor); STORES parameterises the five result stores */
#define MUL_W(out, a, b, STORES) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  "mov rsi, [rbp + " S(a) " + 8]\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t" "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t" "mov r15, [rbp + " S(b) " + 0]\n\t" \
    "mov r13, [rbp + " S(b) " + 8]\n\t"  "mov r9,  [rbp + " S(b) " + 16]\n\t" \
    "mov r10, [rbp + " S(b) " + 24]\n\t" "mov rbx, [rbp + " S(b) " + 32]\n\t" \
    "xor eax, eax\n\t" "xor r8d, r8d\n\t" TRIG_VMW STORES

#define ST_MUL_STD(out) \
    "mov [rbp + " S(out) " + 0],  r15\n\t" "mov [rbp + " S(out) " + 8],  r13\n\t" \
    "mov [rbp + " S(out) " + 16], r9\n\t"  "mov [rbp + " S(out) " + 24], r10\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"
/* same cascade shape as fe_sq: out.limb1<-in.limb2, limb2<-in.limb3, limb3<-in.limb4 */
#define ST_MUL_CASC(out) \
    "mov [rbp + " S(out) " + 0],  r15\n\t" "mov [rbp + " S(out) " + 8],  r12\n\t" \
    "mov [rbp + " S(out) " + 16], r11\n\t" "mov [rbp + " S(out) " + 24], r14\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

#define SQ_W(out, a, STORES) \
    "mov r14, [rbp + " S(a) " + 32]\n\t" "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r12, [rbp + " S(a) " + 16]\n\t" "mov rsi, [rbp + " S(a) " + 8]\n\t" \
    "mov rdi, [rbp + " S(a) " + 0]\n\t" \
    "lea r15, [rdi + rdi]\n\t" "lea r13, [rsi + rsi]\n\t" \
    "lea r9,  [r12 + r12]\n\t" "lea r10, [r11 + r11]\n\t" \
    "imul rbx, r14, 19\n\t"    "imul rdx, r11, 19\n\t" \
    "xor eax, eax\n\t" "xor r8d, r8d\n\t" TRIG_VMR STORES

#define ST_SQ_STD(out) \
    "mov [rbp + " S(out) " + 0],  rdi\n\t" "mov [rbp + " S(out) " + 8],  r9\n\t" \
    "mov [rbp + " S(out) " + 16], r10\n\t" "mov [rbp + " S(out) " + 24], rbx\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"
/* cascade removed: limbs 1..3 store a constant, so only limb0 is chained */
#define ST_SQ_NOCASC(out) \
    "mov [rbp + " S(out) " + 0],  rdi\n\t" "mov [rbp + " S(out) " + 8],  rax\n\t" \
    "mov [rbp + " S(out) " + 16], rax\n\t" "mov [rbp + " S(out) " + 24], rax\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"

static ladder_state_t g_st;
static void init_operands(void) {
    uint64_t *p = (uint64_t *)&g_st;
    for (size_t i = 0; i < sizeof(g_st) / 8; i++)
        p[i] = (0x123456789ABCDULL * (i + 1)) & MASK51;
}

#define MUL_ARM(ST) REP8(MUL_W(B_OFF, A_OFF, Z2_OFF, ST(B_OFF)) MUL_W(A_OFF, B_OFF, Z2_OFF, ST(A_OFF)))
#define SQ_ARM(ST)  REP8(SQ_W(B_OFF, A_OFF, ST(B_OFF))          SQ_W(A_OFF, B_OFF, ST(A_OFF)))

#define MAX_ARMS 40
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

/* ── PART 1: serial-chain latency patches ──────────────────────────────
 * NT triads, 3 ops each, all on one chain TMP0 -> ... -> TMP0, bracketed by
 * ZEROEXT(TMP0,RDI) at the top and ZEROEXT(R15,TMP0) at the bottom.        */
#define NT 30
enum { K_ADD, K_OR, K_AND, K_SHL, K_SHR, K_ZX, K_MUL, K_SETCC, K_NKINDS };
static const char *kind_name[K_NKINDS] =
    { "ADD", "OR", "AND", "SHL", "SHR", "ZEROEXT", "MUL", "ADD+SETCC+ADD" };
/* dependent ops per patch, excluding the two bracket ZEROEXTs */
static const int kind_ops[K_NKINDS] =
    { 3*NT, 3*NT, 3*NT, 3*NT, 3*NT, 3*NT, 3*NT, 3*NT };

static ucode_t chain_buf[NT + 2];

static int build_chain(int kind) {
    int n = 0;
    chain_buf[n].uop0 = ZEROEXT_DSZ64_DR(TMP0, RDI);
    chain_buf[n].uop1 = ZEROEXT_DSZ64_DR(TMP9, RDI);   /* flag donor, off-chain */
    chain_buf[n].uop2 = ADD_DSZ64_DRR(TMP9, TMP9, RDI);
    chain_buf[n].seqw = NOP_SEQWORD; n++;
    for (int t = 0; t < NT; t++) {
        uint64_t a, b, c;
        switch (kind) {
        case K_ADD: a = b = c = ADD_DSZ64_DRR(TMP0, TMP0, RDI); break;
        case K_OR:  a = b = c = OR_DSZ64_DRR(TMP0, TMP0, RDI);  break;
        case K_AND: a = b = c = AND_DSZ64_DRR(TMP0, TMP0, RDI); break;
        case K_SHL: a = b = c = SHL_DSZ64_DRI(TMP0, TMP0, 1);   break;
        case K_SHR: a = b = c = SHR_DSZ64_DRI(TMP0, TMP0, 1);   break;
        case K_ZX:  a = b = c = ZEROEXT_DSZ64_DR(TMP0, TMP0);   break;
        /* MUL_DSZ64_DRR(hi, srcA, srcB): srcB receives the low half, so the
         * chain runs through TMP0 with RDI as the preserved multiplier. */
        case K_MUL: a = b = c = MUL_DSZ64_DRR(RCX, RDI, TMP0);  break;
        case K_SETCC:
        default:
            a = ADD_DSZ64_DRR(TMP0, TMP0, RDI);
            b = SETCC_CONDB_DR(TMP15, TMP0);
            c = ADD_DSZ64_DRR(TMP0, TMP0, TMP15);
            break;
        }
        chain_buf[n].uop0 = a; chain_buf[n].uop1 = b; chain_buf[n].uop2 = c;
        chain_buf[n].seqw = NOP_SEQWORD; n++;
    }
    chain_buf[n].uop0 = ZEROEXT_DSZ64_DR(R15, TMP0);
    chain_buf[n].uop1 = NOP; chain_buf[n].uop2 = NOP;
    chain_buf[n].seqw = END_SEQWORD; n++;
    return n;
}

/* ── PART 3: sq_fast with the merge SETCC pulled off the chain ────────── */
static ucode_t sq_fast_ns[ARRAY_SZ(sq_fast_src)];
static const int merge_idx[4] = { 12, 20, 28, 36 };
static int build_sq_fast_ns(void) {
    memcpy(sq_fast_ns, sq_fast_src, sizeof(sq_fast_src));
    int patched = 0;
    for (int i = 0; i < 4; i++) {
        int t = merge_idx[i];
        if (sq_fast_ns[t].uop0 != ADD_DSZ64_DRR(TMP0, TMP0, TMP2) ||
            sq_fast_ns[t].uop1 != SETCC_CONDB_DR(TMP15, TMP0)) return -1;
        sq_fast_ns[t].uop1 = SETCC_CONDB_DR(TMP15, TMP9);   /* off-chain source */
        patched++;
    }
    return patched;
}

static ucode_t stub_sq[]  = { { ZEROEXT_DSZ64_DR(RDI, RDI), NOP, NOP, END_SEQWORD } };
static ucode_t stub_mul[] = { { ZEROEXT_DSZ64_DR(R15, RDI), NOP, NOP, END_SEQWORD } };

int main(void) {
    if (geteuid() != 0) { printf("needs root\n"); return 1; }
    printf("=== probe_oplat ===\n\n");
    assign_to_core(0);
    init_operands();
    init_match_and_patch();
    do_fix_IN_patch();

    /* ---- PART 2: store-cascade artifact ---- */
    patch_ucode(0x7c00, stub_mul, 1); hook_match_and_patch(0, 0x0cd8, 0x7c00);
    patch_ucode(0x7c04, stub_sq,  1); hook_match_and_patch(1, 0x0618, 0x7c04);
    for (int ph = 0; ph < K_PHASES; ph++) {
        init_operands();
        TIME_ASM("mul wrapper, stores from fixed slot", MUL_ARM(ST_MUL_STD));
        TIME_ASM("mul wrapper, + limb cascade",         MUL_ARM(ST_MUL_CASC));
        TIME_ASM("sq  wrapper, stores as production",   SQ_ARM(ST_SQ_STD));
        TIME_ASM("sq  wrapper, cascade removed",        SQ_ARM(ST_SQ_NOCASC));
    }

    /* ---- PART 1: op latencies ---- */
    for (int ph = 0; ph < K_PHASES; ph++) {
        for (int k = 0; k < K_NKINDS; k++) {
            int n = build_chain(k);
            patch_ucode(0x7c00, chain_buf, n);
            hook_match_and_patch(0, 0x0cd8, 0x7c00);
            init_operands();
            TIME_ASM(kind_name[k], MUL_ARM(ST_MUL_STD));
        }
    }

    /* ---- PART 3 ---- */
    int np = build_sq_fast_ns();
    if (np != 4) { printf("sq_fast_ns: merge triad indices wrong (%d) - skipping\n", np); }
    else {
        patch_ucode(0x7d08, sq_fast_ns, (int)ARRAY_SZ(sq_fast_ns));
        hook_match_and_patch(1, 0x0618, 0x7d08);
        for (int ph = 0; ph < K_PHASES; ph++) {
            init_operands();
            TIME_ASM("sq_fast, SETCC off the chain", SQ_ARM(ST_SQ_STD));
        }
    }

    printf("--- PART 2: is the 45.6 fe_sq dispatch floor an artifact? ---\n");
    printf("  %-40s %8s\n", "arm (1-triad stub patch)", "cyc/op");
    const char *p2[] = { "mul wrapper, stores from fixed slot", "mul wrapper, + limb cascade",
                         "sq  wrapper, stores as production",   "sq  wrapper, cascade removed", NULL };
    for (int i = 0; p2[i]; i++) printf("  %-40s %8.1f\n", p2[i], arm_get(p2[i]));

    printf("\n--- PART 1: micro-op latency in a dependent chain ---\n");
    printf("  (%d triads, %d dependent ops; latency = (cyc - floor) / ops)\n", NT, 3*NT);
    double floor_mul = arm_get("mul wrapper, stores from fixed slot");
    printf("  %-16s %9s %9s\n", "op", "cyc/op", "latency");
    for (int k = 0; k < K_NKINDS; k++) {
        double t = arm_get(kind_name[k]);
        printf("  %-16s %9.1f %9.2f\n", kind_name[k], t, (t - floor_mul) / kind_ops[k]);
    }
    {
        double add = (arm_get("ADD") - floor_mul) / (3*NT);
        double sc  = (arm_get("ADD+SETCC+ADD") - floor_mul) / NT;
        printf("\n  per {ADD,SETCC,ADD} group: %.2f cyc, of which 2 ADDs = %.2f\n", sc, 2*add);
        printf("  => SETCC latency ~= %.2f cyc\n", sc - 2*add);
        printf("     five limbs x one SETCC on the carry chain = %.1f cyc\n", 5*(sc - 2*add));
        printf("     (severing the chain entirely recovered 25.7)\n");
    }

    printf("\n--- PART 3: pricing a SETCC-free carry chain ---\n");
    printf("  %-40s %8.1f\n", "sq_fast, SETCC off the chain", arm_get("sq_fast, SETCC off the chain"));
    printf("  reference: sq_prod 122.3, sq_fast 120.3, sq_cut 96.6, sq tput 84.0\n");

    init_match_and_patch(); do_fix_IN_patch();
    return 0;
}
