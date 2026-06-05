/* amd64-64 hybrid: replace amd64-64's fe25519_mul.S with a thin C
 * wrapper that calls our 4×64 microcode fe_mul (via vmwrite hook).
 *
 * amd64-64's fe25519 is 4×64 saturated (radix 2^64), exactly matching the
 * 4×64 chained-ADC microcode patch — so this is a true drop-in with no
 * representation conversion (unlike the 5×51 hybrid).
 *
 * The cast from `unsigned long long *` to `uint64_t *` is safe in practice
 * on Linux x86-64 (both are 64-bit, same representation, same alignment). */

#include <stdint.h>
#include "fe25519.h"

extern void fe_mul_ucode4(uint64_t *out, const uint64_t *a, const uint64_t *b);

void fe25519_mul(fe25519 *r, const fe25519 *x, const fe25519 *y) {
    fe_mul_ucode4((uint64_t *)r->v,
                  (const uint64_t *)x->v,
                  (const uint64_t *)y->v);
}
