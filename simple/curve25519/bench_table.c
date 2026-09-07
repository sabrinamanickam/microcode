/*
 * bench_table.c — the 5x51 block of the CONTROLLED table, plus the END-TO-END
 * table. Run by bench_tables.sh, which pairs it with bench_table_4x64.c (the
 * 4x64 saturated block) and formats both tables.
 *
 * TABLE 1 rows emitted here: the 5x51 controlled block. All five arms use our
 * common backend-neutral C Montgomery ladder -- the same 55-line ladder body,
 * verified byte-identical across the five copies modulo the backend suffix --
 * plus the same driver, the same 254-squaring/11-mul inversion chain, the same
 * branchless fe_cswap and the same packing. Only fe_mul and fe_sq change.
 *
 *   microcode      5x51 microcode field ops
 *   fiat           fiat-crypto generated C (../../curvesC/curve25519_{mul,square}.c)
 *   CryptOpt       CryptOpt-generated asm, Goldmont-tuned
 *   hand-C         our hand-written 5x51 C
 *   amd64-51 asm   Bernstein-Schwabe amd64-51 fe25519_{mul,square}.S
 *
 * The register-chained inline-asm ladder is deliberately NOT in the controlled
 * block: using it would change the ladder and the field backend at once. It
 * appears only in TABLE 2 as "ours/ucode", which is what it is -- our best
 * end-to-end implementation.
 *
 * TABLE 2 rows emitted here: end-to-end, whole implementations. These differ
 * in ladder, driver, inversion, cswap and packing as well as field ops; they
 * answer "what is the fastest X25519 available on this part", not "what does
 * the field backend buy". Our microcode appears here in its 5x51 form only
 * ("ours/ucode"); the 4x64 hybrid is a TABLE 1 controlled arm, not an
 * end-to-end contender.
 *
 * amd64-64/asm is emitted twice -- once as an end-to-end row and once under
 * the "anchor" tag -- because bench_table_4x64.c benches it too. Comparing the
 * two lets bench_tables.sh show the two processes are equivalent, so TABLE 1's
 * two blocks were measured under matched conditions.
 *
 * full_curve25519_inline2.c is #included as a library (INLINE2_CONTENDERS_ONLY
 * strips its main and profiler) purely to reuse the already-verified backends,
 * the 5x51 patch install and the rdtsc/stats helpers.
 *
 * Build: make PROG=bench_table
 * Run:   sudo taskset -c 0 ./bench_table_static
 */

#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"
#include "include/freq_guard.h"

#define REPS      1000   /* samples per arm */
#define VERIFY_N  1000   /* RFC 7748 iterated-chain length */

typedef void (*row_fn)(uint8_t *out, const uint8_t *scalar, const uint8_t *point);

/* Uniform wrapper so int-returning and void-returning backends share a type. */
#define ROW_FN(NAME, CALL) \
    static void NAME(uint8_t *o, const uint8_t *s, const uint8_t *p) { CALL; }

/* --- the five 5x51 controlled arms: common C ladder, backend varies --- */
ROW_FN(r_ucode,      x25519_ucode(o, s, p))
ROW_FN(r_fiat,       x25519_fiat(o, s, p))
ROW_FN(r_cryptopt,   x25519_cryptopt(o, s, p))
ROW_FN(r_hand_c,     x25519_native(o, s, p))
ROW_FN(r_a51ops,     x25519_a51ops(o, s, p))
/* --- end-to-end whole implementations --- */
ROW_FN(r_ours_ucode, x25519(o, s, p))            /* inline-asm ladder */
ROW_FN(r_a64_asm,    (void)x25519_amd64_64(o, s, p))
ROW_FN(r_a51_asm,    (void)x25519_amd64_51(o, s, p))
ROW_FN(r_donna,      (void)x25519_donna_c64(o, s, p))

