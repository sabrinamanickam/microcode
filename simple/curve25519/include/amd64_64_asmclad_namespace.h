/* Force-included via -include when building the amd64-64/asm-Clad CONTROL.
 *
 * Purpose: isolate the field-op backend from the ladder rewrite.
 *
 * The amd64-64/ucode hybrid replaces THREE of amd64-64's files: ladderstep,
 * fe25519_mul and fe25519_square. So amd64-64/asm vs amd64-64/ucode measures
 * (microcode field ops) AND (C ladder vs qhasm ladderstep.S) at once.
 *
 * This control keeps the C ladder but restores amd64-64's OWN asm field ops,
 * giving the missing middle point:
 *
 *   amd64-64/asm      qhasm ladderstep.S + qhasm fe25519_mul.S/square.S
 *   amd64-64/asm-Clad C ladderstep.c    + qhasm fe25519_mul.S/square.S  <-- this
 *   amd64-64/ucode    C ladderstep.c    + 4x64 microcode field ops
 *
 *   asm-Clad / ucode  = pure field-op effect, ladder held constant
 *   asm / asm-Clad    = the ladder-rewrite tax, measured once
 *
 * Layout:
 *   - mont25519.c, fe25519_invert.c, pack/unpack/setint.c, consts.c,
 *     fe25519_freeze.S, work_cswap.S, fe25519_mul.S, fe25519_square.S
 *     all come from amd64-64 (recompiled with this namespace).
 *   - ladderstep.c comes from simple/curve25519/amd64-64-ucode/ — the SAME
 *     file the ucode hybrid uses, so the ladder is bit-identical between the
 *     two arms.
 *
 * No microcode is installed by this binary; it is pure native code.
 *
 * The public entry point is x25519_amd64_64_asmclad. */

#ifndef AMD64_64_ASMCLAD_NAMESPACE_H
#define AMD64_64_ASMCLAD_NAMESPACE_H

#define CRYPTO_NAMESPACE(x)  supercop_amd64_64_asmclad_##x
#define _CRYPTO_NAMESPACE(x) supercop_amd64_64_asmclad_unused_##x
#define crypto_scalarmult      x25519_amd64_64_asmclad
#define crypto_scalarmult_base x25519_amd64_64_asmclad_base
/* mont25519.c locally `#define`s these; force-define them globally so the
 * ladderstep.c we compile separately defines the properly-prefixed symbol
 * that mont25519.c looks for at link time. */
#define ladderstep           CRYPTO_NAMESPACE(ladderstep)
#define work_cswap           CRYPTO_NAMESPACE(work_cswap)

#endif
