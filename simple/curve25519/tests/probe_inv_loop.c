/*
 * probe_inv_loop.c — does LOOPED squaring beat per-firing squaring in fe_invert?
 *
 * Production fe_invert (full_curve25519_inline2.c) issues its 254 squarings as
 * 254 separate vmread firings, register-chained (INV_SQ_RENAME, no memory in
 * between). Each firing pays the trigger/drain overhead. This probe replaces
 * every squaring run (1,2,1,5,10,20,10,50,100,50,5) with ONE firing that loops
 * the production fe_sq body n times inside the patch, so fe_invert drops from
 * 265 firings to 22 (11 sq runs + 11 muls).
 *
 * Loop patch (51 triads, installed over the fe_sq region, fe_mul untouched):
 *   T0      TMP12 = n (from R8), TMP14 = 0
 *   R1..R5  re-prep: h in (rdi,r9,r10,rbx,rax) -> sq input form
 *           (a, 2a, 19a4, 19a3, rax=r8=0), TMP14 += 1. R1 = loop top.
 *   B0..B41 production fe_sq body, END triad re-emitted as NOP_SEQWORD
 *   X       XOR TMP13 = TMP14 ^ TMP12 ; UJMPCC CONDZ -> EXIT   (forward)
 *   G       SEQ_GOTO0(R1)                                       (backward)
 *   EXIT    END
 * Loop idiom is exactly probe_looped_fieldop's (forward XOR+UJMPCC exit,
 * SEQ_GOTO0 back to a benign non-MUL triad); backward UJMPCC crashes the core.
 * Caller: load limbs into (rdi,r9,r10,rbx,rax), r8 = n >= 1, fire vmread.
 * Output lands in (rdi,r9,r10,rbx,rax), the same as the production fe_sq.
 *
 * !! While the loop patch is installed, the vmread hook no longer speaks the
 * !! production FE_SQ convention: with r8 = 0 it would loop ~2^64 times. Never
 * !! call x25519/ladder_step/fe_invert in that window. main() reinstalls the
 * !! production patches before touching them again.
 *
 * Build: make PROG=tests/probe_inv_loop EXTRA_CPPFLAGS="-DSQ_SERIAL"
 * Run:   sudo taskset -c 0 ./tests/probe_inv_loop_static
 *
 * SERIAL fe_sq ONLY. The "production" arm register-chains squarings
 * (INV_SQ_RENAME), which hard-resets the machine with the five-accumulator
 * fe_sq that is now the default, so this refuses to build without SQ_SERIAL.
 */
#ifndef SQ_SERIAL
#error "probe_inv_loop chains fe_sq firings: build with EXTRA_CPPFLAGS=-DSQ_SERIAL"
#endif
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"

#define MUL_TRIADS 58                       /* production fe_mul size */
#define LSQ_ADDR   (0x7c00 + MUL_TRIADS * 4)

/* production fe_sq triads 0..40 (copied from install_field_patches);
 * triad 41 is the END triad and is re-emitted by build_loop_sq(). */