static const struct {
    const char *tag;      /* which table: ctrl5x51 | e2e */
    const char *name;     /* field backend (ctrl) or implementation (e2e) */
    const char *detail;   /* human note */
    row_fn      fn;
} TABLE[] = {
    { "ctrl5x51", "microcode",    "5x51 microcode field ops",            r_ucode      },
    { "ctrl5x51", "fiat",         "fiat-crypto generated C",             r_fiat       },
    { "ctrl5x51", "CryptOpt",     "CryptOpt generated asm (Goldmont)",   r_cryptopt   },
    { "ctrl5x51", "hand-C",       "our hand-written 5x51 C",             r_hand_c     },
    { "ctrl5x51", "amd64-51 asm", "Bernstein-Schwabe fe25519_*.S",       r_a51ops     },

    { "e2e", "ours/ucode",   "inline-asm 5x51 ladder + 5x51 microcode",  r_ours_ucode },
    { "e2e", "amd64-64/asm", "SUPERCOP amd64-64, stock (qhasm, 4x64)",   r_a64_asm    },
    { "e2e", "amd64-51/asm", "SUPERCOP amd64-51, stock (qhasm, 5x51)",   r_a51_asm    },
    { "e2e", "donna_c64",    "SUPERCOP donna_c64, stock (Langley C)",    r_donna      },
};
#define N_ROWS ((int)(sizeof TABLE / sizeof TABLE[0]))

/* Look a row up by name so nothing below depends on table order. */
static int row(const char *name)
{
    for (int i = 0; i < N_ROWS; i++)
        if (strcmp(TABLE[i].name, name) == 0) return i;
    fprintf(stderr, "bench_table: no such row '%s'\n", name);
    abort();
}

/* ---- RFC 7748 gate ------------------------------------------------------- */

/* Every arm must reproduce all four spec vectors. The VERIFY_N chain is the
 * one that actually gates 5x51 limb-bound compatibility between the common
 * ladder's fe_add/fe_sub and each backend's mul/square -- the single-shot
 * vectors pass even when the bound discipline is subtly wrong. */
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
    uint64_t med[N_ROWS], mn[N_ROWS], p10[N_ROWS], p90[N_ROWS];

    printf("=== 5x51 block + end-to-end (%d reps, interleaved, one process) ===\n\n",
           REPS);

    assign_to_core(0);
    if (frequency_guard()) return 2;

    init_match_and_patch();
    do_fix_IN_patch();
    install_field_patches();          /* the 5x51 mul + sq patches */
    printf("\n");

    printf("--- RFC 7748 (all %d arms) ---\n", N_ROWS);
    int fails = 0;
    for (int i = 0; i < N_ROWS; i++)
        fails += verify_row(TABLE[i].fn, TABLE[i].name);
    if (fails) {
        printf("\n%d check(s) FAILED -- benchmark skipped.\n", fails);
        init_match_and_patch(); do_fix_IN_patch();
        return 1;
    }
    printf("\nAll %d arms: 4/4 RFC 7748.\n\n", N_ROWS);

    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);

    for (int i = 0; i < N_ROWS; i++)          /* warm every arm first */
        TABLE[i].fn(out, scalar, point);

    /* Interleaved: repetition index is the OUTER loop, so frequency drift and
     * any periodic perturbation hit every arm equally. Absolute counts are
     * only cycles because frequency_guard() confirmed TSC == core clock. */
    for (int r = 0; r < REPS; r++)
        for (int i = 0; i < N_ROWS; i++) {
            uint64_t t0 = rdtsc_start();
            TABLE[i].fn(out, scalar, point);
            uint64_t t1 = rdtsc_end();
            samples[i][r] = t1 - t0;
        }

    for (int i = 0; i < N_ROWS; i++)
        bench_stats(samples[i], REPS, &mn[i], &med[i], &p10[i], &p90[i]);

    printf("--- results ---\n");
    for (int i = 0; i < N_ROWS; i++)
        printf("DATA %s|%s|%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|%s\n",
               TABLE[i].tag, TABLE[i].name,
               med[i], mn[i], p10[i], p90[i], TABLE[i].detail);

    /* amd64-64/asm again under the anchor tag: bench_table_4x64.c benches the
     * same implementation, so the script can show the two processes agree. */
    {
        int a = row("amd64-64/asm");
        printf("DATA anchor|amd64-64/asm|%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|%" PRIu64 "|cross-process anchor\n",
               med[a], mn[a], p10[a], p90[a]);
    }

    init_match_and_patch();
    do_fix_IN_patch();
    printf("\nDone.\n");
    return 0;
}

/* inline2's own harness and RFC-7748 routine come along with the include but
 * are unused here (this file has its own). Reference them once for -Wall. */
__attribute__((used)) static void bench_table_unused_(void)
{
    (void)bench_samples;
    if (0) { benchmark(); (void)test_rfc7748(); }
}
