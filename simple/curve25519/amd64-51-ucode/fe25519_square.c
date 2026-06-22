/* amd64-51 hybrid: replace amd64-51's fe25519_square.S with a thin C
 * wrapper that calls our microcode fe_sq (via vmread hook). */

#include <stdint.h>
#include "fe25519.h"

extern void fe_sq_ucode(const uint64_t *a, uint64_t *out);

void fe25519_square(fe25519 *r, const fe25519 *x) {
    fe_sq_ucode((const uint64_t *)x->v, (uint64_t *)r->v);
}