static const ucode_t sq_body[] = {
    /* c0 */
    { ZEROEXT_DSZ64_DR(TMP0, RAX), MUL_DSZ64_DRR(RCX, RDI, RDI),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDI), SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP9, TMP15), MUL_DSZ64_DRR(RCX, RBX, R13),
      ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RDX, R9), ADD_DSZ64_DRR(TMP0, TMP0, R9),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(RDI, TMP6, 13), ZEROEXT_DSZ64_DR(R13, RSI),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    /* c1 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, R13),
      ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(R9, R12, R12), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP9, TMP15), MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RBX, R9), ADD_DSZ64_DRR(TMP0, TMP0, R9),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(R9, TMP6, 13), ZEROEXT_DSZ64_DR(RDX, R12),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    /* c2 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, RDX),
      ADD_DSZ64_DRR(TMP0, TMP0, RDX), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ZEROEXT_DSZ64_DR(R13, RSI), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP9, TMP15), MUL_DSZ64_DRR(RCX, RSI, R13),
      ADD_DSZ64_DRR(TMP0, TMP0, R13), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, RBX, R10), ADD_DSZ64_DRR(TMP0, TMP0, R10),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHL_DSZ64_DRI(TMP6, TMP0, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(R10, TMP6, 13), ZEROEXT_DSZ64_DR(RDX, R11),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    /* c3 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, RDX),
      ADD_DSZ64_DRR(R13, RSI, RSI), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RAX, R12), MUL_DSZ64_DRR(RCX, R13, RAX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RAX), SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, R14, RBX), ADD_DSZ64_DRR(TMP0, TMP0, RBX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(TMP8, TMP0, 51),
      SHL_DSZ64_DRI(TMP6, TMP0, 13), NOP_SEQWORD },
    { SHR_DSZ64_DRI(RBX, TMP6, 13), SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8), NOP_SEQWORD },
    /* c4 */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DRR(RCX, R15, R14),
      ADD_DSZ64_DRR(TMP0, TMP0, R14), NOP_SEQWORD },
    { SETCC_CONDB_DR(TMP15, TMP0), ADD_DSZ64_DRR(R8, R8, RCX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { MUL_DSZ64_DRR(RCX, R13, R11), ADD_DSZ64_DRR(TMP0, TMP0, R11),
      SETCC_CONDB_DR(TMP15, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      MUL_DSZ64_DRR(RCX, R12, R12), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, R12), SETCC_CONDB_DR(TMP15, TMP0),
      ADD_DSZ64_DRR(R8, R8, RCX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHR_DSZ64_DRI(TMP8, TMP0, 51),
      ADD_DSZ64_DRR(R8, R8, TMP9), NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP6, TMP0, 13), SHL_DSZ64_DRI(TMP1, R8, 13),
      SHR_DSZ64_DRI(RAX, TMP6, 13), NOP_SEQWORD },
    /* final reduction */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DIR(TMP6, 19, TMP0),
      ADD_DSZ64_DRR(RDI, RDI, TMP0), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, RDI, 51), ADD_DSZ64_DRR(R9, R9, TMP0),
      NOP, NOP_SEQWORD },
};
#define SQ_BODY_N ((int)(sizeof(sq_body) / sizeof(sq_body[0])))

static ucode_t lsq[64];

static int build_loop_sq(void) {
    int n = 0;
#define A(i) (LSQ_ADDR + (i) * 4)
    lsq[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(TMP12, R8), ZEROEXT_DSZ32_DI(TMP14, 0),
                          NOP, NOP_SEQWORD };
    int top = n;
    /* re-prep; sequential semantics inside a triad (test_raw_war_waw) */
    lsq[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(RSI, R9), ZEROEXT_DSZ64_DR(R12, R10),
                          ZEROEXT_DSZ64_DR(R11, RBX), NOP_SEQWORD };        /* a1..a3 */
    lsq[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(R14, RAX), ZEROEXT_DSZ64_DR(RDX, RBX),
                          MUL_DSZ64_DIR(RCX, 19, RDX), NOP_SEQWORD };       /* a4, rdx=19a3 */
    lsq[n++] = (ucode_t){ ZEROEXT_DSZ64_DR(RBX, RAX), MUL_DSZ64_DIR(RCX, 19, RBX),
                          ADD_DSZ64_DRR(R15, RDI, RDI), NOP_SEQWORD };      /* rbx=19a4, r15=2a0 */
    lsq[n++] = (ucode_t){ ADD_DSZ64_DRR(R13, RSI, RSI), ADD_DSZ64_DRR(R9, R12, R12),
                          ADD_DSZ64_DRR(R10, R11, R11), NOP_SEQWORD };      /* 2a1 2a2 2a3 */
    lsq[n++] = (ucode_t){ ZEROEXT_DSZ32_DI(RAX, 0), ZEROEXT_DSZ32_DI(R8, 0),
                          ADD_DSZ64_DRI(TMP14, TMP14, 1), NOP_SEQWORD };
    for (int i = 0; i < SQ_BODY_N; i++) lsq[n++] = sq_body[i];
    lsq[n++] = (ucode_t){ SHL_DSZ64_DRI(TMP6, RDI, 13), SHR_DSZ64_DRI(RDI, TMP6, 13),
                          NOP, NOP_SEQWORD };
    int exit_idx = n + 2;
    lsq[n++] = (ucode_t){ XOR_DSZ64_DRR(TMP13, TMP14, TMP12), NOP,
                          UJMPCC_DIRECT_NOTTAKEN_CONDZ_RI(TMP13, A(exit_idx)), NOP_SEQWORD };
    lsq[n++] = (ucode_t){ NOP, NOP, NOP, SEQ_GOTO0(A(top)) };
    lsq[n++] = (ucode_t){ NOP, NOP, NOP, END_SEQWORD };
#undef A
    return n;
}

