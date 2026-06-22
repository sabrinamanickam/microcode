/* Force-included via -include when building the amd64-64-UCODE hybrid.
 *
 * Same idea as amd64_64_namespace.h but with a distinct prefix so the
 * hybrid's symbols don't collide with the original amd64-64's symbols
 * (both can be linked into the same binary). 4×64 saturated throughout —
 * the microcode field ops are the 4×64 (radix 2^64) chained-ADC patch,
 * which exactly matches amd64-64's `fe25519 { unsigned long long v[4]; }`,
 * so no representation conversion is needed.
 *
 * Hybrid layout:
 *   - mont25519.c, fe25519_invert.c, pack/unpack/setint.c, consts.c,
 *     fe25519_freeze.S, work_cswap.S come from amd64-64 (recompiled
 *     with this namespace).
 *   - ladderstep.c, fe25519_mul.c, fe25519_square.c come from
 *     simple/amd64-64-ucode/ and call our 4×64 microcode wrappers.
 *
 * The public entry point is x25519_amd64_64_ucode. */

#ifndef AMD64_64_UCODE_NAMESPACE_H
#define AMD64_64_UCODE_NAMESPACE_H

#define CRYPTO_NAMESPACE(x)  supercop_amd64_64_ucode_##x
#define _CRYPTO_NAMESPACE(x) supercop_amd64_64_ucode_unused_##x
#define crypto_scalarmult      x25519_amd64_64_ucode
#define crypto_scalarmult_base x25519_amd64_64_ucode_base
/* mont25519.c locally `#define`s these; force-define them globally so our
 * own ladderstep.c (which doesn't include mont25519.c) defines the
 * properly-prefixed symbol that mont25519.c looks for at link time. */
#define ladderstep           CRYPTO_NAMESPACE(ladderstep)
#define work_cswap           CRYPTO_NAMESPACE(work_cswap)

#endif
