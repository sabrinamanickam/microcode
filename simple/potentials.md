# Potential future optimizations

Catalog of optimizations we identified during the 2026-05 session but
did not pursue. Each entry has an estimated payoff, a design sketch,
and the main risks/blockers — so a future attempt can decide whether
the effort is worth the gain.

Numbers below are TSC ticks at the **pinned 1.1 GHz base clock** on
the Goldmont N3350 (matches `ours/ucode = 312,260 cyc` as of
2026-05-15). At burst (2.4 GHz) numbers are roughly half.

---

## B1. Combined `fe_add + fe_sq` microcode patch  *(the "Option B" idea)*

**Estimated payoff:** ~5–12 k cyc / X25519 (1.5%–4%)

The ladderstep has two patterns where a `fe_add`/`fe_sub` immediately
feeds a `fe_sq_ucode`:

```c
fe_add(A, x2, z2);
fe_sq_ucode(A, AA);          /* AA = (x2 + z2)^2 */

fe_sub(B, x2, z2);
fe_sq_ucode(B, BB);          /* BB = (x2 - z2)^2 */
```

Currently the add/sub writes A or B to memory (5 stores), and the
fe_sq wrapper reads them back (5 loads) + does the LEA/IMUL precompute
+ fires the patch. The intermediate value (`A` or `B`) only exists to
bridge two operations — pure round-trip overhead.

**Proposed:** a single microcode patch that takes `x2` and `z2` as
inputs and produces `AA = (x2+z2)^2` (and similarly for the sub
variant). The patch internally computes A = x2+z2 (5 ADDs in
microcode) before running the existing square. The C wrapper passes
two operands instead of one.

### Sizing

- New patch body: existing fe_sq (42 triads) + 5 ADD triads for the
  add prelude ≈ **45–48 triads**.
- Sub variant: ~same, with biased subtract using ADD-with-immediate to
  inject the 2p bias.
- Total patch RAM after this change:
  `66 (fe_mul) + 47 (fe_add_sq) + 47 (fe_sub_sq)` = **160 triads**.
  **Over the 128-triad budget by 32 triads.**

### Key blocker: doesn't fit alongside both fe_mul and fe_sq

Options to make it fit:
1. **Drop the single-shot fe_sq patch.** Use fe_add_sq with one
   operand zero when we need a plain square. Saves 42 triads. But:
   the C wrapper for fe_add_sq has to load 10 limbs even when one is
   "logically" unused — wastes 5 cyc/call × ~1300 calls.
2. **Install one patch at a time, swap in/out** for the ladder vs.
   invert phases. The patch_ucode + hook_match_and_patch cost is
   ~hundreds to low thousands of cyc per swap — could overwhelm
   savings unless we swap exactly twice per X25519.
3. **Share triads between fe_mul and fe_add_sq** if the body
   overlaps. Probably impossible — fe_mul is structurally different
   from fe_sq.

### Per-call savings model

Per fe_add+fe_sq pair saved:
- Eliminate fe_add C function call: ~5–8 cyc (5 loads + 5 adds + 5
  stores, partly pipelined)
- Eliminate fe_sq wrapper memory I/O for `A`/`B`: ~12 cyc (we measured
  this directly in `fe_sq_chain_microbench`)
- **Total per pair: ~17–20 cyc**

Pairs per X25519: 2 per ladder iter × 255 iters = **510 pairs**.
Theoretical: `510 × 20 ≈ 10 k cyc`.

Realistic after patch-swap overhead and the inevitable per-call
register-shuffling additions: **5–8 k cyc**.

### Effort: half to full day

- Design the combined patch (figure out register conventions, ADD
  prelude, slot scheduling)
- Validate correctness against existing fe_sq path
- Either redesign patch installation strategy or accept the single-
  shot fe_sq loss
- Update `fe_invert_ucode` to keep using single fe_sq (or rewrite
  invert in terms of `fe_add_sq` with operand=0 if we drop single
  fe_sq)

---

## B2. Combined `fe_mul × 2` microcode patch

**Estimated payoff:** ~5–10 k cyc / X25519

In the ladderstep:

```c
fe_mul_ucode(D, A, DA);
fe_mul_ucode(C, B, CB);
```

These are independent — no data dependency. A patch that computes two
products in one fire saves one vmwrite round-trip (~15 cyc wrapper
saved per pair).

### Sizing

- Two interleaved fe_mul bodies: ~120 triads at best (current fe_mul
  is 66 triads; some compute can interleave to fit slot-by-slot).
- Combined with single fe_sq (42 triads): **162 triads**. Over budget
  by 34.

Same patch-budget blocker as B1.

### Per-call savings

Per pair: ~15–20 cyc (one wrapper saved). 4 pairs per X25519 (2 in
ladderstep + 2 more in `mul(x1, z3, z3)` followed by `mul(AA, BB,
x2)` and the closing `mul(E, t0, z2)`).

Wait — the pairs in the ladder are only:
- `fe_mul(D, A, DA)` + `fe_mul(C, B, CB)` (2 muls, independent)
- The other muls are interleaved with non-mul work, so not pairable.

So really just **2 pairs per iter × 255 = ~510 saved wrapper sets**.
At ~15–18 cyc each: **8–9 k cyc.**

