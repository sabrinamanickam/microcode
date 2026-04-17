# ADC on Goldmont — Findings

*Investigation dates: 2026-04-17. Validated against a Red-Unlocked Goldmont CPU via the `vmwrite` xlat (0x0cd8) hook.*

## TL;DR

- **`_ADC_DSZ64 = 0x37e` is real** and performs 64-bit add-with-carry correctly.
- **ADC reads its carry-in from the architectural EFLAGS domain** (domain #2 per `goldmont_flag_architecture`), not from the internal ALU domain that `ADD→TMP` writes (domain #1).
- **Arch CF is frozen inside a patch**: one ADC does not update arch CF in any way visible to another ADC in the same patch (tested within and across triads).
- **GENARITHFLAGS is the bridge**: `ADD→TMP ; GENARITHFLAGS_R(TMP) ; ADC` promotes domain-#1 CF into domain #2 where ADC consumes it. Canonical 128-bit add works in 3 triads.

## Opcode map

| Mnemonic             | Opcode (bits 41:32) | Status         |
|----------------------|---------------------|----------------|
| `_ADC` / `_ADC_DSZ32`| `0x33e`             | Pre-existing   |
| `_ADC_DSZ64`         | `0x37e`             | Speculative, validated |
| `_ADC_DSZ16` (conj.) | `0x3be`             | Untested       |
| `_ADC_DSZ8`  (conj.) | `0x3fe`             | Untested (commented-out line at `opcode.h:469` suggests prior uncertainty) |
| `_SBB` / `_SBB_DSZ32`| `0x33f`             | Pre-existing   |
| `_SBB_DSZ64`         | `0x37f`             | Added by symmetry, untested |

Derivation: the ADD/MUL families encode data-size in bits `[7:6]` of the 10-bit opcode, with delta `0x40` between DSZ32 and DSZ64 (ADD: `0x000→0x040`; MUL: `0x22c→0x26c`). Extending this to ADC gives `0x33e→0x37e`, which empirically works.

## Generated macros

Added via `include/gen_inst.py`:

```python
{'name': 'ADC', 'dsz': ['DSZ32', 'DSZ64'], 'args': ['DRI','DIR','DRM','DMR','DRR']},
{'name': 'SBB', 'dsz': ['DSZ32', 'DSZ64'], 'args': ['DRI','DIR','DRM','DMR','DRR']},
```

Exposes `ADC_DSZ64_DRR(dst,src0,src1)`, `ADC_DSZ64_DRI(dst,src,imm)`, etc. (and DSZ32 equivalents, and SBB counterparts).

## What ADC reads

ADC computes `dst = src0 + src1 + CF_in`. The question was: where does `CF_in` come from?

Measured behavior (`test_adc_dsz64.c`): for every test case under every entry-CF setting, the result exactly matches `src0 + src1 + arch_RFLAGS.CF` captured at hook entry. This holds regardless of:

- whether ADD precedes ADC in the same triad,
- whether ADC's sources are arch registers or TMPs,
- whether there is a triad boundary between ADD and ADC.

Decisive test: running two ADCs back-to-back under arch CF=1, the first ADC's arithmetic result had CF_out=0 (`0+0+1=1`), but the second ADC still added 1 to its sum. → the second ADC read the same frozen arch CF, not the first ADC's output carry.

Conclusion: **ADC reads domain #2 (arch EFLAGS.CF), captured at patch entry and held constant until `END_SEQWORD`.**

## Strategies tested for routing ADD's CF into ADC

Measured by `test_adc_carry_route.c`. Arch CF was forced to 0 at hook entry so any propagation must come from ADD's own carry.

| Strategy | Description                                                      | Result |
|----------|------------------------------------------------------------------|--------|
| A        | Same triad: `ADD(TMP0, RAX, RDX) ; ADC(TMP1, RCX, RBX)`          | fail   |
| B        | Same triad, ADC reads TMP sources (preloaded `a_hi`,`b_hi`)      | fail   |
| C        | Cross-triad: ADD in T0, ADC in T1                                | fail   |
| **D**    | **`ADD(TMP0,...) ; GENARITHFLAGS_R(TMP0) ; …ADC in next triad`** | **works** |
| **E**    | Same as D, but `GENARITHFLAGS_RR(TMP0, RAX)`                     | **works** |

Key negative findings:

- **ADC's read domain is not source-type gated** (B). Making the sources TMPs does not pull ADC into domain #1.
- **Triad boundary alone is not sufficient** (C). Even though domain-#1 CF from ADD→TMP survives across triads for other consumers (per `flags.c` findings), ADC never reads domain #1.

Key positive finding:

- **GENARITHFLAGS is the only known bridge** from domain #1 → domain #2. The `_R` variant (single-operand) is sufficient; the `_RR` operand hint is not required.

## Canonical 128-bit add

```c
ucode_t p[] = {
    { ADD_DSZ64_DRR(TMP0, RAX, RDX),  GENARITHFLAGS_R(TMP0),   NOP, NOP_SEQWORD },
    { ADC_DSZ64_DRR(TMP1, RCX, RBX),  NOP,                     NOP, NOP_SEQWORD },
    { ZEROEXT_DSZ64_DR(RAX, TMP0),    ZEROEXT_DSZ64_DR(RCX, TMP1), NOP, END_SEQWORD },
};
```

`RAX:RDX` holds `a_lo:b_lo`, `RCX:RBX` holds `a_hi:b_hi`. Result returned as `(RAX, RCX) = (sum_lo, sum_hi)`. Three triads.

## Comparison with SETCC-based carry propagation

For reference — `flags.c` Strategy A shows a SETCC-based alternative that does NOT use ADC:

```c
T0: ADD(TMP0, a_lo, b_lo) ; SETCC_CONDB_DR(TMP_c, TMP0) ; NOP         | NOP_SEQWORD
T1: ADD(TMP1, a_hi, b_hi) ; ADD_DSZ64_DRR(TMP1, TMP1, TMP_c) ; NOP    | NOP_SEQWORD
T2: writeback ...                                                     | END_SEQWORD
```

Also 3 triads. Functionally equivalent. Advantages: does not touch arch EFLAGS (no clobber of caller's flag state between patch entry and `END_SEQWORD` — relevant if the patch needs to preserve arch flags for downstream hooked behavior). Disadvantages: one extra ALU op per limb (SETCC + folded ADD vs. single ADC).

For single 128-bit add either is fine; for n-limb chains the GENARITHFLAGS+ADC path is cleaner since each ADC reads and logically updates the "current carry" via one domain.

## Open questions

1. **2-triad compaction**: does `ADD ; GENARITHFLAGS_R ; ADC` all in one triad work? This requires GENARITHFLAGS's arch-CF write to be visible to ADC in the next slot of the same triad. Untested.
2. **N-limb chaining**: does `… ; GENARITHFLAGS_R(TMP1) ; ADC(TMP2, …)` promote the *current* ADC's internal CF for the next limb? If yes, arbitrary-precision add is `3 + 2·(N−1)` triads. Untested.
3. **SBB_DSZ64 (0x37f)**: added to `opcode.h` by symmetry with ADC. Arithmetic semantics and flag domain assumed identical, but not empirically verified.
4. **Short-width ADC (DSZ8/16)**: opcodes `0x3fe` and `0x3be` conjectured but never exercised. The commented-out `/* _ADC (0x3feUL << 32) */` at `opcode.h:469` suggests someone previously suspected 0x3fe was ADC — it is more likely `_ADC_DSZ8` by the bit-[7:6] pattern.

## Artifacts

- `include/opcode.h` — added `_ADC_DSZ32` / `_ADC_DSZ64` / `_SBB_DSZ32` / `_SBB_DSZ64` defines.
- `include/gen_inst.py` — added ADC and SBB to the instruction table.
- `include/inst.h` — regenerated; exposes `ADC_DSZ64_*` / `SBB_DSZ64_*` macros.
- `Sabrina/tests/test_adc_dsz64.c` — validates the 0x37e opcode is ADC and characterizes its read domain (arch vs. slot-0-ADD).
- `Sabrina/tests/test_adc_carry_route.c` — surveys five strategies for routing ADD's CF into ADC; identifies GENARITHFLAGS as the bridge.
- `Sabrina/tests/Makefile` — added `-I ../../include` so tests track live repo headers (not `/usr/local/include` stale copies).
