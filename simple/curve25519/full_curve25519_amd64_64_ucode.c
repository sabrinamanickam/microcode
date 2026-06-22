/*
 * full_curve25519_amd64_64_ucode.c — amd64-64 framework + 4×64 microcode field ops.
 *
 * The amd64-64/ucode hybrid: SUPERCOP amd64-64's driver (mont25519),
 * fe25519_invert, pack/unpack/setint, fe25519_freeze, and work_cswap are
 * reused unchanged (recompiled under the x25519_amd64_64_ucode namespace);
 * only ladderstep + fe25519_mul + fe25519_square are replaced with C that
 * calls the 4×64 chained-ADC microcode (this file's fe_mul_ucode4 /
 * fe_sq_ucode4). Because amd64-64 is 4×64 saturated (radix 2^64) — exactly
 * the microcode's representation — this is a true drop-in: no 4×64↔5×51
 * conversion at the field-op boundary, unlike the 5×51 amd64-51/ucode hybrid.
 *
 * This isolates the field-op backend: amd64-64/asm vs amd64-64/ucode share
 * the same ladder framework and differ only in mul/square. fe_mul is the v3
 * chained-ADC patch (75 triads, ~200 cyc/op); fe_sq is fe_mul(a,a) — no
 * squaring symmetry (a separate sq patch won't fit alongside v3 under the
 * 128-triad RAM cap, and the 4×64 patch can't coexist with the 5×51 patches
 * in full_curve25519.c either, which is why this is a standalone binary —
 * same pattern as full_curve25519_inline2.c / ours/ucode-inline).
 *
 * Standalone binary (own main(), bare "min:"/"median:" output) so the
 * bench_supercop_matrix.sh harness can build + run it per (compiler,-O)
 * config and slot the result into the amd64-64 same-ladder table.
 *
 * Build: make PROG=full_curve25519_amd64_64_ucode
 * Run:   sudo taskset -c 0 ./full_curve25519_amd64_64_ucode_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "../../../include/patch.h"
#include "../../../include/ucode_macro.h"
#include "../../../include/misc.h"

typedef uint64_t fe4[4];

/* The amd64-64/ucode hybrid entry point (mont25519.c, namespaced). */
extern int x25519_amd64_64_ucode(unsigned char *out,
                                 const unsigned char *scalar,
                                 const unsigned char *point);

/* ════════════════════════════════════════════════════════════════════
 * MICROCODE PATCH (v3 chained-ADC fe_mul) — lifted verbatim from
 * full_curve25519_4x64.c (RFC 7748 §5.2 verified).
 * ════════════════════════════════════════════════════════════════════ */

#define SCHOOLBOOK_ROW_START(a_src) \
    { ZEROEXT_DSZ64_DR(RDX, a_src), ZEROEXT_DSZ64_DR(RDI, a_src), \
      NOP, NOP_SEQWORD }

#define MUL_BLOCK \
    { MUL_DSZ64_DRR(RCX, TMP10, RDX), \
      ZEROEXT_DSZ64_DR(TMP0, RDX), \
      ZEROEXT_DSZ64_DR(TMP1, RCX), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(RDX, RDI), \
      MUL_DSZ64_DRR(RCX, TMP11, RDX), \
      ZEROEXT_DSZ64_DR(TMP2, RDX), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP3, RCX), \
      ZEROEXT_DSZ64_DR(RDX, RDI), \
      MUL_DSZ64_DRR(RCX, TMP12, RDX), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(TMP4, RDX), \
      ZEROEXT_DSZ64_DR(TMP5, RCX), \
      ZEROEXT_DSZ64_DR(RDX, RDI), NOP_SEQWORD }, \
    { MUL_DSZ64_DRR(RCX, TMP13, RDX), \
      ZEROEXT_DSZ64_DR(TMP6, RDX), \
      ZEROEXT_DSZ64_DR(TMP7, RCX), NOP_SEQWORD }

