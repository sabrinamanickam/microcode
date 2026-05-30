# X25519 Microcode Benchmark Results

**Generated:** Sat 30 May 2026 18:46:28 ACST
**Host:** redunlock-GB-BPCE-3350C
**CPU:** Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
**Pinned freq:** 2387733 kHz   (governor: `ondemand`, no_turbo: `0`)
**Configs that ran:** 24 / 24
**Pipeline:** `taskset -c 0 ./full_curve25519_static` for each (compiler, -O) combo

## Contender legend

| label | backend |
|---|---|
| ours/hand-C    | our ladder + hand-written C with `__uint128_t` |
| ours/fiat      | our ladder + fiat-crypto autogen C |
| ours/cryptopt  | our ladder + CryptOpt Goldmont-tuned asm field ops |
| ours/ucode     | our ladder + microcode field ops |
| ours/ucode-inline | all-in-one inline-asm 5×51 ladder + microcode field ops |
| donna_c64      | donna whole-stack portable C |
| amd64-51/asm   | Bernstein–Schwabe whole-stack x86-64 asm (5x51 unsaturated) |
| amd64-51/ucode | amd64-51's ladder + microcode field ops (hybrid) |
| amd64-64/asm   | Bernstein–Schwabe whole-stack x86-64 asm (4x64 saturated; lib25519's Goldmont pick) |

## How to read

- **Same-ladder OUR / amd64-51** tables are apples-to-apples: only the field-op backend changes; ladder, invert, cswap, framework all held constant.
- **End-to-end** mixes implementations top to bottom; differences include integration choices (struct layout, cross-TU inlining, invert chaining) on top of the field-op delta.
- The **geomean** row in each ratio table collapses 24 configs into one number — that's the single ratio to quote in a paper.

---

## MIN cycles

### Same-ladder OUR — only field-op differs

_metric: min cycles. **bold** = winning config in that column._

| Config | hand-C | fiat | cryptopt | ucode |
|---|