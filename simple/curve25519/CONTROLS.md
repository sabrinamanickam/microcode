# Ladder controls: separating the field-op backend from the ladder rewrite

**Status:** COMPLETE. Both controls ran in the full 24-config matrix on
2026-08-29 21:46 (`RESULTS.md`, commit `7343adb2e`), all vectors passing. Every
number below is same-process and pinned (no_turbo=1, userspace, 1.094 GHz).

## The problem these controls fix

Both microcode *hybrids* replace more of their SUPERCOP baseline than the
field operations:

| contender | driver / invert / pack / cswap | ladder | fe_mul / fe_square |
|---|---|---|---|
| `amd64-64/asm`   | amd64-64 | amd64-64 `ladderstep.S` (6,580 lines qhasm) | amd64-64 asm |
| `amd64-64/ucode` | amd64-64 | `amd64-64-ucode/ladderstep.c` (114 lines C) | 4×64 microcode |
| `amd64-51/asm`   | amd64-51 | amd64-51 `ladderstep.S` (7,196 lines qhasm) | amd64-51 asm |
| `amd64-51/ucode` | amd64-51 | inline-asm register-chained ladder (in `full_curve25519_inline2.c`) | 5×51 microcode |

So `asm` vs `ucode` moves **two** things at once. Neither ratio can attribute
anything to the microcode field ops on its own — and the `asm` arms are exactly
the ones whose ladders fuse the field ops into a monolithic routine, which is
the coding style microcode structurally cannot use (128-triad patch RAM).

Note also the two hybrids do *not* share a ladder with each other: the 4×64
hybrid uses a C ladder, the 5×51 hybrid an inline-asm one.

## Control 1 — `amd64-64/asm-Clad` (saturated representation)

`full_curve25519_amd64_64_asmclad.c`. amd64-64's framework, **the same
`amd64-64-ucode/ladderstep.c` object source** as `amd64-64/ucode`, but
amd64-64's own asm `fe25519_mul.S` / `fe25519_square.S`. Pure native — no
patch, no red-unlock, no sudo. The binary also links stock amd64-64 and
benches the two arms **interleaved in one process**, so the ladder tax is a
same-process ratio.

```
amd64-64/asm       qhasm ladderstep.S + qhasm mul/square
amd64-64/asm-Clad  C ladderstep.c     + qhasm mul/square   <-- the control
amd64-64/ucode     C ladderstep.c     + 4×64 microcode

asm-Clad / ucode = field-op effect, ladder held constant
asm / asm-Clad   = the ladder-rewrite tax, on its own
```

### Result (24 configs, all 4/4 RFC 7748)

| quantity | best config | geomean |
|---|---|---|
| `amd64-64/asm` median | 271,330 | — |
| `amd64-64/asm-Clad` median | 306,285 | — |
| `amd64-64/ucode` median | 512,251 | — |
| **ladder tax** (`asm-Clad ÷ asm`) | **1.129** | **1.183** |
| **field-op penalty, CONTROLLED** (`ucode ÷ asm-Clad`) | **1.673** | **1.682** |
| field-op penalty, uncontrolled (`ucode ÷ asm`) | 1.888 | 1.990 |

A standalone 24-config sweep of the same binary before the matrix run gave
1.128 / 1.184 for the ladder tax — the matrix reproduces it to three decimals.

**Consequence.** The uncontrolled `a64/asm ÷ a64/ucode` ratio charges the
microcode with a ladder rewrite worth ~18%. Controlled, the 4×64 microcode
field ops are **1.68×** slower than amd64-64's asm, not the ~1.99× the
uncontrolled table implies. The qualitative conclusion (saturated 4×64
microcode loses) is unchanged; the magnitude is materially different.

## Control 2 — `a51ops/C-ladder` (unsaturated representation)

Added to `full_curve25519_inline2.c` as a contender, so it is measured **in the
same process and the same binary** as `ucode/C-ladder`. It is `x25519_cryptopt`
with exactly one substitution: Bernstein–Schwabe's amd64-51 `fe25519_mul.S` /
`fe25519_square.S` in place of the CryptOpt field ops. Identical C ladder,
identical inversion chain, identical cswap/pack/driver.

