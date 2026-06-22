/* probe_mul_carry_style.c — the conclusive isolation: does the fe_mul field-op
 * edge come from TRIAD PACKING DENSITY (MUL + 2 accumulation ops issued in one
 * triad, the "progressive accumulation" form), measured in the LATENCY regime
 * the real ladder pays?
 *
 * Background (this session): probe_mul_isolate ruled out the MUL primitive
 * (patch MUL latency 5.463 == mulq 5.464 cyc/op — same silicon). probe_carry_
 * isolate ruled out carry-dependency DEPTH (PARALLEL depth-10 ~= SERIAL depth-40).
 * Both ran in a THROUGHPUT regime (independent firings overlap). The remaining
 * hypothesis is that the win is the packing itself — 3 useful ops per triad,
 * with the accumulation folded in parallel with the MULs — and that this only
 * shows up when firings are DEPENDENT (output->input), as in the ladder.
 *
 * Method: hold EVERYTHING constant (same MULs, same field math, same wrapper,
 * same dependent-chain bench) and vary ONLY the packing density. The production
 * patch is 66 triads at 3 ops/triad. Because intra-triad ops execute fully
 * sequentially (CLAUDE.md: RAW/WAR/WAW all resolve slot0->1->2), re-emitting the
 * exact same op stream at fewer ops/triad is PROVABLY equivalent — same result,
 * just more triads and a longer critical path. So:
 *
 *   PACKED      — production array verbatim                 (66 triads, 3 ops/tri)
 *   PACKED-regen— repack(per=3): generator sanity check     (~66, must match PACKED)
 *   SERIAL-2    — repack(per=2): half the intra-triad ALU    (~99 triads)
 *   SERIAL-1    — repack(per=1): no packing at all          (~198 triads -> >128,
 *                 does not fit patch RAM; reported, not run — packing is also a
 *                 capacity NECESSITY, not just a speed optimization.)
 *
 * Bench is the realistic ladder pattern: fe_mul(a, b, a) chained so each firing
 * depends on the previous (latency regime). Read the result:
 *   SERIAL-2 >> PACKED  => packing density is the lever; the extra critical-path
 *                          triads cost real cycles in the dependent regime.
 *   SERIAL-2 ~= PACKED  => even packing is hidden; the field-op cost is the flat
 *                          per-firing trigger floor, and the same-ladder win over
 *                          amd64-51 is that floor < amd64-51's native call cost.
 *
 * Build: make PROG=probe_mul_carry_style CFLAGS="-O3 -march=native -masm=intel -fwrapv -fPIE -I include/"
 * Run:   sudo taskset -c 0 ./probe_mul_carry_style_static
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../include/patch.h"
#include "../../include/ucode_macro.h"
#include "../../include/misc.h"

#define MASK51 0x7FFFFFFFFFFFFULL

/* ════════════════════════════════════════════════════════════════════
 * PRODUCTION fe_mul patch (verbatim copy of install_fe_mul_patch's array
 * from asm_op_curve25519_mul.c — 66 triads, progressive accumulation).
 * ════════════════════════════════════════════════════════════════════ */
static const ucode_t packed[] = {
    /* ═══ PREP (3 triads) ═══ */
    { ZEROEXT_DSZ64_DR(TMP10, R15), ZEROEXT_DSZ64_DR(TMP11, R13), ZEROEXT_DSZ64_DR(TMP12, R9), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R10), ZEROEXT_DSZ64_DR(TMP14, RBX), MUL_DSZ64_DIR(RCX, 19, R13), NOP_SEQWORD },
    { MUL_DSZ64_DIR(RCX, 19, R9), MUL_DSZ64_DIR(RCX, 19, R10), MUL_DSZ64_DIR(RCX, 19, RBX), NOP_SEQWORD },

    /* ═══ c0 ═══ */
    { ZEROEXT_DSZ64_DR(RDX, TMP10), MUL_DSZ64_DRR(RCX, RDI, RDX), NOTAND_DSZ64_DRR(TMP0, TMP0, TMP0), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX), ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, R9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, R13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R15, TMP2, 13), SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* ═══ c1 ═══ */
    { ZEROEXT_DSZ64_DR(RDX, TMP11), MUL_DSZ64_DRR(RCX, RDI, RDX), OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX), ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, R9), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R13, TMP2, 13), SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* ═══ c2 ═══ */
    { ZEROEXT_DSZ64_DR(RDX, TMP12), MUL_DSZ64_DRR(RCX, RDI, RDX), OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX), ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, R10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R9, TMP2, 13), SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* ═══ c3 ═══ */
    { ZEROEXT_DSZ64_DR(RDX, TMP13), MUL_DSZ64_DRR(RCX, RDI, RDX), OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP12), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX), ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, RBX), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(R10, TMP2, 13), SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* ═══ c4 ═══ */
    { ZEROEXT_DSZ64_DR(RDX, TMP14), MUL_DSZ64_DRR(RCX, RDI, RDX), OR_DSZ64_DRR(TMP0, TMP8, TMP1), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP13), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R8, RCX), MUL_DSZ64_DRR(RCX, RSI, RDX), ZEROEXT_DSZ64_DR(TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP12), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R12, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP11), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R11, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), ZEROEXT_DSZ64_DR(RDX, TMP10), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), MUL_DSZ64_DRR(RCX, R14, RDX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP0, TMP0, RDX), SETCC_CONDB_DR(TMP15, TMP0), SHR_DSZ64_DRI(TMP8, TMP0, 51), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, RCX), ADD_DSZ64_DRR(TMP9, TMP9, TMP15), SHL_DSZ64_DRI(TMP2, TMP0, 13), NOP_SEQWORD },
    { ADD_DSZ64_DRR(R8, R8, TMP9), SHR_DSZ64_DRI(RAX, TMP2, 13), SHL_DSZ64_DRI(TMP1, R8, 13), NOP_SEQWORD },

    /* ═══ FINAL REDUCTION (3 triads) ═══ */
    { OR_DSZ64_DRR(TMP0, TMP8, TMP1), MUL_DSZ64_DIR(TMP2, 19, TMP0), ADD_DSZ64_DRR(R15, R15, TMP0), NOP_SEQWORD },
    { SHR_DSZ64_DRI(TMP0, R15, 51), ADD_DSZ64_DRR(R13, R13, TMP0), NOP, NOP_SEQWORD },
    { SHL_DSZ64_DRI(TMP2, R15, 13), SHR_DSZ64_DRI(R15, TMP2, 13), NOP, END_SEQWORD },
};
#define N_PACKED ((int)ARRAY_SZ(packed))