static void install_loop_sq(void) {
    int n = build_loop_sq();
    if (MUL_TRIADS + n > 120) { printf("loop patch too big (%d)\n", n); exit(1); }
    patch_ucode(LSQ_ADDR, lsq, n);
    hook_match_and_patch(1, 0x0618, LSQ_ADDR);
    printf("[install] loop-sq: %d triads at U%04x (vmread hook), ends U%04x\n",
           n, LSQ_ADDR, LSQ_ADDR + n * 4);
}

static void install_production(void) {
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();
}

/* ── loop-sq caller: a -> dst, n squarings, one firing ─────────────── */
#define LSQ(dst, a, n) \
    "mov rdi, [rbp + " S(a) " + 0]\n\t"  \
    "mov r9,  [rbp + " S(a) " + 8]\n\t"  \
    "mov r10, [rbp + " S(a) " + 16]\n\t" \
    "mov rbx, [rbp + " S(a) " + 24]\n\t" \
    "mov rax, [rbp + " S(a) " + 32]\n\t" \
    "mov r8d, " #n "\n\t"                \
    ".byte 0x0f, 0x78, 0xca\n\t"         \
    INV_SQ_STORE(dst)

#define CLOB "rax","rbx","rcx","rdx","rsi","rdi", \
             "r8","r9","r10","r11","r12","r13","r14","r15","memory","cc"

/* Same addition chain as production fe_invert, squarings looped. */
static void fe_invert_loop(uint64_t out[5], const uint64_t z[5]) {
    invert_state_t st;
    memcpy(st.z, z, 40);
    register invert_state_t *_st asm("rbp") = &st;
    asm volatile(
        LSQ(IZ2_OFF, IZ_OFF, 1)
        LSQ(IT_OFF, IZ2_OFF, 2)
        INV_MUL(IZ9_OFF, IT_OFF, IZ_OFF)
        INV_MUL(IZ11_OFF, IZ9_OFF, IZ2_OFF)
        LSQ(IT_OFF, IZ11_OFF, 1)
        INV_MUL(IT0_OFF, IT_OFF, IZ9_OFF)
        LSQ(IT1_OFF, IT0_OFF, 5)
        INV_MUL(IT1_OFF, IT1_OFF, IT0_OFF)
        LSQ(IT2_OFF, IT1_OFF, 10)
        INV_MUL(IT2_OFF, IT2_OFF, IT1_OFF)
        LSQ(IT3_OFF, IT2_OFF, 20)
        INV_MUL(IT3_OFF, IT3_OFF, IT2_OFF)
        LSQ(IT3_OFF, IT3_OFF, 10)
        INV_MUL(IT1_OFF, IT3_OFF, IT1_OFF)
        LSQ(IT2_OFF, IT1_OFF, 50)
        INV_MUL(IT2_OFF, IT2_OFF, IT1_OFF)
        LSQ(IT3_OFF, IT2_OFF, 100)
        INV_MUL(IT3_OFF, IT3_OFF, IT2_OFF)
        LSQ(IT3_OFF, IT3_OFF, 50)
        INV_MUL(IT1_OFF, IT3_OFF, IT1_OFF)
        LSQ(IT1_OFF, IT1_OFF, 5)
        INV_MUL(IT1_OFF, IT1_OFF, IZ11_OFF)
        : : "r"(_st) : CLOB);
    memcpy(out, st.t1, 40);
}

/* ── n-squaring kernels on one slot (offset 0 of a 5-limb buffer) ──── */
/* production: n register-chained firings (exactly INV_RUN_CHAINED's shape) */
#define PROD_RUN(rep) INV_SQ_LOAD(0) INV_SQ_OP ".rept " #rep "\n\t" \
                      INV_SQ_RENAME INV_SQ_OP ".endr\n\t" INV_SQ_STORE(0)
