/* test_new_contenders.c — check the OpenSSL and s2n-bignum X25519 contenders
 * against RFC 7748 without installing any microcode patch, so it runs without
 * root. The matrix binary gates on the same vector in process, but catching a
 * wrong contender here costs seconds instead of a full sweep. */
#define _GNU_SOURCE
#define INLINE2_CONTENDERS_ONLY
#include "full_curve25519_inline2.c"

int main(void) {
    uint8_t scalar[32], point[32], out[32];
    struct { const char *name; void (*fn)(uint8_t *, const uint8_t *, const uint8_t *); }
    t[] = {
        { "s2n-bignum/asm",   x25519_s2n     },
        { "openssl",          x25519_openssl },
        { "osslops/C-ladder", x25519_osslops },
        { "ours/hand-C",      x25519_native  },
    };
    struct { const char *s, *p, *want; } v[] = {
      { "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
        "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
        "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552" },
      { "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
        "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
        "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957" },
    };
    int fail = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        for (size_t j = 0; j < sizeof v / sizeof v[0]; j++) {
            hex_to_bytes(v[j].s, scalar, 32);
            hex_to_bytes(v[j].p, point, 32);
            t[i].fn(out, scalar, point);
            int ok = memcmp_hex(out, v[j].want, 32) == 0;
            printf("  %-18s vector %zu  %s\n", t[i].name, j + 1, ok ? "PASS" : "FAIL");
            if (!ok) { print_hex("    got ", out, 32); fail++; }
        }
    }
    printf("\n%s\n", fail ? "FAILURES PRESENT" : "all new contenders reproduce RFC 7748");
    return fail != 0;
}