/* ════════════════════════════════════════════════════════════════════
 * Generator: re-emit the packed op stream at `per` ops/triad. Sequential
 * intra-triad semantics (CLAUDE.md) make this provably equivalent — same
 * ops, same order, just a longer critical path. NOP op-slots in the source
 * (only 2 of them, in F1/F2) are streamed harmlessly.
 * ════════════════════════════════════════════════════════════════════ */
static ucode_t outbuf[256];

static int repack(const ucode_t *src, int ns, int per, ucode_t *dst) {
    uint64_t slots[3 * 256];
    int s = 0;
    for (int i = 0; i < ns; i++) {
        slots[s++] = src[i].uop0;
        slots[s++] = src[i].uop1;
        slots[s++] = src[i].uop2;
    }
    int n = 0, k = 0;
    while (k < s) {
        uint64_t a = (k < s) ? slots[k++] : NOP;
        uint64_t b = (per >= 2 && k < s) ? slots[k++] : NOP;
        uint64_t c = (per >= 3 && k < s) ? slots[k++] : NOP;
        dst[n].uop0 = a; dst[n].uop1 = b; dst[n].uop2 = c;
        dst[n].seqw = NOP_SEQWORD;
        n++;
    }
    dst[n - 1].seqw = END_SEQWORD;
    return n;
}

static void install_patch(const ucode_t *p, int n) {
    patch_ucode(0x7c00, (ucode_t *)p, n);
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
}

/* ── fe_mul via microcode (identical wrapper to production) ──────────── */
static void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_b   asm("rbx") = (uint64_t *)b;
    register uint64_t *_out asm("r15") = out;
    asm volatile(
        "push r15\n\t"
        "mov rdi, [rcx]\n\t"  "mov rsi, [rcx + 8]\n\t" "mov r12, [rcx + 16]\n\t"
        "mov r11, [rcx + 24]\n\t" "mov r14, [rcx + 32]\n\t"
        "mov r15, [rbx]\n\t"  "mov r13, [rbx + 8]\n\t"  "mov r9,  [rbx + 16]\n\t"
        "mov r10, [rbx + 24]\n\t" "mov rbx, [rbx + 32]\n\t"
        "xor eax, eax\n\t" "xor r8d, r8d\n\t"
        "vmwrite rcx, rdx\n\t"
        "pop rcx\n\t"
        "mov [rcx], r15\n\t" "mov [rcx + 8], r13\n\t" "mov [rcx + 16], r9\n\t"
        "mov [rcx + 24], r10\n\t" "mov [rcx + 32], rax\n\t"
        : "+r"(_a), "+r"(_b), "+r"(_out)
        :
        : "rax", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14",
          "memory", "cc");
}