static void prod_sq1(uint64_t *v)   { register uint64_t *p asm("rbp") = v; asm volatile(INV_SQ_LOAD(0) INV_SQ_OP INV_SQ_STORE(0) : : "r"(p) : CLOB); }
static void prod_sq10(uint64_t *v)  { register uint64_t *p asm("rbp") = v; asm volatile(PROD_RUN(9)  : : "r"(p) : CLOB); }
static void prod_sq100(uint64_t *v) { register uint64_t *p asm("rbp") = v; asm volatile(PROD_RUN(99) : : "r"(p) : CLOB); }
static void loop_sq1(uint64_t *v)   { register uint64_t *p asm("rbp") = v; asm volatile(LSQ(0, 0, 1)   : : "r"(p) : CLOB); }
static void loop_sq2(uint64_t *v)   { register uint64_t *p asm("rbp") = v; asm volatile(LSQ(0, 0, 2)   : : "r"(p) : CLOB); }
static void loop_sq5(uint64_t *v)   { register uint64_t *p asm("rbp") = v; asm volatile(LSQ(0, 0, 5)   : : "r"(p) : CLOB); }
static void loop_sq10(uint64_t *v)  { register uint64_t *p asm("rbp") = v; asm volatile(LSQ(0, 0, 10)  : : "r"(p) : CLOB); }
static void loop_sq100(uint64_t *v) { register uint64_t *p asm("rbp") = v; asm volatile(LSQ(0, 0, 100) : : "r"(p) : CLOB); }

static int fe_same(const uint64_t a[5], const uint64_t b[5]) {
    uint8_t x[32], y[32];
    fe_tobytes(x, a); fe_tobytes(y, b);
    return memcmp(x, y, 32) == 0;
}

#define TRIALS 200
#define TIME_MIN(res, ITERS, CALL) do {                                   \
        uint64_t _best = ~0ULL;                                           \
        for (int _t = 0; _t < TRIALS; _t++) {                             \
            uint64_t _a = rdtsc_start();                                  \
            for (int _r = 0; _r < (ITERS); _r++) { CALL; }                \
            uint64_t _c = rdtsc_end() - _a;                               \
            if (_c < _best) _best = _c;                                   \
        }                                                                 \
        (res) = (double)_best / (ITERS);                                  \
    } while (0)

