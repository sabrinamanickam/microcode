/*
 * full_curve25519_amd64_64_asmclad.c — the amd64-64/asm-Clad CONTROL.
 *
 * Why this binary exists
 * ----------------------
 * `amd64-64/ucode` (full_curve25519_amd64_64_ucode.c) replaces THREE of
 * amd64-64's files: ladderstep.S, fe25519_mul.S and fe25519_square.S. So the
 * amd64-64/asm vs amd64-64/ucode ratio measures two changes at once — the
 * microcode field ops AND a 6,580-line qhasm ladder swapped for a 114-line C
 * one. That ratio cannot attribute anything to the field-op backend.
 *
 * This control supplies the missing middle point: the SAME C ladder as the
 * ucode hybrid (literally the same amd64-64-ucode/ladderstep.c object source,
 * recompiled under this namespace), but with amd64-64's OWN asm fe25519_mul.S
 * / fe25519_square.S restored.
 *
 *   amd64-64/asm       qhasm ladderstep.S + qhasm mul/square
 *   amd64-64/asm-Clad  C ladderstep.c    + qhasm mul/square   <-- this binary
 *   amd64-64/ucode     C ladderstep.c    + 4x64 microcode
 *
 *   asm-Clad / ucode = pure field-op effect, ladder held constant
 *   asm / asm-Clad   = the ladder-rewrite tax, measured once
 *
 * Pure native code: installs no microcode patch and needs no red-unlock. It
 * still runs under the same taskset/sudo harness as the other contenders so
 * the measurement conditions are identical.
 *
 * Standalone binary with bare "min:"/"median:" output so lib/build_run.sh's
 * run_standalone can slot it into the matrix.
 *
 * Build: make PROG=full_curve25519_amd64_64_asmclad
 * Run:   sudo taskset -c 0 ./full_curve25519_amd64_64_asmclad_static
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "../../../include/misc.h"

/* The control's entry point (mont25519.c, namespaced). */
extern int x25519_amd64_64_asmclad(unsigned char *out,
                                   const unsigned char *scalar,
                                   const unsigned char *point);

/* Stock SUPERCOP amd64-64 (qhasm ladderstep.S + qhasm mul/square), linked
 * into the SAME binary under its own namespace. Benching both arms in one
 * process makes the ladder-rewrite tax a same-process ratio — no cross-run
 * frequency state, the methodology the Keccak harness already uses. */
extern int x25519_amd64_64(unsigned char *out,
                           const unsigned char *scalar,
                           const unsigned char *point);

/* ════════════════════════════════════════════════════════════════════
 * VERIFICATION + BENCH  (same vectors/format as the ucode hybrid)
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

    printf("=== RFC 7748 Test Vectors (amd64-64 C ladder + amd64-64 asm field ops) ===\n\n");

    printf("--- Vector 1 ---\n");
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);
    x25519_amd64_64_asmclad(r, scalar, point);
    print_hex("got     ", r, 32);
    printf("  expect: c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552\n");
    if (memcmp_hex(r, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    printf("\n--- Vector 2 ---\n");
    hex_to_bytes("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", scalar, 32);
    hex_to_bytes("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", point, 32);
    x25519_amd64_64_asmclad(r, scalar, point);
    print_hex("got     ", r, 32);
    printf("  expect: 95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957\n");
    if (memcmp_hex(r, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    printf("\n--- Iterated test (1 round, scalar=u=9) ---\n");
    uint8_t k[32] = {0}, u[32] = {0};
    k[0] = 9; u[0] = 9;
    x25519_amd64_64_asmclad(r, k, u);
    print_hex("got     ", r, 32);
    printf("  expect: 422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079\n");
    if (memcmp_hex(r, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", 32) == 0) {
        printf("  PASS\n"); pass++;
    } else { printf("  FAIL\n"); fail++; }

    printf("\n--- Iterated test (1000 rounds) ---\n");
    uint8_t kn[32], un[32];
    memcpy(kn, k, 32);
    memcpy(un, u, 32);
    for (int i = 0; i < 1000; i++) {
        x25519_amd64_64_asmclad(r, kn, un);
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

#define BENCH_REPS 1000  /* matches the main harness; was 100 */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(void) {
    printf("=== X25519: amd64-64 C ladder + amd64-64 asm field ops (asm-Clad control) ===\n\n");

    assign_to_core(0);

    int fail = test_rfc7748();
    if (fail) {
        printf("Verification FAILED, aborting bench.\n");
        return 1;
    }

    uint8_t scalar[32], point[32], out[32];
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);

    /* Warm up both arms before timing either. */
    for (int i = 0; i < 5; i++) {
        x25519_amd64_64_asmclad(out, scalar, point);
        x25519_amd64_64(out, scalar, point);
    }

    /* Interleave the two arms rep-by-rep so any frequency drift over the run
     * hits both equally (same discipline as asm_op_keccak_vs.c). */
    uint64_t s_clad[BENCH_REPS], s_asm[BENCH_REPS];
    for (int r = 0; r < BENCH_REPS; r++) {
        uint64_t t0 = rdtsc_start();
        x25519_amd64_64_asmclad(out, scalar, point);
        uint64_t t1 = rdtsc_end();
        s_clad[r] = t1 - t0;

        t0 = rdtsc_start();
        x25519_amd64_64(out, scalar, point);
        t1 = rdtsc_end();
        s_asm[r] = t1 - t0;
    }
    qsort(s_clad, BENCH_REPS, sizeof(s_clad[0]), cmp_u64);
    qsort(s_asm,  BENCH_REPS, sizeof(s_asm[0]),  cmp_u64);

    uint64_t med_clad = s_clad[BENCH_REPS/2], med_asm = s_asm[BENCH_REPS/2];

    printf("--- Bench (%d reps, interleaved same-process) ---\n", BENCH_REPS);
    /* Bare min:/median: = the asm-Clad control, scraped by run_standalone. */
    printf("min: %" PRIu64 " cyc\n", s_clad[0]);
    printf("median: %" PRIu64 " cyc\n", med_clad);
    printf("p90: %" PRIu64 " cyc\n", s_clad[BENCH_REPS*9/10]);

    printf("\n--- Same-process reference arm ---\n");
    printf("amd64-64/asm       median %8" PRIu64 "  min %8" PRIu64 "  (qhasm ladderstep.S + qhasm mul/square)\n",
           med_asm, s_asm[0]);
    printf("amd64-64/asm-Clad  median %8" PRIu64 "  min %8" PRIu64 "  (C ladderstep.c    + qhasm mul/square)\n",
           med_clad, s_clad[0]);
    printf("\nladder-rewrite tax = asm-Clad / asm = %.4f  (%+.1f%% cycles from the ladder swap alone)\n",
           (double)med_clad / (double)med_asm,
           100.0 * ((double)med_clad / (double)med_asm - 1.0));
    printf("This is the component that amd64-64/asm vs amd64-64/ucode\n"
           "wrongly attributes to the microcode field ops.\n");
    return 0;
}
