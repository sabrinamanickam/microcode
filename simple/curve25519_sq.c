/*
 * bench_sq.c — fe_sq via microcode (vmwrite) vs native C (-O3)
 *
 * Patch: 56 triads at U7c00, hooked on vmwrite (0x0cd8).
 * All 15 MACs + carry propagation + reduction in microcode.
 *
 * Register convention (caller → microcode):
 *   RDI=a0  RSI=a1  R12=a2  R11=a3  R14=a4
 *   R15=2*a0  R13=2*a1  R9=2*a2  R10=2*a3
 *   RBX=19*a4  RDX=19*a3
 *   RAX=0  R8=0
 *
 * Output: RDI=h0  R9=h1  R10=h2  RBX=h3  RAX=h4
 *
 * Build:  gcc -O3 -o bench_sq bench_sq.c -I../../include
 * Run:    sudo taskset -c 0 ./bench_sq
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL

/* ── microcode patch ──────────────────────────────────────────── */

static void install_fe_sq_patch(void) {
    ucode_t patch[] = {

    /* ═══ LIMB c0 = a0*a0 + d1*r4 + d2*r3 ═══ */
    { ZEROEXT_DSZ64_DR(TMP0, RAX),
      MUL_DSZ64_DRR(RCX, RDI, RDI),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, RDI),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R13),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, R13),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RDX, R9),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R9),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(RDI, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ c0→c1 transition ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(R13, RSI),
      NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, R13),
      ADD_DSZ64_DRR(R9, R12, R12),
      NOP_SEQWORD },

    /* ═══ LIMB c1 = d0*a1 + r3*a3 + d2*r4 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, R13),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R11, RDX),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, RDX),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R9),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R9),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(R9, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ c1→c2 transition ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(RDX, R12),
      NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, RDX),
      ZEROEXT_DSZ64_DR(R13, RSI),
      NOP_SEQWORD },

    /* ═══ LIMB c2 = d0*a2 + a1*a1 + d3*r4 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RSI, R13),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, R13),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, RBX, R10),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R10),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(R10, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ c2→c3 transition ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      ZEROEXT_DSZ64_DR(RDX, R11),
      NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, RDX),
      ADD_DSZ64_DRR(R13, RSI, RSI),
      NOP_SEQWORD },

    /* ═══ LIMB c3 = d0*a3 + d1*a2 + r4*a4 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, RDX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RSI, R12),
      ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { NOP,
      MUL_DSZ64_DRR(RCX, R13, RSI),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, RSI),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R14, RBX),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, RBX),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(RBX, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ c3→c4 transition ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOTAND_DSZ64_DRR(R8, R8, R8),
      NOP, NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      MUL_DSZ64_DRR(RCX, R15, R14),
      NOP, NOP_SEQWORD },

    /* ═══ LIMB c4 = d0*a4 + d1*a3 + a2*a2 ═══ */
    { ADD_DSZ64_DRR(TMP2, TMP0, R14),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP4, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R13, R11),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP2, R11),
      SETCC_CONDB_DR(TMP3, TMP0),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP5, RCX, TMP3),
      MUL_DSZ64_DRR(RCX, R12, R12),
      NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP2, TMP0, R12),
      SETCC_CONDB_DR(TMP3, TMP2),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP8, TMP2, 51),
      ADD_DSZ64_DRR(TMP6, RCX, TMP3),
      NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP9, TMP2, 13),
      ADD_DSZ64_DRR(R8, R8, TMP4),
      ADD_DSZ64_DRR(TMP0, TMP5, TMP6),
      NOP_SEQWORD },
    { SHR_DSZ64_DRI(RAX, TMP9, 13),
      ADD_DSZ64_DRR(R8, R8, TMP0),
      NOP, NOP_SEQWORD },

    /* ═══ FINAL REDUCTION ═══ */
    { SHL_DSZ64_DRI(TMP1, R8, 13),
      NOP, NOP, NOP_SEQWORD },
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1),
      NOP, NOP, NOP_SEQWORD },
    { MUL_DSZ64_DIR(TMP1, 19, TMP0),
      NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(RDI, RDI, TMP0),
      NOP, NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, RDI, 51),
      NOP, NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP1, RDI, 13),
      ADD_DSZ64_DRR(R9, R9, TMP0),
      NOP, NOP_SEQWORD },
    { SHR_DSZ64_DRI(RDI, TMP1, 13),
      NOP, NOP, END_SEQWORD }

    };

    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("fe_sq patch installed: %d triads at U7c00\n", (int)ARRAY_SZ(patch));
}

/* ── fe_sq via microcode (register I/O) ───────────────────────── */

static struct { uint64_t a; uint64_t out; } fe_sq_args;