#define NIN 8
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== probe_inv_loop: looped vs per-firing squaring in fe_invert ===\n\n");
    if (freq_guard()) return 2;
    bench_pin();

    uint64_t in[NIN][5];
    uint64_t seed = 0x9E3779B97F4A7C15ULL;
    for (int k = 0; k < NIN; k++)
        for (int j = 0; j < 5; j++) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            in[k][j] = seed & MASK51;
        }

    /* ── phase 1: production patches — references + timing ─────────── */
    install_production();
    uint8_t sc[32], pt[32], r[32];
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", sc, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", pt, 32);
    x25519(r, sc, pt);
    if (memcmp_hex(r, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32)) {
        printf("production RFC vector FAILED; abort\n");
        install_production(); return 1;
    }
    printf("[prod] RFC 7748 vector 1 OK\n");

    uint64_t ref_inv[NIN][5], ref_s[5][5];   /* ref_s: in[0]^(2^{1,2,5,10,100}) */
    const int sN[5] = {1, 2, 5, 10, 100};
    for (int k = 0; k < NIN; k++) {
        fe_invert(ref_inv[k], in[k]);
        uint64_t f[5]; fe_invert_fiat(f, in[k]);
        if (!fe_same(f, ref_inv[k])) { printf("prod invert != fiat invert (k=%d); abort\n", k); install_production(); return 1; }
    }
    for (int s = 0; s < 5; s++) {
        uint64_t v[5]; memcpy(v, in[0], 40);
        for (int i = 0; i < sN[s]; i++) prod_sq1(v);
        memcpy(ref_s[s], v, 40);
    }
    printf("[prod] references computed (production fe_invert == fiat on %d inputs)\n", NIN);

    double p_inv, p_sq1, p_sq10, p_sq100;
    uint64_t buf[5], o[5];
    memcpy(buf, in[1], 40);
    TIME_MIN(p_sq1,   1000, prod_sq1(buf));
    TIME_MIN(p_sq10,   200, prod_sq10(buf));
    TIME_MIN(p_sq100,   20, prod_sq100(buf));
    TIME_MIN(p_inv,     20, fe_invert(o, in[2]));

    /* ── phase 2: loop-sq patch ─────────────────────────────────────── */
    install_loop_sq();
    printf("[loop] n=1 single firing ... ");
    { uint64_t v[5]; memcpy(v, in[0], 40); loop_sq1(v);
      printf("%s\n", fe_same(v, ref_s[0]) ? "OK" : "MISMATCH"); if (!fe_same(v, ref_s[0])) goto bad; }
    void (*lf[5])(uint64_t *) = { loop_sq1, loop_sq2, loop_sq5, loop_sq10, loop_sq100 };
    for (int s = 1; s < 5; s++) {
        uint64_t v[5]; memcpy(v, in[0], 40); lf[s](v);
        int ok = fe_same(v, ref_s[s]);
        printf("[loop] n=%-3d vs %d production firings: %s\n", sN[s], sN[s], ok ? "OK" : "MISMATCH");
        if (!ok) goto bad;
    }
    int inv_ok = 1;
    for (int k = 0; k < NIN; k++) {
        uint64_t g[5]; fe_invert_loop(g, in[k]);
        if (!fe_same(g, ref_inv[k])) { inv_ok = 0; printf("[loop] fe_invert_loop MISMATCH k=%d\n", k); }
    }
    if (!inv_ok) goto bad;
    printf("[loop] fe_invert_loop == production fe_invert on %d inputs\n", NIN);

    double l_inv, l_sq1, l_sq2, l_sq5, l_sq10, l_sq100;
    memcpy(buf, in[1], 40);
    TIME_MIN(l_sq1,   1000, loop_sq1(buf));
    TIME_MIN(l_sq2,    500, loop_sq2(buf));
    TIME_MIN(l_sq5,    200, loop_sq5(buf));
    TIME_MIN(l_sq10,   200, loop_sq10(buf));
    TIME_MIN(l_sq100,   20, loop_sq100(buf));
    TIME_MIN(l_inv,     20, fe_invert_loop(o, in[2]));

    /* ── phase 3: production again (drift bracket + RFC after swap-back) */
    install_production();
    double p_inv2;
    TIME_MIN(p_inv2, 20, fe_invert(o, in[2]));
    x25519(r, sc, pt);
    int rfc2 = !memcmp_hex(r, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32);

    printf("\n-- squaring runs (TSC cyc, min of %d) --\n", TRIALS);
    printf("  %-6s %12s %12s %12s\n", "n", "prod/run", "loop/run", "loop-prod");
    printf("  %-6d %12.1f %12.1f %+12.1f\n", 1,   p_sq1,   l_sq1,   l_sq1 - p_sq1);
    printf("  %-6d %12.1f %12.1f %+12.1f\n", 10,  p_sq10,  l_sq10,  l_sq10 - p_sq10);
    printf("  %-6d %12.1f %12.1f %+12.1f\n", 100, p_sq100, l_sq100, l_sq100 - p_sq100);
    printf("  loop n=2 %.1f, n=5 %.1f\n", l_sq2, l_sq5);
    printf("  per-sq slope (n=10->100): prod %.2f  loop %.2f cyc/sq\n",
           (p_sq100 - p_sq10) / 90, (l_sq100 - l_sq10) / 90);
    printf("\n-- fe_invert (TSC cyc, min of %d) --\n", TRIALS);
    printf("  production (265 firings) : %9.1f   (re-timed after swap-back: %.1f)\n", p_inv, p_inv2);
    printf("  looped sq  ( 22 firings) : %9.1f   delta %+.1f (%+.1f%%)\n",
           l_inv, l_inv - p_inv, 100.0 * (l_inv - p_inv) / p_inv);
    printf("\n[prod] RFC vector 1 after swap-back: %s\n", rfc2 ? "OK" : "FAIL");
    init_match_and_patch(); do_fix_IN_patch();
    return 0;
bad:
    install_production();
    printf("loop patch produced wrong results; timing skipped\n");
    init_match_and_patch(); do_fix_IN_patch();
    return 1;
}
