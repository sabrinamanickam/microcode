# Theories to Beat CryptOpt Across All Curves

## Status Summary

| Regime | Curves | Current Status | Target |
|--------|--------|---------------|--------|
| Single vmwrite, Solinas | curve25519, secp256k1_dett, P-521, poly1305, **P-224** | **Winning** | Widen lead |
| Multi vmwrite, Karatsuba | P-448 | **Break-even** | Beat CryptOpt |
| Montgomery iterative | P-256, P-384, secp256k1_mont | **Losing** | Beat CryptOpt |

### P-224 sq (2026-04-30): 113 cyc microcode vs 164 cyc fiat-crypto = 1.45×
Recipe: convert to 4×56-bit unsaturated Solinas, schoolbook squaring (10 MULs)
in single vmwrite (34 triads, ~37 cyc), 56→64 + Solinas fold in C wrapper.
Naive hand-written Solinas in C is 70 cyc (faster, but irrelevant — fiat is the
real GCC baseline). Inline asm tightened: PREP reads RDI/RSI/R12/R11 directly
so the 4 dup movs to R15/R13/R9/R10 are gone; output pointer rides in R8
(preserved by the patch) instead of stack-stashing R15.

## Theory 1: Redundant/Lazy Carry — Skip SETCC Entirely
**Status: DISPROVEN — does not work for ≥2 products per limb**

For unsaturated limbs (W bits) with N products per limb:
N × 2^W < 2^64 means SETCC is unnecessary (64-bit accumulator never overflows).

- W=56 (P-448): N < 256 products — always safe
- W=58 (P-521): N < 64 — always safe
- W=51 (curve25519): N < 8192 — always safe

Current pattern (2 triads per MAC):
```
{ ADD(TMP13+=lo), SETCC(carry), prep }
{ ADD(R8+=hi), MUL, ADD(R10+=carry) }
```

New pattern (1 triad per MAC):
```
{ MUL, ADD(TMP13+=lo_srcB_RAW), ADD(R8+=hi) }
```

Wait — the issue is that TMP13 += lo CAN overflow 64 bits even with small limbs,
because MUL lo output is a full 64-bit value. With 5 products: 5 × 2^64 overflows.

BUT: for unsaturated limbs, products are < 2^(2W). So lo < 2^64 but typically
< 2^(2W) ≈ 2^112 (P-521) or 2^102 (curve25519). The lo part IS up to 2^64-1.

Key insight: ACCUMULATE THE FULL PRODUCT INTO A WIDER ACCUMULATOR without
per-product carry. Use TWO registers: TMP13 (lo 64-bit) and R8 (hi accumulated).

For each MAC:
- MUL(RCX, srcA, RDX) → RDX = lo, RCX = hi
- ADD(TMP13, TMP13, RDX) → lo accumulator. MAY overflow if many products!
- ADD(R8, R8, RCX) → hi accumulator

The overflow from TMP13 is LOST without SETCC. This is ONLY safe if:
sum of N lo-products < 2^64

For N products with inputs < 2^W:
- Each product < 2^(2W)
- Each lo part < 2^64
- Sum of N lo parts < N × 2^64 — OVERFLOWS for N ≥ 2!

So we CANNOT skip SETCC for the lo accumulation in general.

HOWEVER: if we split the 128-bit product differently:
- Accumulate the FULL product (not just lo) using 128-bit precision
- The hi from MUL goes to R8, carry from ADD goes to R8 too

The only carry source is the ADD(TMP13 += lo). If we capture this carry and
add it to R8, that's the standard approach (SETCC + ADD R10).

REVISED THEORY: Skip SETCC only for limbs with FEW products where overflow
is impossible. For limbs with W-bit inputs and N products:
- lo of each product is at most 2^64 - 1
- Sum of N lo's is at most N × (2^64 - 1)
- Overflows if N ≥ 2

So SETCC can only be skipped for 1-product limbs. Not useful.

ACTUALLY: the products are a[i]*a[j] where a[i] < 2^W. The product value is
< 2^(2W). For W=58 (P-521): product < 2^116. The lo part is product mod 2^64,
which can be any value in [0, 2^64). So even for 1 product, lo can be up to
2^64 - 1, and adding carry-in (up to ~2^8) won't overflow.

For 2 products: lo1 + lo2 can be up to 2×(2^64-1) ≈ 2^65. OVERFLOWS 64 bits.