#define COMBINED_CHAIN \
    { ADD_DSZ64_DRR(TMP1, TMP1, TMP2), GENARITHFLAGS_RR(TMP1, TMP1), \
      ADC_DSZ64_DRR(TMP3, TMP3, TMP4), NOP_SEQWORD }, \
    { GENARITHFLAGS_RR(TMP3, TMP3), ADC_DSZ64_DRR(TMP5, TMP5, TMP6), \
      GENARITHFLAGS_RR(TMP5, TMP5), NOP_SEQWORD }, \
    { ADC_DSZ64_DRR(TMP7, TMP7, TMP9), ADD_DSZ64_DRR(TMP0, R15, TMP0), \
      GENARITHFLAGS_RR(TMP0, TMP0), NOP_SEQWORD }, \
    { ADC_DSZ64_DRR(TMP1, R9, TMP1), GENARITHFLAGS_RR(TMP1, TMP1), \
      ADC_DSZ64_DRR(TMP3, R10, TMP3), NOP_SEQWORD }, \
    { GENARITHFLAGS_RR(TMP3, TMP3), ADC_DSZ64_DRR(TMP5, R13, TMP5), \
      GENARITHFLAGS_RR(TMP5, TMP5), NOP_SEQWORD }, \
    { ADC_DSZ64_DRR(TMP7, RAX, TMP7), GENARITHFLAGS_RR(TMP7, TMP7), \
      ADC_DSZ64_DRR(TMP14, TMP9, TMP9), NOP_SEQWORD }

#define SHIFT_WRITEBACK_MERGED(save_reg) \
    { ZEROEXT_DSZ64_DR(save_reg, TMP0), ZEROEXT_DSZ64_DR(R15, TMP1), \
      ZEROEXT_DSZ64_DR(R9, TMP3), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(R10, TMP5), ZEROEXT_DSZ64_DR(R13, TMP7), \
      ZEROEXT_DSZ64_DR(RAX, TMP14), NOP_SEQWORD }

#define ROW3_WRITEBACK \
    { ZEROEXT_DSZ64_DR(R15, TMP0), ZEROEXT_DSZ64_DR(R9, TMP1), \
      ZEROEXT_DSZ64_DR(R10, TMP3), NOP_SEQWORD }, \
    { ZEROEXT_DSZ64_DR(R13, TMP5), ZEROEXT_DSZ64_DR(RAX, TMP7), \
      NOP, NOP_SEQWORD }

static void install_mul_patch(void) {
    ucode_t patch[] = {
    { ZEROEXT_DSZ64_DR(TMP10, RSI), ZEROEXT_DSZ64_DR(TMP11, R12),
      ZEROEXT_DSZ64_DR(TMP12, R11), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP13, R14), ZEROEXT_DSZ64_DR(R14, RDX),
      ZEROEXT_DSZ64_DR(TMP15, RBP), NOP_SEQWORD },
    { ZEROEXT_DSZ32_DI(TMP9, 0), ZEROEXT_DSZ32_DI(TMP14, 0),
      NOP, NOP_SEQWORD },

    SCHOOLBOOK_ROW_START(RDI), MUL_BLOCK, COMBINED_CHAIN, SHIFT_WRITEBACK_MERGED(RSI),
    SCHOOLBOOK_ROW_START(R14), MUL_BLOCK, COMBINED_CHAIN, SHIFT_WRITEBACK_MERGED(R12),
    SCHOOLBOOK_ROW_START(TMP15), MUL_BLOCK, COMBINED_CHAIN, SHIFT_WRITEBACK_MERGED(R11),
    SCHOOLBOOK_ROW_START(RBX), MUL_BLOCK, COMBINED_CHAIN, ROW3_WRITEBACK,

    { ADD_DSZ64_DRR(RAX, RAX, TMP14), MUL_DSZ64_DIR(RCX, 38, R9), NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP0, RCX), MUL_DSZ64_DIR(RCX, 38, R10), NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP1, RCX), MUL_DSZ64_DIR(RCX, 38, R13), NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(TMP2, RCX), MUL_DSZ64_DIR(RCX, 38, RAX), NOP, NOP_SEQWORD },

    { ADD_DSZ64_DRR(TMP0, TMP0, R10), GENARITHFLAGS_RR(TMP0, TMP0),
      ADC_DSZ64_DRR(TMP1, TMP1, R13), NOP_SEQWORD },
    { GENARITHFLAGS_RR(TMP1, TMP1), ADC_DSZ64_DRR(TMP2, TMP2, RAX),
      GENARITHFLAGS_RR(TMP2, TMP2), NOP_SEQWORD },
    { ADC_DSZ64_DRR(RCX, RCX, TMP9), ADD_DSZ64_DRR(TMP3, RSI, R9),
      GENARITHFLAGS_RR(TMP3, TMP3), NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP4, R12, TMP0), GENARITHFLAGS_RR(TMP4, TMP4),
      ADC_DSZ64_DRR(TMP5, R11, TMP1), NOP_SEQWORD },
    { GENARITHFLAGS_RR(TMP5, TMP5), ADC_DSZ64_DRR(TMP6, R15, TMP2),
      GENARITHFLAGS_RR(TMP6, TMP6), NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP7, RCX, TMP9), NOP, NOP, NOP_SEQWORD },

    { MUL_DSZ64_DIR(RCX, 38, TMP7), NOP, NOP, NOP_SEQWORD },
    { ADD_DSZ64_DRR(TMP3, TMP3, TMP7), GENARITHFLAGS_RR(TMP3, TMP3),
      ADC_DSZ64_DRR(TMP4, TMP4, TMP9), NOP_SEQWORD },
    { GENARITHFLAGS_RR(TMP4, TMP4), ADC_DSZ64_DRR(TMP5, TMP5, TMP9),
      GENARITHFLAGS_RR(TMP5, TMP5), NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP6, TMP6, TMP9), NOP, NOP, NOP_SEQWORD },

    { ZEROEXT_DSZ64_DR(R15, TMP3), ZEROEXT_DSZ64_DR(R9, TMP4),
      ZEROEXT_DSZ64_DR(R10, TMP5), NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(R13, TMP6), NOP, NOP, END_SEQWORD }
    };
    patch_ucode(0x7c00, patch, ARRAY_SZ(patch));
    hook_match_and_patch(0, 0x0cd8, 0x7c00);
    printf("4x64 mul patch: %d triads at U7c00 (vmwrite hook)\n", (int)ARRAY_SZ(patch));
}

