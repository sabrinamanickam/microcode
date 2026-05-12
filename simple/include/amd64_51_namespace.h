/* Force-included via -include when building SUPERCOP amd64-51 sources.
 * SUPERCOP normally generates a per-impl mangling header at build time;
 * we replicate it with two #defines:
 *   - CRYPTO_NAMESPACE prefixes symbols used by amd64-51's asm + headers
 *     (fe25519_mul → supercop_amd64_51_fe25519_mul, etc.)
 *   - crypto_scalarmult is also renamed to x25519_amd64_51 so the public
 *     entry from mont25519.c doesn't clash with our other implementations. */

#ifndef AMD64_51_NAMESPACE_H
#define AMD64_51_NAMESPACE_H

#define CRYPTO_NAMESPACE(x)  supercop_amd64_51_##x
/* macOS prepends `_` to symbols; on Linux ELF the _CRYPTO_NAMESPACE
 * lines are dead weight. Map them to a unique unused name so GAS
 * accepts the labels/.globl directives. */
#define _CRYPTO_NAMESPACE(x) supercop_amd64_51_unused_##x
#define crypto_scalarmult      x25519_amd64_51
#define crypto_scalarmult_base x25519_amd64_51_base

#endif
