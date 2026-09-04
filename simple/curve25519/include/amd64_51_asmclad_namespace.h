/* Force-included when building the amd64-51/asm-Clad CONTROL.
 *
 * The framework-matched partner of amd64_64_asmclad_namespace.h. Gives the
 * ladder-rewrite tax for the UNSATURATED representation inside amd64-51's own
 * framework, so the 5x51 decomposition no longer has to borrow a ladder tax
 * measured across two different frameworks.
 *
 *   amd64-51/asm        qhasm ladderstep.S + qhasm mul/square
 *   amd64-51/asm-Clad   C ladderstep.c    + qhasm mul/square   <-- this
 *   amd64-51/ucode-Clad C ladderstep.c    + 5x51 microcode
 *   amd64-51/ucode      inline-asm ladder + 5x51 microcode
 *
 * Layout: everything from amd64-51 (recompiled under this namespace) INCLUDING
 * fe25519_mul.S / fe25519_square.S; only ladderstep is replaced, by the C one
 * in extra/amd64-51-ucode/. Pure native — installs no microcode.
 *
 * Public entry point: x25519_amd64_51_asmclad. */

#ifndef AMD64_51_ASMCLAD_NAMESPACE_H
#define AMD64_51_ASMCLAD_NAMESPACE_H

#define CRYPTO_NAMESPACE(x)  supercop_amd64_51_asmclad_##x
#define _CRYPTO_NAMESPACE(x) supercop_amd64_51_asmclad_unused_##x
#define crypto_scalarmult      x25519_amd64_51_asmclad
#define crypto_scalarmult_base x25519_amd64_51_asmclad_base
#define ladderstep           CRYPTO_NAMESPACE(ladderstep)
#define work_cswap           CRYPTO_NAMESPACE(work_cswap)

#endif
