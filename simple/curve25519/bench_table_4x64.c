/*
 * bench_table_4x64.c — the 4x64 saturated block of the CONTROLLED table.
 *
 * Two arms, same C ladder, field backend the only difference:
 *
 *   amd64-64 asm   amd64-64-ucode/ladderstep.c + amd64-64's qhasm mul/square
 *   microcode      amd64-64-ucode/ladderstep.c + 4x64 chained-ADC microcode
 *
 * Both arms take ladderstep.c from the SAME source file (the Makefile compiles
 * AMD64A's ladderstep from $(AMD64U_DIR)/ladderstep.c), and both reuse
 * amd64-64's own driver, fe25519_invert, pack/unpack/setint, fe25519_freeze
 * and work_cswap. So within this block the ladder, driver, inversion, cswap
 * and packing are identical and only fe_mul / fe_sq change.
 *
 * WHY THIS IS A SEPARATE BINARY: the 4x64 mul patch is 75 triads at U7c00 and
 * the 5x51 pair is 66 (U7c00) + 42 (U7d08) = 108 triads, also based at U7c00.
 * They cannot coexist under the 128-triad patch RAM cap, so the 5x51 block
 * (bench_table.c) and this 4x64 block cannot share a process. Each block is
 * internally same-process, which is what "controlled" requires; the blocks are
 * never compared to each other. bench_tables.sh runs both and also carries
 * stock amd64-64/asm in BOTH binaries as a cross-process anchor so the two
 * processes can be shown to agree.
 *
 * Build: make PROG=bench_table_4x64
 * Run:   sudo taskset -c 0 ./bench_table_4x64_static
 */

#define _GNU_SOURCE
#define A64U_LIB                       /* strip the standalone main() */
#include "full_curve25519_amd64_64_ucode.c"
#include "include/freq_guard.h"

/* amd64-64/asm-Clad: the asm arm of this block (C ladder + qhasm mul/square). */
extern int x25519_amd64_64_asmclad(unsigned char *out,
                                   const unsigned char *scalar,
                                   const unsigned char *point);
/* Stock amd64-64 (qhasm ladderstep.S). NOT part of the controlled block --
 * carried only as the cross-process anchor shared with bench_table.c. */
extern int x25519_amd64_64(unsigned char *out,
                           const unsigned char *scalar,
                           const unsigned char *point);

#define REPS      1000
#define VERIFY_N  1000

typedef void (*row_fn)(uint8_t *out, const uint8_t *scalar, const uint8_t *point);

#define ROW_FN(NAME, CALL) \
    static void NAME(uint8_t *o, const uint8_t *s, const uint8_t *p) { CALL; }

ROW_FN(r4_asm,    (void)x25519_amd64_64_asmclad(o, s, p))
ROW_FN(r4_ucode,  (void)x25519_amd64_64_ucode(o, s, p))
ROW_FN(r4_anchor, (void)x25519_amd64_64(o, s, p))

/* tag: which table the row belongs to, consumed by bench_tables.sh. */
static const struct {
    const char *tag; const char *backend; const char *detail; row_fn fn;
} TABLE[] = {
    { "ctrl4x64", "amd64-64 asm", "amd64-64 qhasm fe25519_{mul,square}.S", r4_asm    },
    { "ctrl4x64", "microcode",    "4x64 chained-ADC microcode (sq=mul(a,a))", r4_ucode  },
    { "anchor",   "amd64-64/asm", "cross-process anchor",                  r4_anchor },
};
#define N_ROWS ((int)(sizeof TABLE / sizeof TABLE[0]))

/* ---- stats (cmp_u64 lives inside the A64U_LIB guard, so define our own) --- */

static int bt_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t bt_pctl(const uint64_t *sorted, int n, double pct)
{
    int i = (int)(pct / 100.0 * (n - 1) + 0.5);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return sorted[i];
}

/* ---- RFC 7748 gate ------------------------------------------------------- */

