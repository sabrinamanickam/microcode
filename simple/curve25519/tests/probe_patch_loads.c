/*
 * probe_patch_loads.c — can a firing load its own operands cheaper than the
 * caller's native loads?
 *
 * probe_glue_ablation found native loads issued just before a firing cost
 * ~1.07 cyc each and are not hidden. fe_mul's caller issues 10 of them. This
 * probe hands the patch two pointers instead and lets it LDZX the 10 limbs
 * itself, then runs the unchanged production fe_mul body.
 *
 * Arms (all fe_mul, dependent chain out == a, same process, re-hooked between
 * arms; the production patches are never modified):
 *   A   production FE_MUL: 10 native loads, xor eax, xor r8d, mov rcx, vmwrite
 *   A2  A without the two xors (the patch writes RAX and R8 before reading
 *       them, so they are dead on hardware) -- the dead-instruction trim
 *   C   A2, but vmwrite enters a 1-triad stub that SEQ_GOTO0s to fe_mul:
 *       the control for the goto the load prologue also pays
 *   B   lea rdi=&a, lea r15=&b, mov rcx, vmwrite -> 10-triad LDZX prologue
 *       (one load per triad, slot 0, NOPs elsewhere -- exactly the form the
 *       Keccak patch ships) -> SEQ_GOTO0 -> production fe_mul
 * B-C is native loads vs in-patch loads with everything else equal; B-A is
 * the end-to-end answer (a real build would fall through instead of goto).
 * An independent-input run of every arm is printed too.
 *
 * Safety: LDZX_DSZ64_ASZ32 is the proven encoding (32-bit addressing), so the
 * operands live in a global buffer and main() refuses to fire unless it sits
 * below 4 GB (binary is non-PIE). The ASZ64 form is unverified: not used.
 * Base registers are RDI/R15 (Keccak used RCX); RCX/RDX, the trigger's own
 * operands, keep their production roles. One memory op per triad only --
 * two per triad hard-crashed the machine.
 * Patch RAM: prologue U7d90-U7dbb, stub U7dc0, below the U7de0 staging area.
 *
 * Build: make PROG=tests/probe_patch_loads
 * Run:   sudo taskset -c 0 ./tests/probe_patch_loads_static
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"

#define MUL_ADDR  0x7c00
#define PRO_ADDR  0x7d90            /* after fe_sq: 0x7ce8 + 42*4 */
#define STUB_ADDR 0x7dc0

static void install_prologue(void) {
    ucode_t p[] = {
        /* b first: fe_mul's PREP reads b4, b3, b2, b1; row 0 reads b0, a0 */
        { LDZX_DSZ64_ASZ32_SC1_DRI(RBX, R15, 32, SEG_DS), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(R10, R15, 24, SEG_DS), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(R9,  R15, 16, SEG_DS), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(R13, R15,  8, SEG_DS), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(R15, R15,  0, SEG_DS), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(RSI, RDI,  8, SEG_DS), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(R12, RDI, 16, SEG_DS), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(R11, RDI, 24, SEG_DS), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(R14, RDI, 32, SEG_DS), NOP, NOP, NOP_SEQWORD },
        { LDZX_DSZ64_ASZ32_SC1_DRI(RDI, RDI,  0, SEG_DS), NOP, NOP, NOP_SEQWORD },
        { NOP, NOP, NOP, SEQ_GOTO0(MUL_ADDR) },
    };
    ucode_t s[] = { { NOP, NOP, NOP, SEQ_GOTO0(MUL_ADDR) } };
    patch_ucode(PRO_ADDR, p, ARRAY_SZ(p));
    patch_ucode(STUB_ADDR, s, ARRAY_SZ(s));
    printf("[install] load prologue %d triads at U%04x, goto stub at U%04x\n",
           (int)ARRAY_SZ(p), PRO_ADDR, STUB_ADDR);
}
static void hook_mul(uint64_t addr) { hook_match_and_patch(0, 0x0cd8, addr); }

#define MUL_STORE(out) \
    "mov [rbp + " S(out) " + 0],  r15\n\t" \
    "mov [rbp + " S(out) " + 8],  r13\n\t" \
    "mov [rbp + " S(out) " + 16], r9\n\t"  \
    "mov [rbp + " S(out) " + 24], r10\n\t" \
    "mov [rbp + " S(out) " + 32], rax\n\t"
