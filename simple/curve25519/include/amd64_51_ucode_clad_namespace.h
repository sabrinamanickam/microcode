/* Force-included when building the amd64-51/ucode-Clad arm.
 *
 * The fourth corner of the 5x51 square: amd64-51's framework + the SAME C
 * ladderstep.c as amd64-51/asm-Clad + the 5x51 microcode field ops. Pairing it
 * with amd64-51/asm-Clad isolates the field-op backend inside amd64-51's own
 * framework; pairing it with amd64-51/ucode isolates the ladder coding style
 * (C vs register-chained inline asm) with the field ops held constant.
 *
 * This is what the retired full_curve25519 a51-hybrid path used to be; it is
 * restored here as a measured contender rather than a build variant.
 *
 * Layout: mont25519/invert/pack/unpack/setint/consts + freeze.S/cswap.S from
 * amd64-51; ladderstep.c from extra/amd64-51-ucode/; fe25519_mul.c and
 * fe25519_square.c from amd64-51-ucode/ (they call fe_mul_ucode / fe_sq_ucode,
 * which the hosting binary defines and whose patches it installs).
 *
 * Public entry point: x25519_amd64_51_ucode_clad. */

#ifndef AMD64_51_UCODE_CLAD_NAMESPACE_H
#define AMD64_51_UCODE_CLAD_NAMESPACE_H

#define CRYPTO_NAMESPACE(x)  supercop_amd64_51_ucode_clad_##x
#define _CRYPTO_NAMESPACE(x) supercop_amd64_51_ucode_clad_unused_##x
#define crypto_scalarmult      x25519_amd64_51_ucode_clad
#define crypto_scalarmult_base x25519_amd64_51_ucode_clad_base
#define ladderstep           CRYPTO_NAMESPACE(ladderstep)
#define work_cswap           CRYPTO_NAMESPACE(work_cswap)

#endif
