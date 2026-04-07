# Curve25519 Field Square via MAC128 Microcode Hook

## Overview

Field element squaring in GF(2^255 - 19) accelerated by a custom microcode hook on Goldmont. The hook replaces `vmwrite rcx, rdx` with a 128-bit multiply-accumulate: `{R8, RAX} += RCX * RDX`.

This is one primitive in Curve25519 -- a full X25519 scalar multiplication requires ~1020 squarings, ~1275 multiplications, and ~2000 additions across a 255-step Montgomery ladder.

## Representation

Field elements use **radix 2^51**, 5 limbs of 51 bits each:

```
value = limb[0] + limb[1]*2^51 + limb[2]*2^102 + limb[3]*2^153 + limb[4]*2^204
```

Reduction uses `2^255 = 19 (mod p)`, so products where limb indices sum to >= 5 are multiplied by 19.

## The MAC128 Hook

Installed at MSRAM `0x7c00`, triggered by `vmwrite` (opcode match at `0x0cd8`).

**Interface:**
- **Input:** RCX = multiplicand, RDX = multiplier, RAX = acc_lo, R8 = acc_hi
- **Output:** RAX = acc_lo, R8 = acc_hi (128-bit accumulator updated)
- **Clobbers:** RCX, RDX

**Microcode (6 triads, from `curve_twohooks.c`):**

```
T0: ZEROEXT TMP3, RAX       | MUL RCX*RDX -> RCX:RDX   | NOP
T1: ADD TMP0, TMP3, RCX     | AND TMP1, TMP3, RCX       | OR TMP2, TMP3, RCX
T2: NOTAND TMP4, RAX, TMP2  | ADD R8, R8, RDX            | NOP
T3: OR TMP5, TMP1, TMP4     | NOP                        | NOP
T4: SHR TMP5, TMP5, 63      | NOP                        | NOP
T5: ADD R8, R8, TMP5        | NOP                        | END
```

Uses a bit-manipulation carry chain (AND/OR/NOTAND/SHR) because `SETCC` only works when ADD writes to a TMP register, not architectural registers, and the 4-triad SETCC version (`mac128_nomovs.c`) has this constraint.

## Squaring: 15 vmwrite Calls

Pre-computation (all uint64):
```
d0 = 2*a0    d1 = 2*a1    d2 = 2*a2    d3 = 2*a3
r3 = 19*a3   r4 = 19*a4
```

Each limb accumulates 3 multiply-add products via vmwrite:

| Limb | Products | vmwrite operands |
|------|----------|------------------|
| c[0] | a0*a0 + d1*r4 + d2*r3 | (a0,a0), (d1,r4), (d2,r3) |
| c[1] | d0*a1 + r3*a3 + d2*r4 | (d0,a1), (r3,a3), (d2,r4) |
| c[2] | d0*a2 + a1*a1 + d3*r4 | (d0,a2), (a1,a1), (d3,r4) |
| c[3] | d0*a3 + d1*a2 + r4*a4 | (d0,a3), (d1,a2), (r4,a4) |
| c[4] | d0*a4 + d1*a3 + a2*a2 | (d0,a4), (d1,a3), (a2,a2) |

Between limbs, carry is extracted in x86:
```c
carry = (c_hi << 13) | (c_lo >> 51);   // bits [51:127]
out[i] = c_lo & 0x7FFFFFFFFFFFF;       // bits [0:50]
```

The carry seeds the next limb's accumulator (`mov rax, carry; xor r8, r8`).

Final reduction wraps the last carry back to limb 0:
```c
out[0] += carry * 19;
carry = out[0] >> 51; out[0] &= MASK51;
out[1] += carry;
```

This matches the Fiat Cryptography `fiat_curve25519_carry_square` exactly (same products, same reduction -- Fiat just factors `x2 = 38*a4` instead of `d1 = 2*a1, r4 = 19*a4`).

## Files

| File | Purpose |
|------|---------|
| `curve_twohooks.c` | Hook installer + correctness tests (8 vectors + 1000-iter stress) |
| `bench_twohooks.c` | Cycle timing benchmark (assumes hook already installed) |
| `mac128_nomovs.c` | Alternate 4-triad hook using SETCC (simpler but different constraint) |

## Build and Test

```bash
# Build
make PROG=curve_twohooks
make PROG=bench_twohooks

# Step 1: Install hook and verify correctness
sudo taskset -c 0 ./curve_twohooks_static

# Step 2: Benchmark (must run on same core as hook)
sudo taskset -c 0 ./bench_twohooks_static
```

**Critical:** Both binaries must run on core 0 (`taskset -c 0`). The hook is installed per-core. Running on the wrong core gives silent wrong results -- vmwrite executes but doesn't modify RAX:R8.

## Results

```
Native C:   min/op  142  avg/op  151 cycles
MAC128:     min/op  100  avg/op  102 cycles    (pending correctness fix)
```

## Optimization Path

| Approach | vmwrite calls | Estimated cycles |
|----------|---------------|------------------|
| Current (1-MAC/call) | 15 | ~120 |
| 3-MAC/call batched | 5 | ~70 |
| Monolithic (all in ucode) | 1 | ~60 |
| 2-hook split (phase 1 + 2) | 2 | ~67 |

The monolithic approach packs all 15 MACs + carry chain into ~90 triads of microcode, eliminating x86 <-> ucode transition overhead. Main challenge: no memory stores (`STAD` broken in vmwrite context), so all intermediate values must live in TMP registers.