/* ── native reference oracle ────────────────────────────────────────── */
static void fe_mul_native(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t a0=a[0],a1=a[1],a2=a[2],a3=a[3],a4=a[4];
    uint64_t b0=b[0],b1=b[1],b2=b[2],b3=b[3],b4=b[4];
    uint64_t r1=19*b1,r2=19*b2,r3=19*b3,r4=19*b4;
    __uint128_t c0=(__uint128_t)a0*b0+(__uint128_t)a1*r4+(__uint128_t)a2*r3+(__uint128_t)a3*r2+(__uint128_t)a4*r1;
    __uint128_t c1=(__uint128_t)a0*b1+(__uint128_t)a1*b0+(__uint128_t)a2*r4+(__uint128_t)a3*r3+(__uint128_t)a4*r2;
    __uint128_t c2=(__uint128_t)a0*b2+(__uint128_t)a1*b1+(__uint128_t)a2*b0+(__uint128_t)a3*r4+(__uint128_t)a4*r3;
    __uint128_t c3=(__uint128_t)a0*b3+(__uint128_t)a1*b2+(__uint128_t)a2*b1+(__uint128_t)a3*b0+(__uint128_t)a4*r4;
    __uint128_t c4=(__uint128_t)a0*b4+(__uint128_t)a1*b3+(__uint128_t)a2*b2+(__uint128_t)a3*b1+(__uint128_t)a4*b0;
    uint64_t carry;
    carry=(uint64_t)(c0>>51); out[0]=(uint64_t)c0&MASK51; c1+=carry;
    carry=(uint64_t)(c1>>51); out[1]=(uint64_t)c1&MASK51; c2+=carry;
    carry=(uint64_t)(c2>>51); out[2]=(uint64_t)c2&MASK51; c3+=carry;
    carry=(uint64_t)(c3>>51); out[3]=(uint64_t)c3&MASK51; c4+=carry;
    carry=(uint64_t)(c4>>51); out[4]=(uint64_t)c4&MASK51;
    out[0]+=carry*19; carry=out[0]>>51; out[0]&=MASK51; out[1]+=carry;
}

static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* compare ucode vs native over N random inputs; return #failures */
static int verify_random(int N) {
    uint64_t seed = 0xC0FFEE123456789ULL;
    int fail = 0;
    for (int t = 0; t < N; t++) {
        uint64_t a[5], b[5], u[5], v[5];
        for (int i = 0; i < 5; i++) a[i] = splitmix64(&seed) & MASK51;
        for (int i = 0; i < 5; i++) b[i] = splitmix64(&seed) & MASK51;
        fe_mul_ucode(a, b, u);
        fe_mul_native(a, b, v);
        if (memcmp(u, v, sizeof u) != 0) fail++;
    }
    return fail;
}

/* ── timing (same harness shape as the other probes) ─────────────────── */
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
#define REPS  200

/* dependent chain: fe_mul(a, b, a) — each firing's input a is the previous
 * firing's output (the ladder's latency regime). Returns min cyc/op. */
static double bench_dependent(void) {
    uint64_t seed = 0x1234ABCDULL;
    uint64_t a0[5], b[5];
    for (int i = 0; i < 5; i++) a0[i] = splitmix64(&seed) & MASK51;
    for (int i = 0; i < 5; i++) b[i]  = (splitmix64(&seed) & MASK51) | 1;
    uint64_t best = ~0ULL;
    for (int r = 0; r < REPS; r++) {
        uint64_t a[5]; memcpy(a, a0, sizeof a);
        uint64_t t0 = rdtsc_start();
        for (int i = 0; i < BATCH; i++) fe_mul_ucode(a, b, a);
        uint64_t t1 = rdtsc_end();
        uint64_t c = t1 - t0;
        if (c < best) best = c;
    }
    return (double)best / (double)BATCH;
}

static void run(const char *name, const ucode_t *p, int n) {
    if (n > 128) {
        printf("  %-13s %6d   %9s   %s\n", name, n, "-",
               "DOES NOT FIT patch RAM (>128 triads) — skipped");
        return;
    }
    install_patch(p, n);
    int fail = verify_random(2000);
    double per = bench_dependent();
    printf("  %-13s %6d   %9.2f   %s\n", name, n, per,
           fail == 0 ? "verify OK (2000 vec)" : "VERIFY FAILED");
    if (fail) printf("                 ^^ %d/2000 mismatches vs native\n", fail);
}

int main(void) {
    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();

    printf("=== fe_mul packing-density isolation (dependent chain, min cyc/op) ===\n");
    printf("(same MULs, same math, same wrapper — only ops/triad differs)\n\n");
    printf("  %-13s %6s   %9s   %s\n", "variant", "triads", "cyc/op", "result");
    printf("  %-13s %6s   %9s   %s\n", "-------", "------", "------", "------");

    /* PACKED: production array verbatim (3 ops/triad). */
    run("PACKED", packed, N_PACKED);

    /* PACKED-regen: generator at per=3 — must match PACKED in output & ~cyc.
     * Validates that repack() preserves semantics before we trust SERIAL. */
    int n3 = repack(packed, N_PACKED, 3, outbuf);
    run("PACKED-regen", outbuf, n3);

    /* SERIAL-2: half the intra-triad ALU parallelism. */
    int n2 = repack(packed, N_PACKED, 2, outbuf);
    run("SERIAL-2", outbuf, n2);

    /* SERIAL-1: no packing — expected ~198 triads, will not fit. */
    int n1 = repack(packed, N_PACKED, 1, outbuf);
    run("SERIAL-1", outbuf, n1);

    printf("\nSERIAL-2 >> PACKED  => packing density is the lever (critical-path\n");
    printf("                       triads cost real cycles in the dependent regime).\n");
    printf("SERIAL-2 ~= PACKED  => packing is hidden too; the field-op cost is the\n");
    printf("                       flat per-firing trigger floor.\n");

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
