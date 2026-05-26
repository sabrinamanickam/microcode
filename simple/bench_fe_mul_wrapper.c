/*
 * bench_fe_mul_wrapper.c — measure the C-wrapper overhead around the 5×51 fe_mul patch.
 *
 * The hypothesis: amd64-64 beats 5×51 microcode by inlining the entire
 * ladderstep into one register-resident asm block — no C-function-call
 * wrapper, no memory I/O between consecutive field ops. If true, then a
 * 5×51 inline-asm ladder could close (or beat) the 40k gap.
 *
 * Three patterns measured here:
 *   A. Standalone tight loop:  fe_mul_ucode(out, a, b)  with constant a, b.
 *      OoO+STLF pipelines hard; this is the lower bound.
 *   B. Chained via C wrapper:  out = out * b  (out feeds itself).
 *      Forces a dependency chain through memory; this is what the LADDER
 *      pays per fe_mul call.
 *   C. Chained inline-asm:     same dependency chain, but with state in
 *      arch regs across all N vmwrites. NO memory load/store per iter.
 *      This is what an inlined ladder would achieve.
 *
 * Decision rule:
 *   delta = (B − C)  =  per-call wrapper overhead we can recover.
 *   If delta × 1287 fe_mul ≥ 40k cyc, the inline-asm 5×51 ladder is worth
 *   building.
 *
 * Build:  make PROG=bench_fe_mul_wrapper
 * Run:    sudo taskset -c 0 ./bench_fe_mul_wrapper_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

/* ─────────────── 5×51 fe_mul patch (verbatim from full_curve25519.c) ─────────────── */

static void install_field_patches(void) {
    ucode_t mul_patch[] = {
    { ZEROEXT_DSZ64_DR(TMP10, R15),
      ZEROEXT_DSZ64_DR(TMP11, R13),
      ZEROEXT_DSZ64_DR(TMP12, R9), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R10),
      ZEROEXT_DSZ64_DR(TMP14, RBX),
      MUL_DSZ64_DIR(RCX, 19, R13), NOP_SEQWORD },
    { MUL_DSZ64_DIR(RCX, 19, R9),
      MUL_DSZ64_DIR(RCX, 19, R10),
      MUL_DSZ64_DIR(RCX, 19, RBX), NOP_SEQWORD },

    /* c0 = a0*b0 + a1*19b4 + a2*19b3 + a3*19b2 + a4*19b1 */
    { ZEROEXT_DSZ64_DR(RDX, TMP10), MUL_DSZ64_DRR(RCX, RDI, RDX),
      NOTAND_DSZ64_DRR(TMP0, TMP0, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R15, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* c1 = a0*b1 + a1*b0 + a2*19b4 + a3*19b3 + a4*19b2 */
    { ZEROEXT_DSZ64_DR(RDX, TMP11), MUL_DSZ64_DRR(RCX, RDI, RDX),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R13, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* c2 */
    { ZEROEXT_DSZ64_DR(RDX, TMP12), MUL_DSZ64_DRR(RCX, RDI, RDX),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R9, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* c3 */
    { ZEROEXT_DSZ64_DR(RDX, TMP13), MUL_DSZ64_DRR(RCX, RDI, RDX),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP12), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R10, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* c4 = a0*b4 + a1*b3 + a2*b2 + a3*b1 + a4*b0 */
    { ZEROEXT_DSZ64_DR(RDX, TMP14), MUL_DSZ64_DRR(RCX, RDI, RDX),
      OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP13), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX),
      ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP12), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX),
      ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0),
      SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15),
      SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R13, TMP2, 13),
      SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* finalize c4 to RAX, fold carry → c0 (R15) */
    { OR_DSZ64_DRR(R10, TMP8, TMP1), MUL_DSZ64_DIR(RAX, 19, TMP8),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(R15, R15, RAX), SHL_DSZ64_DRI(TMP2, R10, 13),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(R10, TMP2, 13), SHR_DSZ64_DRI(TMP8, R15, 51),
      NOP, NOP_SEQWORD },
    { OR_DSZ64_DRR(R15, R15, TMP8), SHL_DSZ64_DRI(TMP2, R15, 13),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(R15, TMP2, 13), ZEROEXT_DSZ64_DR(RAX, R13),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ32_DI(R13, 0), ZEROEXT_DSZ32_DI(RBX, 0), NOP, END_SEQWORD }
    };
    patch_ucode(0x7c00, mul_patch, ARRAY_SZ(mul_patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("5×51 fe_mul patch: %d triads at U7c00\n", (int)ARRAY_SZ(mul_patch));
}

/* ─────────────── C-wrapper fe_mul (verbatim from full_curve25519.c) ─────────────── */

static void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    register const uint64_t *_a   asm("rdi") = a;
    register const uint64_t *_b   asm("rsi") = b;
    register       uint64_t *_out asm("rdx") = out;
    asm volatile(
        "mov rbp, rdx\n\t"
        "mov r15, [rsi]\n\t"
        "mov r13, [rsi + 8]\n\t"
        "mov r9,  [rsi + 16]\n\t"
        "mov r10, [rsi + 24]\n\t"
        "mov rbx, [rsi + 32]\n\t"
        "mov rsi, [rdi + 8]\n\t"
        "mov r12, [rdi + 16]\n\t"
        "mov r11, [rdi + 24]\n\t"
        "mov r14, [rdi + 32]\n\t"
        "mov rdi, [rdi]\n\t"
        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"
        "vmwrite rcx, rdx\n\t"
        "mov [rbp],      r15\n\t"
        "mov [rbp + 8],  r13\n\t"
        "mov [rbp + 16], r9\n\t"
        "mov [rbp + 24], r10\n\t"
        "mov [rbp + 32], rax\n\t"
        : "+r"(_a), "+r"(_b), "+r"(_out)
        :
        : "rax", "rbx", "rcx", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory", "cc"
    );
}

