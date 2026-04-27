# Microcode Cryptographic Field Arithmetic

Implements elliptic curve field arithmetic (squaring/multiplication) via CPU microcode patches on Intel Goldmont (Celeron N3350). Goal: beat GCC -O3 by encoding operations directly in patch RAM.

## Architecture

Patches are arrays of `ucode_t` triads (3 micro-ops + seqword). Installed via `patch_ucode()`, hooked via `hook_match_and_patch()` on vmwrite (0x0cd8). Max 128 triads at U7c00-U7dfc. Staging area at U7de0-U7df0 (reserved).

Build: `make PROG=asm_op_<name>` produces `<name>_static`. Run: `sudo taskset -c 0 ./<name>_static`.

## Hardware Constraints

- 128-triad patch RAM limit (each triad = 4 address units)
- MUL must be in slot 0 of a triad
- Intra-triad RAW: slot 0->1 confirmed (value+flags), slot 1->2 confirmed (flags via SETCC)
- Slot 0->2 value RAW: **CONFIRMED** (test_slot02_raw.c, 2026-04-28)
- Slot 0+2 WAW: **CONFIRMED, slot 2 wins** (test_slot02_raw.c, 2026-04-28)
- SETCC_CONDB_DR only works on TMP registers, NOT arch registers
- TMP registers don't persist across vmwrite calls — keep state in arch regs
- MUL_DSZ64_DRR(hi, srcA, srcB): srcA preserved, srcB gets lo, hi gets hi
- Patch addresses must be even

## Implemented Curves

| Curve | Type | Limbs | Files | Perf vs GCC |
|-------|------|-------|-------|-------------|
| P-256 | Montgomery 4x64 | 4 | asm_op_p256_sq.c, asm_op_p256_mul.c | 119cyc (27% faster) |
| curve25519 | Dettman 5x51 | 5 | asm_op_curve25519.c, asm_op_curve25519_mul.c | faster |
| secp256k1 (Dettman) | Dettman 5x52/48 | 5 | asm_op_secp256k1.c, asm_op_secp256k1_mul.c | faster |
| poly1305 | Solinas 3x44/43 | 3 | asm_op_poly1305.c, asm_op_poly1305_mul.c | ~same |
| secp256k1 (Montgomery) | Montgomery 4x64 | 4 | asm_op_secp256k1_mont_sq.c, asm_op_secp256k1_mont_mul.c | in progress |

Reference C implementations in `../curvesC/`. Not yet implemented: P224, P384, P448, P521, P434.

## Montgomery Multiplication (P-256 / secp256k1_mont)

Word-by-word Montgomery, 4 iterations per operation, 2-iterations-per-vmwrite.

Each MONT_ITER has 4 phases:
- **Phase A** (schoolbook): a_i x b[0..3] -> product w0-w4
- **Phase A'** (accumulate): acc += product, triple-pack SETCC carry chains
- **Phase B** (reduction): m = acc[0] * mu, then m x p -> red[0..4]
- **Phase C** (add+shift): acc += red, discard word 0, shift

### Register allocation
- Acc: R15=acc[0], R9=acc[1], R10=acc[2], R13=acc[3], RAX=acc[4]
- b values: RSI=b[0], R12=b[1], R11=b[2], R14=b[3] -> TMP10-13 in PREP
- Constants: R8, RBX (curve-specific), RBP (secp256k1 mu)
- Scratch: RDI=a_i, RDX/RCX for MUL
- TMP9: P256=p[1] precompute (SHR), secp256k1=mu. MUST NOT be clobbered between iterations
- TMP10-13: free after Phase A (b copied to TMPs in PREP)
- TMP14: Phase A' extra carry, TMP15: a[i+1] for 2-iter switch

### P-256 specifics
- mu=1 (m=acc[0], no mu MUL), p[2]=0 (skip one MUL), 3 MULs in Phase B
- R8=p[0]=0xFFFFFFFFFFFFFFFF, RBX=p[3]=0xFFFFFFFF00000001
- p[1] precomputed as SHR(R8,32)=0xFFFFFFFF stored in TMP9

### secp256k1 Montgomery specifics
- mu=0xD838091DD2253531, p[0]=0xFFFFFFFEFFFFFC2F, p[1..3]=all ones
- R8=p[1..3]=0xFFFFFFFFFFFFFFFF, RBX=p[0], RBP=mu (push/pop in inline asm)
- p[1]=p[2]=p[3] identical -> one m*R8 MUL reused for all three
- 3 MULs in Phase B: mu, p[0], R8
- TMP9=mu (from RBP via PREP). Save red[2] to TMP10 (NOT TMP9!) to preserve mu

### Key optimizations
- Triple-pack: `{SETCC(c_prev), ADD(+cin), SETCC(c_next)}` — 3 ops/1 triad
- Phase A' w0 merged into Phase A MUL triad (slot 1-2 of MUL triad)
- Phase B `{MUL, ADD, SETCC}` merges (MUL in slot 0, carry chain in slots 1-2)
- Boundary merges: Phase A'/B, Phase B/C

## ADC / Flag Domains

- Domain #1: per-TMP-register flags, set by ADD, read by SETCC. Persist until next ADD to same TMP.
- Domain #2: arch RFLAGS. ADC reads domain #2 (frozen at patch entry).
- GENARITHFLAGS_R bridges #1->#2 but fails unreliably in large patches.
- Use SETCC (domain #1) for all carry chains. ADC not used in production code.
- See `../tests/adc_findings.md` for full investigation.

## Lessons Learned

1. SETCC on arch registers fails silently — always ADD->TMP, SETCC->TMP, then ZEROEXT->arch
2. Never clobber a TMP needed across MONT_ITER invocations within one vmwrite (e.g. TMP9=mu)
3. PREP reloads TMPs at each vmwrite, but NOT between the two MONT_ITERs within a vmwrite
4. `movabs` is AT&T syntax; use `mov` in Intel syntax inline asm for 64-bit immediates
5. The `*4` factor in address calculations: `patch_ucode` addresses are 4 units per triad
6. OR_DSZ64_DRM uses memory operand — use register-only ops in patches
