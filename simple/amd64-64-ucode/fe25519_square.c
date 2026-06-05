/* amd64-64 hybrid: replace amd64-64's fe25519_square.S with a thin C
 * wrapper that calls our 4×64 microcode fe_sq (= fe_mul(a,a); the 4×64
 * patch has no separate squaring symmetry, fired via the vmwrite hook). */

#include <stdint.h>
#include "fe25519.h"

extern void fe_sq_ucode4(uint64_t *out, const uint64_t *a);

void fe25519_square(fe25519 *r, const fe25519 *x) {
    fe_sq_ucode4((uint64_t *)r->v, (const uint64_t *)x->v);
}