/* ─────────────── timing ─────────────── */

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

/* ─────────────── Bench A: standalone (constant a, b — tight loop) ─────────────── */
static uint64_t bench_standalone(int N) {
    uint64_t a[5] = {1, 2, 3, 4, 5};
    uint64_t b[5] = {6, 7, 8, 9, 10};
    uint64_t out[5];
    for (int w = 0; w < 100; w++) fe_mul_ucode(a, b, out);   /* warmup */
    uint64_t t0 = rdtsc_start();
    for (int i = 0; i < N; i++) {
        fe_mul_ucode(a, b, out);   /* constant inputs — OoO pipelines hard */
    }
    uint64_t t1 = rdtsc_end();
    return t1 - t0;
}

/* ─────────────── Bench B: chained C-wrapper (out feeds back) ─────────────── */
static uint64_t bench_chained_wrapper(int N) {
    uint64_t out[5] = {1, 2, 3, 4, 5};
    uint64_t b[5]   = {6, 7, 8, 9, 10};
    for (int w = 0; w < 100; w++) fe_mul_ucode(out, b, out); /* warmup */
    uint64_t t0 = rdtsc_start();
    for (int i = 0; i < N; i++) {
        fe_mul_ucode(out, b, out);  /* out = out * b — forces dependency through memory */
    }
    uint64_t t1 = rdtsc_end();
    return t1 - t0;
}

/* ─────────────── Bench C: chained inline-asm (data stays in regs) ───────────────
 *
 * Hand-written inline asm that loops N times, holding both 'a' and 'b' in arch
 * registers across all iterations. Output of each call becomes input 'a' of
 * the next; 'b' is preserved via a stack slot (5 limbs = 40 bytes) and
 * reloaded per iter — but stack STLF is fast.
 *
 * Pre-patch regs (caller convention):
 *   RDI=a0  RSI=a1  R12=a2  R11=a3  R14=a4
 *   R15=b0  R13=b1  R9=b2   R10=b3  RBX=b4
 *   RAX=0   R8=0
 * Post-patch (h):
 *   R15=h0  R13=h1  R9=h2   R10=h3  RAX=h4
 *
 * Per iter: vmwrite, then 5 reg-moves to stage h as next 'a', then reload b
 * from stack into b-regs. (b-regs get clobbered by the patch since they hold
 * the output.)
 */
/* Pack args into a single struct to reduce GCC's register pressure. */
typedef struct {
    const uint64_t *a;
    const uint64_t *b;
    uint64_t       *out;
    uint64_t        N;
    uint64_t        cycles;
} bench_args_t;