#define MUL_LOAD(a, b) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  \
    "mov rsi, [rbp + " S(a) " + 8]\n\t"  \
    "mov r12, [rbp + " S(a) " + 16]\n\t" \
    "mov r11, [rbp + " S(a) " + 24]\n\t" \
    "mov r14, [rbp + " S(a) " + 32]\n\t" \
    "mov r15, [rbp + " S(b) " + 0]\n\t"  \
    "mov r13, [rbp + " S(b) " + 8]\n\t"  \
    "mov r9,  [rbp + " S(b) " + 16]\n\t" \
    "mov r10, [rbp + " S(b) " + 24]\n\t" \
    "mov rbx, [rbp + " S(b) " + 32]\n\t"
#define FIRE "mov rcx, 0x7FFFFFFFFFFFF\n\t" "vmwrite rcx, rdx\n\t"

#define ARM_A(o, a, b)  FE_MUL(o, a, b)
#define ARM_A2(o, a, b) MUL_LOAD(a, b) FIRE MUL_STORE(o)     /* also arm C */
#define ARM_B(o, a, b) \
    "lea rdi, [rbp + " S(a) "]\n\t" "lea r15, [rbp + " S(b) "]\n\t" FIRE MUL_STORE(o)

#define PCLOB "rax","rbx","rcx","rdx","rsi","rdi", \
              "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"
#define REP2(x)  x x
#define REP4(x)  REP2(x) REP2(x)
#define REP8(x)  REP4(x) REP4(x)
#define REP16(x) REP8(x) REP8(x)

static ladder_state_t g_buf __attribute__((aligned(64)));

/* dependent: DA = DA * A ; independent: CB = X1 * X2 */
#define KERNELS(N, ARM) \
static void N##_dep(void)   { register ladder_state_t *p asm("rbp") = &g_buf; \
    asm volatile(REP16(ARM(DA_OFF, DA_OFF, A_OFF)) : : "r"(p) : PCLOB); }       \
static void N##_ind(void)   { register ladder_state_t *p asm("rbp") = &g_buf; \
    asm volatile(REP16(ARM(CB_OFF, X1_OFF, X2_OFF)) : : "r"(p) : PCLOB); }      \
static void N##_once(void)  { register ladder_state_t *p asm("rbp") = &g_buf; \
    asm volatile(ARM(CB_OFF, X1_OFF, X2_OFF) : : "r"(p) : PCLOB); }
KERNELS(a,  ARM_A)
KERNELS(a2, ARM_A2)
KERNELS(b,  ARM_B)

static void buf_init(void) {
    uint64_t *p = (uint64_t *)&g_buf;
    for (size_t i = 0; i < sizeof(g_buf) / 8; i++)
        p[i] = (0x123456789ABCDULL * (i + 1)) & MASK51;
}

#define REPS   1000
#define TRIALS 50
static double time_k(void (*f)(void)) {
    uint64_t best = ~0ULL;
    for (int t = 0; t < TRIALS; t++) {
        buf_init();
        uint64_t s = rdtsc_start();
        for (int r = 0; r < REPS; r++) f();
        uint64_t c = rdtsc_end() - s;
        if (c < best) best = c;
    }
    return (double)best / (REPS * 16);
}

static int rfc1(void) {
    uint8_t sc[32], pt[32], r[32];
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", sc, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", pt, 32);
    x25519(r, sc, pt);
    return !memcmp_hex(r, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32);
}

