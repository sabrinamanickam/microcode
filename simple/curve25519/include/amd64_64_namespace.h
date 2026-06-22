/* Force-included via -include when building SUPERCOP amd64-64 sources.
 * SUPERCOP normally generates a per-impl mangling header at build time;
 * we replicate it with two #defines:
 *   - CRYPTO_NAMESPACE prefixes symbols used by amd64-64's asm + headers
 *     (fe25519_mul -> supercop_amd64_64_fe25519_mul, etc.)
 *   - crypto_scalarmult is also renamed to x25519_amd64_64 so the public
 *     entry from mont25519.c doesn't clash with our other implementations. */

#ifndef AMD64_64_NAMESPACE_H
#define AMD64_64_NAMESPACE_H

#define CRYPTO_NAMESPACE(x)  supercop_amd64_64_##x
#define _CRYPTO_NAMESPACE(x) supercop_amd64_64_unused_##x
#define crypto_scalarmult      x25519_amd64_64
#define crypto_scalarmult_base x25519_amd64_64_base

#endif