static uint64_t bench_chained_inline(int N) {
    uint64_t a_init[5] = {1, 2, 3, 4, 5};
    uint64_t b_init[5] = {6, 7, 8, 9, 10};
    uint64_t out[5];
    bench_args_t args = { a_init, b_init, out, (uint64_t)N, 0 };

    /* 100 warmup iters via C wrapper — gets the patch hot in icache */
    uint64_t scratch[5];
    memcpy(scratch, a_init, sizeof(scratch));
    for (int w = 0; w < 100; w++) fe_mul_ucode(scratch, b_init, scratch);

    /* Pass &args through a single register; everything else is offsets from it. */
    register bench_args_t *_p asm("rbp") = &args;

    asm volatile(
        /* Save callee-saved regs we'll clobber */
        "push rbp\n\t"
        "push rbx\n\t"
        "push r12\n\t"
        "push r13\n\t"
        "push r14\n\t"
        "push r15\n\t"

        /* reload rbp = &args (the pushes may have moved rsp; rbp still holds &args) */
        "sub rsp, 64\n\t"

        /* Copy b to stack slots [rsp+0..32] for fast reload after each vmwrite. */
        "mov rax, [rbp + 8]\n\t"          /* args.b */
        "mov rcx, [rax + 0]\n\t"   "mov [rsp + 0],  rcx\n\t"
        "mov rcx, [rax + 8]\n\t"   "mov [rsp + 8],  rcx\n\t"
        "mov rcx, [rax + 16]\n\t"  "mov [rsp + 16], rcx\n\t"
        "mov rcx, [rax + 24]\n\t"  "mov [rsp + 24], rcx\n\t"
        "mov rcx, [rax + 32]\n\t"  "mov [rsp + 32], rcx\n\t"

        /* Save N to [rsp+40] */
        "mov rax, [rbp + 24]\n\t"         /* args.N */
        "mov [rsp + 40], rax\n\t"

        /* Load initial a */
        "mov rax, [rbp + 0]\n\t"          /* args.a */
        "mov rdi, [rax + 0]\n\t"
        "mov rsi, [rax + 8]\n\t"
        "mov r12, [rax + 16]\n\t"
        "mov r11, [rax + 24]\n\t"
        "mov r14, [rax + 32]\n\t"

        /* Load b → b-regs */
        "mov r15, [rsp + 0]\n\t"
        "mov r13, [rsp + 8]\n\t"
        "mov r9,  [rsp + 16]\n\t"
        "mov r10, [rsp + 24]\n\t"
        "mov rbx, [rsp + 32]\n\t"

        /* Timing start — cpuid + rdtsc */
        "xor eax, eax\n\t"
        "cpuid\n\t"
        "rdtsc\n\t"
        "shl rdx, 32\n\t"
        "or rax, rdx\n\t"
        "mov [rsp + 48], rax\n\t"        /* save t0 at [rsp+48] */

        /* Main loop: vmwrite ; stage h → a ; reload b */
        "Lloop_inline%=:\n\t"
        "  xor eax, eax\n\t"
        "  xor r8d, r8d\n\t"
        "  vmwrite rcx, rdx\n\t"          /* fires patch; h → R15,R13,R9,R10,RAX */
        /* h → next 'a' */
        "  mov rdi, r15\n\t"
        "  mov rsi, r13\n\t"
        "  mov r12, r9\n\t"
        "  mov r11, r10\n\t"
        "  mov r14, rax\n\t"
        /* reload b → b-regs (the patch overwrote them) */
        "  mov r15, [rsp + 0]\n\t"
        "  mov r13, [rsp + 8]\n\t"
        "  mov r9,  [rsp + 16]\n\t"
        "  mov r10, [rsp + 24]\n\t"
        "  mov rbx, [rsp + 32]\n\t"
        "  dec qword ptr [rsp + 40]\n\t"
        "  jnz Lloop_inline%=\n\t"

        /* Save final h to args.out (= rbp+16) */
        "mov rax, [rbp + 16]\n\t"
        "mov [rax + 0],  rdi\n\t"
        "mov [rax + 8],  rsi\n\t"
        "mov [rax + 16], r12\n\t"
        "mov [rax + 24], r11\n\t"
        "mov [rax + 32], r14\n\t"

        /* Timing end — rdtscp + cpuid */
        "rdtscp\n\t"
        "shl rdx, 32\n\t"
        "or rax, rdx\n\t"
        "sub rax, [rsp + 48]\n\t"
        "mov [rbp + 32], rax\n\t"        /* args.cycles = t1 − t0 */
        "xor eax, eax\n\t"
        "cpuid\n\t"

        "add rsp, 64\n\t"

        "pop r15\n\t"
        "pop r14\n\t"
        "pop r13\n\t"
        "pop r12\n\t"
        "pop rbx\n\t"
        "pop rbp\n\t"

        :
        : "r"(_p)
        : "memory", "cc"
    );
    return args.cycles;
}

/* ─────────────── helpers ─────────────── */

static int cmp_u64(const void *p, const void *q) {
    uint64_t x = *(const uint64_t *)p, y = *(const uint64_t *)q;
    return (x > y) - (x < y);
}

#define N_PER_RUN 10000
#define REPS      200

static void run(const char *label, uint64_t (*fn)(int)) {
    uint64_t s[REPS];
    for (int r = 0; r < REPS; r++) s[r] = fn(N_PER_RUN);
    qsort(s, REPS, sizeof(s[0]), cmp_u64);
    uint64_t mn = s[0], md = s[REPS/2];
    printf("  %-32s min %4" PRIu64 " cyc/op   median %4" PRIu64 " cyc/op\n",
           label, mn / N_PER_RUN, md / N_PER_RUN);
}

int main(void) {
    printf("=== fe_mul wrapper-overhead probe ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();

    printf("\nN=%d ops/run, %d runs, taskset -c 0\n\n", N_PER_RUN, REPS);
    run("A. Standalone, constant inputs", bench_standalone);
    run("B. C-wrapper chained (mem dep)", bench_chained_wrapper);
    run("C. Inline-asm chained (reg dep)", bench_chained_inline);

    printf("\n");
    printf("Interpretation:\n");
    printf("  A is the throughput floor (OoO+STLF pipelines).\n");
    printf("  B is what the ladder pays per fe_mul (dependency through memory).\n");
    printf("  C is what an inline-asm ladder would achieve.\n");
    printf("  Recoverable per-call wrapper cost ≈ B − C cycles.\n");
    printf("  Total X25519 savings ≈ (B−C) × 1287 fe_mul + (similar for fe_sq).\n");
    printf("  Decision: if (B−C) × 1287 ≥ 40k cyc, the inline-asm ladder is worth building.\n");

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