/* ════════════════════════════════════════════════════════════════════
 * 4×64 MICROCODE FIELD OPS — exported (non-static) so the hybrid's
 * amd64-64-ucode/fe25519_{mul,square}.c wrappers can call them.
 * ════════════════════════════════════════════════════════════════════ */

/* fe_mul via microcode. Output unreduced relative to p (< 2^256). */
void fe_mul_ucode4(uint64_t *out, const uint64_t *a, const uint64_t *b) {
    uint64_t r[4];
    register uint64_t *_a   asm("rcx") = (uint64_t *)a;
    register uint64_t *_b   asm("rbx") = (uint64_t *)b;
    register uint64_t *_out asm("r15") = r;
    asm volatile(
        "push r15\n\t"
        "push rbp\n\t"
        "push rcx\n\t"
        "mov rsi, [rbx]\n\t"
        "mov r12, [rbx + 8]\n\t"
        "mov r11, [rbx + 16]\n\t"
        "mov r14, [rbx + 24]\n\t"
        "mov rdi, [rcx]\n\t"
        "mov rdx, [rcx + 8]\n\t"
        "mov rbp, [rcx + 16]\n\t"
        "mov rbx, [rcx + 24]\n\t"
        "mov r8, 38\n\t"
        "xor r15d, r15d\n\t"
        "xor r9d, r9d\n\t"
        "xor r10d, r10d\n\t"
        "xor r13d, r13d\n\t"
        "xor eax, eax\n\t"
        "vmwrite rcx, rdx\n\t"
        "pop rcx\n\t"
        "pop rbp\n\t"
        "pop rcx\n\t"
        "mov [rcx],      r15\n\t"
        "mov [rcx + 8],  r9\n\t"
        "mov [rcx + 16], r10\n\t"
        "mov [rcx + 24], r13\n\t"
        : "+r"(_a), "+r"(_b), "+r"(_out)
        :
        : "rax", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14",
          "memory", "cc"
    );
    out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = r[3];
}

void fe_sq_ucode4(uint64_t *out, const uint64_t *a) {
    fe_mul_ucode4(out, a, a);
}

/* ════════════════════════════════════════════════════════════════════
 * VERIFICATION + BENCH
 * ════════════════════════════════════════════════════════════════════ */