static void fe_sq_ucode(const uint64_t *a, uint64_t *out) {
    fe_sq_args.a   = (uint64_t)a;
    fe_sq_args.out = (uint64_t)out;

    asm volatile(
        "mov rax, [%[args]]\n\t"
        "mov rcx, [%[args]+8]\n\t"

        "sub rsp, 56\n\t"
        "mov [rsp+40], rcx\n\t"

        "mov rcx, [rax]\n\t"     "mov [rsp],    rcx\n\t"
        "mov rcx, [rax+8]\n\t"   "mov [rsp+8],  rcx\n\t"
        "mov rcx, [rax+16]\n\t"  "mov [rsp+16], rcx\n\t"
        "mov rcx, [rax+24]\n\t"  "mov [rsp+24], rcx\n\t"
        "mov rcx, [rax+32]\n\t"  "mov [rsp+32], rcx\n\t"

        "mov rdi, [rsp]\n\t"
        "mov rsi, [rsp+8]\n\t"
        "mov r12, [rsp+16]\n\t"
        "mov r11, [rsp+24]\n\t"
        "mov r14, [rsp+32]\n\t"

        "lea r15, [rdi+rdi]\n\t"
        "lea r13, [rsi+rsi]\n\t"
        "lea r9,  [r12+r12]\n\t"
        "lea r10, [r11+r11]\n\t"

        "imul rbx, r14, 19\n\t"
        "imul rdx, r11, 19\n\t"

        "xor eax, eax\n\t"
        "xor r8d, r8d\n\t"

        "vmwrite rcx, rdx\n\t"

        "mov rcx, [rsp+40]\n\t"
        "mov [rcx],    rdi\n\t"
        "mov [rcx+8],  r9\n\t"
        "mov [rcx+16], r10\n\t"
        "mov [rcx+24], rbx\n\t"
        "mov [rcx+32], rax\n\t"

        "add rsp, 56\n\t"
        :
        : [args] "r"(&fe_sq_args)
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory"
    );
}

/* ── fe_sq native C (compiled with -O3) ───────────────────────── */

static void fe_sq_native(const uint64_t *a, uint64_t *out) {
    uint64_t a0=a[0], a1=a[1], a2=a[2], a3=a[3], a4=a[4];
    uint64_t d0=2*a0, d1=2*a1, d2=2*a2, d3=2*a3;
    uint64_t r3=19*a3, r4=19*a4;

    __uint128_t c0 = (__uint128_t)a0*a0 + (__uint128_t)d1*r4 + (__uint128_t)d2*r3;
    __uint128_t c1 = (__uint128_t)d0*a1 + (__uint128_t)r3*a3 + (__uint128_t)d2*r4;
    __uint128_t c2 = (__uint128_t)d0*a2 + (__uint128_t)a1*a1 + (__uint128_t)d3*r4;
    __uint128_t c3 = (__uint128_t)d0*a3 + (__uint128_t)d1*a2 + (__uint128_t)r4*a4;
    __uint128_t c4 = (__uint128_t)d0*a4 + (__uint128_t)d1*a3 + (__uint128_t)a2*a2;

    uint64_t carry;
    carry = (uint64_t)(c0>>51); out[0] = (uint64_t)c0 & MASK51;
    c1 += carry;
    carry = (uint64_t)(c1>>51); out[1] = (uint64_t)c1 & MASK51;
    c2 += carry;
    carry = (uint64_t)(c2>>51); out[2] = (uint64_t)c2 & MASK51;
    c3 += carry;
    carry = (uint64_t)(c3>>51); out[3] = (uint64_t)c3 & MASK51;
    c4 += carry;
    carry = (uint64_t)(c4>>51); out[4] = (uint64_t)c4 & MASK51;
    out[0] += carry * 19;
    carry = out[0] >> 51; out[0] &= MASK51;
    out[1] += carry;
}

/* ── timing ───────────────────────────────────────────────────── */

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

#define BATCH 1000
#define REPS  100

int main(void) {
    uint64_t t0, t1, min, sum;

    printf("=== fe_sq: microcode vs native -O3 ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_fe_sq_patch();

    /* ── correctness ──────────────────────────────────────────── */
    uint64_t state[5] = { 0x00062D608F25D51AULL, 0x000412A4B4F6592AULL,
                          0x00075B7171A4B31DULL, 0x0001FF60527118FEULL,
                          0x000216936D3CD6E5ULL };
    uint64_t ref[5], ucd[5];

    fe_sq_native(state, ref);
    fe_sq_ucode(state, ucd);

    int ok = memcmp(ref, ucd, sizeof(ref)) == 0;
    printf("Single:  %s\n", ok ? "PASS" : "FAIL");
    if (!ok) {
        for (int i = 0; i < 5; i++)
            printf("  [%d] ref=%016" PRIx64 " ucd=%016" PRIx64 " %s\n",
                   i, ref[i], ucd[i], ref[i]==ucd[i] ? "" : "***");
        return 1;
    }

    /* iterated */
    uint64_t ri[5], ui[5];
    memcpy(ri, state, sizeof(ri));
    memcpy(ui, state, sizeof(ui));
    for (int i = 0; i < 1000; i++) {
        fe_sq_native(ri, ri);
        fe_sq_ucode(ui, ui);
    }
    ok = memcmp(ri, ui, sizeof(ri)) == 0;
    printf("1000 sq: %s\n\n", ok ? "PASS" : "FAIL");
    if (!ok) return 1;

    /* ── benchmark ────────────────────────────────────────────── */
    printf("--- %d ops/batch, %d batches ---\n\n", BATCH, REPS);

    uint64_t tmp[5];

    /* native C -O3 */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_native(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Native -O3:  min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* microcode */
    min = UINT64_MAX; sum = 0;
    for (int r = 0; r < REPS; r++) {
        memcpy(tmp, state, sizeof(tmp));
        t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_sq_ucode(tmp, tmp);
        t1 = rdtsc_end();
        uint64_t dt = t1 - t0;
        sum += dt; if (dt < min) min = dt;
    }
    printf("Microcode:   min/op %4" PRIu64 "  avg/op %4" PRIu64 " cycles\n",
           min/BATCH, sum/REPS/BATCH);

    /* clean up */
    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}
