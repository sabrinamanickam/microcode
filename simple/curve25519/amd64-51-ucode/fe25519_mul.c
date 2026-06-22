/* amd64-51 hybrid: replace amd64-51's fe25519_mul.S with a thin C
 * wrapper that calls our microcode fe_mul (via vmwrite hook).
 *
 * The cast from `unsigned long long *` to `uint64_t *` is safe in
 * practice on Linux x86-64 (both are 64-bit, same representation,
 * same alignment); gcc does not warn for this cast. */

#include <stdint.h>
#include "fe25519.h"

extern void fe_mul_ucode(const uint64_t *a, const uint64_t *b, uint64_t *out);

void fe25519_mul(fe25519 *r, const fe25519 *x, const fe25519 *y) {
    fe_mul_ucode((const uint64_t *)x->v,
                 (const uint64_t *)y->v,
                 (uint64_t *)r->v);
}