enum { A, A2, C, B, NARM };
static const char *arm_name[NARM] = {
    "A  production FE_MUL", "A2 minus xor eax/r8d",
    "C  A2 + goto stub", "B  in-patch loads + goto" };

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== probe_patch_loads: fe_mul operands loaded by the patch ===\n\n");
    if ((uintptr_t)&g_buf + sizeof g_buf >= (1ULL << 32)) {
        printf("operand buffer at %p is above 4 GB; ASZ32 loads would truncate. abort\n",
               (void *)&g_buf);
        return 1;
    }
    printf("operand buffer at %p (below 4 GB, ASZ32 OK)\n", (void *)&g_buf);
    if (freq_guard()) return 2;
    bench_pin();
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();
    install_prologue();
    if (!rfc1()) { printf("RFC 7748 vector 1 FAILED; abort\n"); goto out; }
    printf("RFC 7748 vector 1 OK\n");

    /* correctness: one independent firing per arm must match arm A */
    uint64_t ref[5], got[5];
    buf_init(); hook_mul(MUL_ADDR);  a_once();  memcpy(ref, g_buf.CB, 40);
    buf_init(); hook_mul(MUL_ADDR);  a2_once(); memcpy(got, g_buf.CB, 40);
    int ok_a2 = !memcmp(ref, got, 40);
    buf_init(); hook_mul(STUB_ADDR); a2_once(); memcpy(got, g_buf.CB, 40);
    int ok_c = !memcmp(ref, got, 40);
    printf("[check] A2 %s, C %s\n", ok_a2 ? "OK" : "MISMATCH", ok_c ? "OK" : "MISMATCH");
    if (!ok_a2 || !ok_c) goto out;
    printf("[check] B: first in-patch-load firing ... ");
    buf_init(); hook_mul(PRO_ADDR);  b_once();  memcpy(got, g_buf.CB, 40);
    int ok_b = !memcmp(ref, got, 40);
    printf("%s\n", ok_b ? "OK" : "MISMATCH");
    if (!ok_b) goto out;
    /* dependent-chain agreement after 16 chained ops */
    uint64_t d_ref[5], d_got[5];
    buf_init(); hook_mul(MUL_ADDR); a_dep(); memcpy(d_ref, g_buf.DA, 40);
    buf_init(); hook_mul(PRO_ADDR); b_dep(); memcpy(d_got, g_buf.DA, 40);
    printf("[check] B dependent chain x16 %s\n", memcmp(d_ref, d_got, 40) ? "MISMATCH" : "OK");
    if (memcmp(d_ref, d_got, 40)) goto out;

    uint64_t hook[NARM] = { MUL_ADDR, MUL_ADDR, STUB_ADDR, PRO_ADDR };
    void (*dep[NARM])(void) = { a_dep, a2_dep, a2_dep, b_dep };
    void (*ind[NARM])(void) = { a_ind, a2_ind, a2_ind, b_ind };
    double cd[3][NARM], ci[3][NARM];
    for (int r = 0; r < 3; r++)
        for (int v = 0; v < NARM; v++) {
            hook_mul(hook[v]);
            cd[r][v] = time_k(dep[v]);
            ci[r][v] = time_k(ind[v]);
        }
    hook_mul(MUL_ADDR);

    printf("\n-- fe_mul cyc/op (TSC, min of %d x %d), 3 interleaved rounds --\n",
           TRIALS, REPS * 16);
    printf("  %-26s %24s  %7s   %s\n", "arm", "dependent r0/r1/r2", "best", "independent best");
    double bd[NARM], bi[NARM];
    for (int v = 0; v < NARM; v++) {
        bd[v] = cd[0][v]; bi[v] = ci[0][v];
        for (int r = 1; r < 3; r++) {
            if (cd[r][v] < bd[v]) bd[v] = cd[r][v];
            if (ci[r][v] < bi[v]) bi[v] = ci[r][v];
        }
        printf("  %-26s %7.2f %7.2f %7.2f  %7.2f   %7.2f\n", arm_name[v],
               cd[0][v], cd[1][v], cd[2][v], bd[v], bi[v]);
    }
    printf("\n  dead-xor trim   A -A2 : %+6.2f cyc/mul  (x2561 firings ~ %+.0f/X25519 if sq matches)\n",
           bd[A] - bd[A2], 2561 * (bd[A] - bd[A2]));
    printf("  goto cost       C -A2 : %+6.2f cyc/mul\n", bd[C] - bd[A2]);
    printf("  in-patch loads  C -B  : %+6.2f cyc/mul saved vs native loads (same goto)\n", bd[C] - bd[B]);
    printf("  end to end      A -B  : %+6.2f cyc/mul  (x1287 muls = %+.0f/X25519)\n",
           bd[A] - bd[B], 1287 * (bd[A] - bd[B]));
    printf("  target to reach OpenSSL: ~13k/X25519 total\n");

out:
    hook_mul(MUL_ADDR);
    {
        int ok = rfc1();
        printf("\nRFC 7748 vector 1 at exit: %s\n", ok ? "OK" : "FAIL");
    }
    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