static int verify_row(row_fn f, const char *label)
{
    static const char *V1_S = "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4";
    static const char *V1_U = "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c";
    static const char *V1_R = "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552";
    static const char *V2_S = "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d";
    static const char *V2_U = "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493";
    static const char *V2_R = "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957";
    static const char *IT1  = "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079";
    static const char *IT_N = "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51";

    uint8_t s[32], u[32], r[32];
    int ok_v1, ok_v2, ok_i1, ok_in;

    hex_to_bytes(V1_S, s, 32); hex_to_bytes(V1_U, u, 32);
    f(r, s, u); ok_v1 = (memcmp_hex(r, V1_R, 32) == 0);

    hex_to_bytes(V2_S, s, 32); hex_to_bytes(V2_U, u, 32);
    f(r, s, u); ok_v2 = (memcmp_hex(r, V2_R, 32) == 0);

    { uint8_t k[32] = {0}, n[32] = {0}; k[0] = 9; n[0] = 9;
      f(r, k, n); ok_i1 = (memcmp_hex(r, IT1, 32) == 0); }
    { uint8_t k[32] = {0}, n[32] = {0}; k[0] = 9; n[0] = 9;
      for (int i = 0; i < VERIFY_N; i++) {
          f(r, k, n); memcpy(n, k, 32); memcpy(k, r, 32);
      }
      ok_in = (memcmp_hex(k, IT_N, 32) == 0); }

    int fails = !ok_v1 + !ok_v2 + !ok_i1 + !ok_in;
    printf("  %-14s vec1 %-4s vec2 %-4s iter1 %-4s iter%d %-4s %s\n",
           label, ok_v1 ? "ok" : "FAIL", ok_v2 ? "ok" : "FAIL",
           ok_i1 ? "ok" : "FAIL", VERIFY_N, ok_in ? "ok" : "FAIL",
           fails ? "<-- FAILED" : "");
    return fails;
}

/* ---- main --------------------------------------------------------------- */

static uint64_t samples[N_ROWS][REPS];

int main(void)
{
    uint8_t scalar[32], point[32], out[32];

    printf("=== 4x64 saturated block (%d reps, interleaved, one process) ===\n\n", REPS);

    assign_to_core(0);
    if (frequency_guard()) return 2;

    init_match_and_patch();
    do_fix_IN_patch();
    install_mul_patch();               /* 4x64 chained-ADC fe_mul */
    printf("(fe_sq uses fe_mul(a,a) -- no separate sq patch)\n\n");

    printf("--- RFC 7748 ---\n");
    int fails = 0;
    for (int i = 0; i < N_ROWS; i++)
        fails += verify_row(TABLE[i].fn, TABLE[i].backend);
    if (fails) {
        printf("\n%d check(s) FAILED -- benchmark skipped.\n", fails);
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }
    printf("\nAll %d arms: 4/4 RFC 7748.\n\n", N_ROWS);

    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);

    for (int i = 0; i < N_ROWS; i++)
        TABLE[i].fn(out, scalar, point);

    /* Interleaved: repetition index is the OUTER loop, so frequency drift and
     * any periodic perturbation hit every arm equally. */
    for (int r = 0; r < REPS; r++)
        for (int i = 0; i < N_ROWS; i++) {
            uint64_t t0 = rdtsc_start();
            TABLE[i].fn(out, scalar, point);
            uint64_t t1 = rdtsc_end();
            samples[i][r] = t1 - t0;
        }

    printf("--- results ---\n");
    for (int i = 0; i < N_ROWS; i++) {
        qsort(samples[i], REPS, sizeof(uint64_t), bt_cmp_u64);
        printf("DATA %s|%s|%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|%s\n",
               TABLE[i].tag, TABLE[i].backend,
               samples[i][REPS / 2], samples[i][0],
               bt_pctl(samples[i], REPS, 10.0), bt_pctl(samples[i], REPS, 90.0),
               TABLE[i].detail);
    }

    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}

/* test_rfc7748 comes in with the include but this file has its own gate;
 * reference it once so -Wall stays quiet. */
__attribute__((used)) static void bt4_unused_(void) { if (0) (void)test_rfc7748(); }