CONCLUSION: SETCC cannot be universally skipped. The per-MAC carry tracking
is necessary whenever ≥ 2 products accumulate into the same 64-bit register.

ALTERNATIVE: Use a DIFFERENT accumulation strategy:
- Don't add products to a running 64-bit accumulator
- Instead, save each product's lo and hi separately
- Do the accumulation in native code with ADC after the vmwrite
- This is Theory 3 (MUL-only patches)

## Theory 2: Convert Montgomery Curves to Unsaturated Solinas
**Status: PROVEN on P-224 (1.45× over fiat). Apply next to P-256, P-384.**

Montgomery curves lose because of iterative SETCC carry chains.
Converting to unsaturated/Solinas representation eliminates the iterative structure.

P-256: p = 2^256 - 2^224 + 2^192 + 2^96 - 1
- 5 limbs of 52 bits (Solinas with multiple reduction terms)
- Schoolbook squaring: 15 MULs, single vmwrite
- Reduction: multiple Solinas terms (more complex than curve25519's ×19)
- Already done by fiat-crypto: `fiat_p256_solinas` exists?

P-224: p = 2^224 - 2^96 + 1
- 4 limbs of 56 bits (Solinas)
- Schoolbook: 10 MULs for squaring
- Reduction: simpler than P-256

P-384: p = 2^384 - 2^128 - 2^96 + 2^32 - 1
- 7 limbs of 56 bits
- Schoolbook: 28 MULs for squaring (fits in single vmwrite!)

For each: the Solinas reduction has multiple ±2^k terms. The patch would
compute schoolbook products + reduction fold, all in one vmwrite.

Estimated impact:
- P-256 sq: 15 MULs, ~55 triads, 1 vmwrite → ~53 cycles vs 150 fiat = 2.83×
- P-224 sq: 10 MULs, ~35 triads, 1 vmwrite → ~37 cycles vs 163 fiat = 4.41×
- P-384 sq: 28 MULs, ~85 triads, 1 vmwrite → ~76 cycles vs ??? fiat

These estimates are VERY optimistic — the Solinas reduction adds triads.
But even with 50% more triads, the single-vmwrite advantage is huge.

## Theory 3: MUL-only Patches + Native ADC Assembly (Hybrid)
**Status: BACKUP if Theory 1 doesn't work**

Microcode does ONLY the MULs (1 triad each). Native asm does ADC accumulation.

Per vmwrite: ~10 MULs → ~12 triads → 10 lo/hi pairs in arch regs
Native ADC: accumulates with native carry chain (1 cycle/ADC)

Challenge: vmwrite overhead (10 cycles per call) limits benefit.
Best for curves where many MULs can be batched in one vmwrite.

## Theory 4: Pipeline Across Consecutive Operations
**Status: FUTURE**

In scalar multiplication (thousands of sq/mul): overlap MUL phase of
operation N with carry phase of operation N-1.

The carry chain runs in native asm BEFORE triggering the next vmwrite.
The vmwrite computes the next set of products.

Doesn't reduce latency per operation but improves throughput in chains.

## Theory 5: Multi-Core with Batched Operations
**Status: FUTURE**

For protocol-level parallelism (multiple independent scalar muls):
Core 0 does MULs via microcode, Core 1 does carry chains.
Communicate via shared L2 cache (~50 cycle latency).

Improves throughput, not single-operation latency.

## Theory 6: Hook Additional Instructions
**Status: RESEARCH NEEDED**

Known hooks: vmwrite (0x0cd8), vmread (0x0618).
Each additional hookable instruction = 1 more "free" patch call.
Need to reverse-engineer more hookable microcode addresses.

## Theory 7: Custom Number Representation for Microcode
**Status: RESEARCH NEEDED**

Design a representation where carries are NEVER needed during accumulation.
Possible approaches:
- Signed digits (each limb in [-2^W, 2^W])
- Carry-save representation (store carry bits separately)
- Redundant representation with wider limbs

## Priority Order
1. ~~Theory 1: No-SETCC~~ — DISPROVEN (lo accumulation overflows for ≥2 products)
2. **Theory 2: Solinas P-256/P-224/P-384** (biggest gap to close, most promising)
3. Theory 3: Hybrid MUL+ADC (fallback)
4. Theory 4-7: Future work