static void hex_to_bytes(const char *hex, uint8_t *out, int len) {
    for (int i = 0; i < len; i++) {
        unsigned int val;
        sscanf(hex + 2*i, "%02x", &val);
        out[i] = (uint8_t)val;
    }
}

static void print_hex(const char *label, const uint8_t *data, int len) {
    printf("  %s: ", label);
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
    printf("\n");
}

static int memcmp_hex(const uint8_t *data, const char *hex, int len) {
    uint8_t expected[32];
    hex_to_bytes(hex, expected, len);
    return memcmp(data, expected, len);
}

static int test_rfc7748(void) {
    int pass = 0, fail = 0;
    uint8_t scalar[32], point[32], r[32];

    printf("=== RFC 7748 Test Vectors (amd64-64 ladder + 4×64 microcode) ===\n\n");

    /* Vector 1 */
    printf("--- Vector 1 ---\n");
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);
    x25519_amd64_64_ucode(r, scalar, point);
    print_hex("got     ", r, 32);
    printf("  expect: c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552\n");
    if (memcmp_hex(r, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    /* Vector 2 */
    printf("\n--- Vector 2 ---\n");
    hex_to_bytes("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", scalar, 32);
    hex_to_bytes("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", point, 32);
    x25519_amd64_64_ucode(r, scalar, point);
    print_hex("got     ", r, 32);
    printf("  expect: 95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957\n");
    if (memcmp_hex(r, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    /* Iterated test (1 round) */
    printf("\n--- Iterated test (1 round, scalar=u=9) ---\n");
    uint8_t k[32] = {0}, u[32] = {0};
    k[0] = 9; u[0] = 9;
    x25519_amd64_64_ucode(r, k, u);
    print_hex("got     ", r, 32);
    printf("  expect: 422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079\n");
    if (memcmp_hex(r, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    /* Iterated test (1000 rounds) — full RFC 7748 §5.2 verification */
    printf("\n--- Iterated test (1000 rounds) ---\n");
    uint8_t kn[32], un[32];
    memcpy(kn, k, 32);
    memcpy(un, u, 32);
    for (int i = 0; i < 1000; i++) {
        x25519_amd64_64_ucode(r, kn, un);
        memcpy(un, kn, 32);
        memcpy(kn, r, 32);
    }
    print_hex("got     ", kn, 32);
    printf("  expect: 684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51\n");
    if (memcmp_hex(kn, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    printf("\n=== RFC 7748: %d / %d passed ===\n\n", pass, pass + fail);
    return fail;
}

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

#define BENCH_REPS 100

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(void) {
    printf("=== X25519: amd64-64 ladder + 4×64 chained-ADC microcode field ops ===\n\n");

    assign_to_core(0);
    init_match_and_patch();
    do_fix_IN_patch();
    install_mul_patch();
    printf("(fe_sq uses fe_mul(a,a) wrapper — no separate sq patch)\n\n");

    int fail = test_rfc7748();
    if (fail) {
        printf("Verification FAILED, aborting bench.\n");
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }

    /* Bench: time one X25519 op many times, report min/median */
    uint8_t scalar[32], point[32], out[32];
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);

    uint64_t samples[BENCH_REPS];
    /* warmup */
    for (int i = 0; i < 5; i++) x25519_amd64_64_ucode(out, scalar, point);

    for (int r = 0; r < BENCH_REPS; r++) {
        uint64_t t0 = rdtsc_start();
        x25519_amd64_64_ucode(out, scalar, point);
        uint64_t t1 = rdtsc_end();
        samples[r] = t1 - t0;
    }
    qsort(samples, BENCH_REPS, sizeof(samples[0]), cmp_u64);

    printf("--- Bench (%d reps) ---\n", BENCH_REPS);
    printf("min: %" PRIu64 " cyc\n", samples[0]);
    printf("median: %" PRIu64 " cyc\n", samples[BENCH_REPS/2]);
    printf("p90: %" PRIu64 " cyc\n", samples[BENCH_REPS*9/10]);
    printf("\nFor reference:\n");
    printf("  5×51 microcode (production): ~312000 cyc\n");
    printf("  amd64-64 (SUPERCOP, asm):    ~272000 cyc\n");

    init_match_and_patch();
    do_fix_IN_patch();
    return 0;
}
