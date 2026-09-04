/*
 * probe_a51ops.c — verify + bench the "a51ops/C-ladder" control WITHOUT
 * touching microcode, so it runs on any CPU with no red-unlock and no sudo.
 *
 * full_curve25519_inline2.c is included as a library (INLINE2_CONTENDERS_ONLY strips its
 * main/profiler), which brings in x25519_a51ops — Bernstein-Schwabe's amd64-51
 * hand-asm fe25519_mul.S / fe25519_square.S on the shared C Montgomery ladder
 * — plus the pure-native contenders that share that exact ladder. No patch is
 * installed and no vmwrite is fired, so nothing here needs patch RAM.
 *
 * What this establishes on its own: that the control is correct (RFC 7748,
 * incl. the 1000-iteration chain that gates the 5x51 bound-discipline
 * compatibility) and how it sits against the other same-ladder backends.
 * The microcode arm (ucode/C-ladder) is measured by the full matrix run.
 *
 * Build: make PROG=probe_a51ops
 * Run:   taskset -c 0 ./probe_a51ops_static      (no sudo needed)
 */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"

#define REPS 100

int main(void) {
    printf("=== a51ops/C-ladder control — verify + bench (no microcode) ===\n\n");
    assign_to_core(0);

    uint8_t scalar[32], point[32], out[32], r[32];
    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);

    int pass = 0, fail = 0;

    x25519_a51ops(r, scalar, point);
    print_hex("vector 1", r, 32);
    if (memcmp_hex(r, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32) == 0) {
        printf("  vector 1: PASS\n"); pass++;
    } else { printf("  vector 1: FAIL\n"); fail++; }

    uint8_t s2[32], p2[32];
    hex_to_bytes("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", s2, 32);
    hex_to_bytes("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", p2, 32);
    x25519_a51ops(r, s2, p2);
    print_hex("vector 2", r, 32);
    if (memcmp_hex(r, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", 32) == 0) {
        printf("  vector 2: PASS\n"); pass++;
    } else { printf("  vector 2: FAIL\n"); fail++; }

    /* The 1000-iteration chain — the real gate on 5x51 bound compatibility
     * between our C ladder's fe_add/fe_sub and amd64-51's asm mul/square. */
    uint8_t k[32] = {0}, u[32] = {0};
    k[0] = 9; u[0] = 9;
    for (int i = 0; i < 1000; i++) {
        x25519_a51ops(r, k, u);
        memcpy(u, k, 32);
        memcpy(k, r, 32);
    }
    print_hex("iter1000", k, 32);
    if (memcmp_hex(k, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", 32) == 0) {
        printf("  iterated 1000: PASS\n"); pass++;
    } else { printf("  iterated 1000: FAIL\n"); fail++; }

    /* amd64-51/asm-Clad is native too, so it can be gated here as well.
     * (amd64-51/ucode-Clad needs the patch — the matrix run covers it.) */
    uint8_t kc[32] = {0}, uc[32] = {0};
    kc[0] = 9; uc[0] = 9;
    for (int i = 0; i < 1000; i++) {
        x25519_amd64_51_asmclad(r, kc, uc);
        memcpy(uc, kc, 32);
        memcpy(kc, r, 32);
    }
    print_hex("a51/asmCld 1000", kc, 32);
    if (memcmp_hex(kc, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", 32) == 0) {
        printf("  a51/asm-Clad iterated 1000: PASS\n"); pass++;
    } else { printf("  a51/asm-Clad iterated 1000: FAIL\n"); fail++; }

    printf("\n=== RFC 7748: %d / %d passed ===\n\n", pass, pass + fail);
    if (fail) return 1;

    /* Same-ladder, same-process bench of the native backends. */
    uint64_t smp[REPS], mn, med, p10, p90;

    for (int i = 0; i < 5; i++) {
        x25519_a51ops(out, scalar, point);
        x25519_cryptopt(out, scalar, point);
        x25519_fiat(out, scalar, point);
        x25519_native(out, scalar, point);
        x25519_amd64_51_asmclad(out, scalar, point);
        x25519_amd64_51(out, scalar, point);
    }

#define B(LABEL, CALL) do {                                                   \
        for (int q = 0; q < REPS; q++) {                                      \
            uint64_t t0 = rdtsc_start(); CALL; uint64_t t1 = rdtsc_end();     \
            smp[q] = t1 - t0;                                                 \
        }                                                                     \
        bench_stats(smp, REPS, &mn, &med, &p10, &p90);                        \
        printf("%-20s median %8" PRIu64 "  min %8" PRIu64 "\n", LABEL, med, mn); \
    } while (0)

    printf("--- Same C ladder, only the field op differs (%d reps) ---\n", REPS);
    B("a51ops/C-ladder:", x25519_a51ops(out, scalar, point));
    B("a51/asm-Clad:",    x25519_amd64_51_asmclad(out, scalar, point));
    B("amd64-51/asm:",    x25519_amd64_51(out, scalar, point));
    B("ours/cryptopt:",   x25519_cryptopt(out, scalar, point));
    B("ours/fiat:",       x25519_fiat(out, scalar, point));
    B("ours/hand-C:",     x25519_native(out, scalar, point));
#undef B
    printf("\n(ucode/C-ladder needs the patch — run the full matrix for that arm.)\n");
    return 0;
}
