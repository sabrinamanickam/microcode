/*
 * openssl_x25519.c — OpenSSL's own X25519, extracted so it can be benchmarked
 * without building all of libcrypto.
 *
 * This is NOT a substitution of OpenSSL field ops into our ladder. It is
 * OpenSSL's own base-2^51 ladder, inversion chain, cswap, and encoding, driving
 * OpenSSL's own assembly field kernels. It therefore belongs in the end to end
 * table, where implementations are allowed to differ in every respect, and not
 * in the controlled same ladder table.
 *
 * The body is lifted verbatim from openssl/crypto/ec/curve25519.c, the block
 * guarded by "#if defined(X25519_ASM) ... BASE_2_51_IMPLEMENTED", lines 263 to
 * 785 of that file, kept in openssl_x25519_core.inc so the extraction is
 * auditable and re-extractable rather than retyped.
 *
 * Defining X25519_ASM makes that block resolve fe51_mul/fe51_sq/fe51_mul121666
 * to the external symbols x25519_fe51_mul/_sqr/_mul121666 and NOT compile the
 * __uint128_t fallbacks at all, so the routines being measured are provably the
 * assembly ones from crypto/ec/asm/x25519-x86_64.pl. BASE_2_64_IMPLEMENTED is
 * deliberately left undefined: the fe64 path needs ADX, which this core lacks,
 * so OpenSSL would select the fe51 ladder here in any case.
 */
#include <stdint.h>
#include <string.h>

#define X25519_ASM 1

/* The only libcrypto symbol the extracted block needs. */
static void OPENSSL_cleanse(void *p, size_t n) { memset(p, 0, n); }

#include "openssl_x25519_core.inc"

void x25519_openssl(uint8_t out[32], const uint8_t scalar[32],
                    const uint8_t point[32])
{
    x25519_scalar_mult(out, scalar, point);
}