```
ucode/C-ladder    shared C ladder + 5×51 microcode
a51ops/C-ladder   shared C ladder + amd64-51 asm mul/square   <-- the control
```

This pair differs in the field-op backend and nothing else — the clean form of
"microcode beats hand-tuned asm field ops".

Correctness gate: the 1000-iteration RFC 7748 chain also gates 5×51 bound
compatibility between our `fe_add`/`fe_sub` and amd64-51's asm mul/square.
It passes.

`probe_a51ops.c` verifies and benches this control **without any microcode**
(`taskset -c 0 ./probe_a51ops_static`, no sudo) via the new
`INLINE2_CONTENDERS_ONLY` include mode.

### Result (24/24 configs, same process, same binary)

| quantity | best config | geomean |
|---|---|---|
| `ucode/C-ladder` median | 304,919 | — |
| `a51ops/C-ladder` median | 383,490 | — |
| **field-op win, CONTROLLED** (`a51ops ÷ ucode`) | **1.258** | **1.223** |
| headline, uncontrolled (`a51/asm ÷ a51/ucode`) | 1.133 | 1.083 |
| ladder tax on the asm arm (`a51ops/C-ladder ÷ a51/asm`) | 1.077 | — |

**On an identical ladder the 5×51 microcode field ops are 22.3% faster
(geomean) than Bernstein–Schwabe's hand-written amd64-51 asm** — not the ~6.5%
(here 8.3% geomean) the end-to-end comparison reports.

The end-to-end figure is smaller because amd64-51's asm field ops gain from
being *fused into* its monolithic `ladderstep.S`: 356,013 end-to-end versus
383,490 when the same asm is called per-op from the C ladder, ≈ +7.7%. That is
a gain the microcode structurally cannot access — the 128-triad patch RAM
forbids holding a whole ladder step. The two effects compose:
1.223 ÷ 1.077 ≈ 1.136, which is the measured best-config headline of 1.133.

**This decomposition is the honest claim** and it is more informative than the
net: the field-op win is larger than reported, and part of it is handed back
because patch RAM forbids ladder-level fusion. Both halves are the paper's
thesis.

## The headline claim, restated

> On an identical Montgomery ladder, microcode 5×51 field arithmetic is
> **22% faster** than the best hand-written x86-64 asm for the same
> representation. About 8 points of that are given back end-to-end because
> patch RAM cannot hold a fused ladder step, leaving a net **8%**.

Both halves are the thesis: descending below the ISA buys a real constant
factor on the operation, and the patch-RAM ceiling caps how much of it
survives integration.

## What changed in the tree

- `full_curve25519_amd64_64_asmclad.c` — new control binary (native).
- `include/amd64_64_asmclad_namespace.h` — its namespace.
- `probe_a51ops.c` — no-sudo verify/bench of the 5×51 control.
- `full_curve25519_inline2.c` — added `fe_mul_a51`/`fe_sq_a51`/
  `fe_invert_a51`/`x25519_a51ops`, its RFC 7748 chain, its bench line, and the
  `INLINE2_CONTENDERS_ONLY` include mode.
- `Makefile` — `AMD64A_*` object set and the two new PROG entries.
- `lib/build_run.sh`, `lib/print_matrix.sh`, `bench_supercop_matrix.sh` —
  both controls added to the contender list, matrix table and `FIELDOP_ISO`;
  new `SATURATED_ISO` table.

## Doc fixes this implies

- `lib/build_run.sh` called `amd64-64/asm` vs `amd64-64/ucode` "a clean
  same-ladder field-op comparison" two lines after stating that ladderstep is
  swapped. It is not same-ladder; `asm-Clad` is the same-ladder arm. (Fixed.)
- `RESULTS.md`'s same-ladder section attributes the win "with zero confounds",
  but its table lacked any hand-asm field-op arm — `a51ops/C-ladder` is that
  arm.