### Effort: full day

Patch design is harder than B1 — fe_mul has tighter register pressure
than fe_sq.

---

## B3. Fused `fe_mul121665 + fe_add` in microcode

**Estimated payoff:** ~3–5 k cyc / X25519

In the ladderstep tail:

```c
fe_mul121665(t0, E);
fe_add(t0, AA, t0);
fe_mul_ucode(E, t0, z2);
```

The `121665 * E` computation in C uses `__uint128_t` and is ~30 cyc.
Then add ~5 cyc, then the fe_mul. The first two ops could collapse
into a small microcode patch that takes `AA` and `E`, computes
`AA + 121665*E`, and exits.

### Sizing

- 5 MUL-by-17-bit operations (121665 is 17 bits)
- 5 ADD operations (combining with AA)
- Carry propagation
- ≈ **15–20 triads** (fits comfortably in the 19 free triads)

### Per-call savings

- Eliminate the C `fe_mul121665` body: ~30 cyc
- Eliminate the C `fe_add`: ~5 cyc
- Add the new patch's wrapper overhead: ~15 cyc
- Net: ~20 cyc per iter × 255 iters = **~5 k cyc**

### Effort: half day

Smallest of the three options. Mostly a question of finding a
sufficiently dense way to compute 121665 × 5 limbs in microcode (each
MUL needs to deliver hi+lo across limb boundaries; the small 17-bit
constant might enable shift-and-add tricks instead of full MUL).

**This is probably the highest ROI per effort-hour of the three.**

---

## B4. Microcoded `fe_sq` internal loop (UJMP/UJMPCC variant)

**Estimated payoff:** marginal over the existing C-level chaining
(2026-05-15 chaining already saves ~3-7 k cyc; internal looping
would save the LEAs+IMULs per iter — maybe **+1–2 k cyc** more).

We have `[[microcode-control-flow]]` proving UJMP_I/UJMPCC works.
A looped fe_sq patch could replace the C-side LEA/IMUL precompute
with microcode-internal precompute, plus collapse N squarings into
one vmread.

### Why not pursue

The C-level chaining (`fe_sq_ucode_n`) already captures most of the
win. The remaining LEAs/IMULs in the wrapper are <10 cyc/iter total
and partly pipeline with the patch fire. Marginal additional gain
doesn't justify the patch-design complexity or the patch RAM cost.

Skip unless we ever want a "pure microcode" version for some other
reason (academic claim, side-channel argument, etc.).

---

## B5. SIMD parallelism inside microcode (speculative)

**Estimated payoff:** unknown — potentially 2× speedup if it works

We saw `SIMDLSTADSTGBUF` and `SIMDHSTADSTGBUF` in the opcode header
but didn't investigate. Goldmont supports SSE4.2 natively, so its
microcode probably has SIMD-flavored micro-ops available.

If we can run two fe25519 operations in parallel using SIMD micro-
ops (e.g., process limbs 0,1 then 2,3 in pairs), one X25519 could
near-halve.

### Why not pursued (yet)

We don't know:
- Whether SIMD ld/st/arithmetic micro-ops are documented or
  reachable through the existing macro set
- Whether the patch infrastructure (vmwrite/vmread hooks) interacts
  cleanly with SIMD registers
- Whether the MAC sequence in our fe_mul could be expressed in
  SIMD form

**Effort: multi-day exploration**, no guarantee of payoff. Would be
a paper in itself.

---

## B6. Re-examine fe_mul_ucode wrapper for micro-trims

**Estimated payoff:** small, < 2k cyc

The fe_mul wrapper currently has:
- 10 mem loads (5 a + 5 b)
- 2 XOR clears
- vmwrite
- 5 mem stores

That's 17 instructions of regular x86. Hard to compress without
removing functionality. But it's worth re-reading the inline asm
once with fresh eyes — sometimes a redundant `mov` slips in.

### Worth attempting if you're already in the file

Five minutes of effort, potentially zero gain — read-only optimization.

---

## Stgbuf-based designs — DEAD

See `[[stgbuf-capability]]`. The 30-cyc LDSTGBUF latency makes any
design that reads state from stgbuf in the hot path lose to memory
I/O. Don't revisit unless the algorithm has few loads per output
operation (X25519's ~10 loads per fe_mul rules it out).

---

## Stack-ranked priority for future work

| # | optimization | payoff | effort | $/hr |
|---|---|---:|---:|---:|
| B3 | fe_mul121665+add patch | ~5k | 0.5 day | best |
| B1 | fe_add+fe_sq patch | ~5–8k | 1 day | medium |
| B2 | fe_mul × 2 patch | ~8–9k | 1+ day | medium |
| B4 | microcoded fe_sq loop | ~1–2k | 0.5 day | marginal |
| B6 | wrapper micro-trim | <2k | 5 min | high if anything found |
| B5 | SIMD microcode | unknown, possibly big | multi-day | speculative |

If picking one, **B3** has the best ratio of payoff to effort.

If aiming for biggest single jump, **B2** has the highest absolute
ceiling but the biggest patch-RAM problem.

If looking for a research story, **B5** is the open frontier.
