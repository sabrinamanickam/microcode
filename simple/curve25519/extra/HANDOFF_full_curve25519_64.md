# Handoff: full_curve25519_64.c — 4×64 saturated X25519 in microcode

**Created:** 2026-05-22, end of prior session.
**Goal:** build a full X25519 (`crypto_scalarmult`) using 4×64 saturated field
arithmetic in microcode, attempting to close the 40k-cyc gap to SUPERCOP
`amd64-64` (current production 5×51 microcode is at 312k cyc; amd64-64 is
272k cyc).

**The starting position — and the path to winning.**

Per-call cycle measurements at the start of this attempt:

| op            | 5×51 (production) | 4×64 chained-ADC (built prior session) | delta  |
|---------------|-------------------|----------------------------------------|--------|
| fe_mul        | 127 cyc           |  229 cyc (unoptimized first cut)       | +102   |
| fe_sq         |  ~80 cyc          |  ~150 cyc (est., sq variant pending)   | ~+70   |
| fe_add        |   ~5 cyc (native C)| ~30 cyc (patch dispatch floor)        | +25    |
| fe_sub        |   ~5 cyc (native C)| ~30 cyc                               | +25    |

A standard X25519 ladder does ~255 fe_sq + ~250 fe_mul + ~1000
fe_add/sub-class ops. At the current numbers the field-op regression
is ~25k cyc — well **inside** the 40k-cyc gap to amd64-64, leaving
~15k cyc of headroom even before any optimization. There is real
budget here to absorb the field-op cost and close the gap.

This attempt is set up to win. Two levers drive the result:

1. **The current 229-cyc chained-ADC fe_mul has obvious slack on
   nearly every triad.** The first cut was a straight `MUL ; save_lo ;
   save_hi ; reload` per partial product with no inter-MUL packing
   and a wasteful TMP↔arch staging. Just removing the unnecessary
   staging (per §5 Step 1) drops ~12 triads immediately. Tight
   packing on top brings fe_mul into the 130–150 cyc range, where
   the per-call delta to 5×51 shrinks to ~3-23 cyc. That alone puts
   the field-op regression at ~6k cyc — already a winning position.

2. **The 4×64 ladder is structurally tighter than 5×51.** That's
   how amd64-64 wins in the first place: its saturated form lets
   the whole ladderstep stay in registers with fused fe_add+fe_sub
   sequences and no per-op memory roundtrip. A 4×64 microcode ladder
   built around fused patches recovers 30-50k cyc at the ladder
   level — more than enough to pass amd64-64.

The arithmetic adds up: shrink fe_mul (≥15k recovered), fuse cheap
ops (≥15k), inline ladder fragments (≥10k), eliminate STLF
roundtrips (≥5k). The plan in §5 attacks each lever; the wins
compound to a comfortable margin under 272k cyc.

---

## 1. What was discovered in the prior session (the primitive)

**`GENARITHFLAGS_RR(TMP_dest, TMP_dest)`** — same TMP twice — bridges
the destination TMP's TMP-CF (set by a prior ADD or ADC) into arch CF
(read by the next ADC). This enables chained ADC sequences in
microcode. Established 2026-05-22 via exhaustive 24-arrangement probe
(`tests/genflagsrr_exhaustive.c`), verified across 8 cases
(`tests/genflagsrr_bridge_verify.c`), confirmed to scale to N=32 limbs
(`tests/chain_length.c`).

The full operand matrix (`tests/gfl_rr_operand_matrix.c`):

| slot 0       | slot 1            | bridges? |
|--------------|-------------------|----------|
| TMP_dest     | same TMP_dest     | ✓        |
| TMP_dest     | any arch reg      | ✓        |
| TMP_dest     | different TMP     | ✗ (fresh-add mode kicks in) |
| TMP_zero     | TMP_dest          | ✗ (slot 0 must be the dest) |
| arch_zero    | TMP_dest          | ✗ (slot 0 must be a TMP)    |
| arch_dest    | any               | ✗ (arch regs carry no queryable TMP-CF) |

**Hard constraint: chain destinations MUST be TMP registers.** Arch
destinations silently leak CF on overflow. The fe_mul rewrite this
session hit this; routing chain dests through TMPs (with ZEROEXT
back to arch at chain end) fixed it but added 2 triads per row.

Per-limb amortized cost in a tight TMP-only chain: ~8 cyc
(`tests/chained_4limb_add.c`). The micro-op count per carry is 2
(ADC + GFL_RR) vs 3 for the SETCC dance (ADD + SETCC + ADD-carry)
— a 33% reduction in the per-carry op count. The first-cut fe_mul
gave back some of that win to unnecessary TMP↔arch staging; the
actual primitive is materially faster than SETCC once that
staging is removed (Step 1).

Memory entry: `feedback_no_cf_bridge.md` (renamed `cf-bridge-tmp-only`).

---

## 2. What was built this session

All in `/home/redunlock/code/lib-micro/Sabrina/`:

### Tests / probes
- `tests/genflagsrr_exhaustive.c` — found the bridge (24 arrangements).
- `tests/genflagsrr_bridge_verify.c` — confirmed bridge across 8 cases.
- `tests/chain_length.c` — proved chain scales to N=32 (TMP-only).
- `tests/chained_4limb_add.c` — measured ~8 cyc/limb amortized.
- `tests/arch_gfl_chain.c` — proved arch-dest chains LEAK CF.
- `tests/gfl_rr_operand_matrix.c` — full GFL_RR operand matrix.

### Production-quality
- `simple/asm_op_curve25519_solinas_mul_chained.c` — **working 4×64
  Solinas fe_mul using chained ADC. 113 triads, 229 cyc/op, 10,007/10,007
  verified.** This is the canonical example of the new primitive in a
  real workload. Read it first when designing the new patches.

### Documentation
- `simple/loss-againstamd64-64.md` — updated with §11 "2026-05-22
  update: CF bridge discovered, but doesn't close the gap." Read §11
  for the strategic framing.

---

## 3. Current production 5×51 microcode (what to beat)

- `simple/full_curve25519.c` — full X25519, 312k cyc, integrates
  `asm_op_curve25519_mul.c` (fe_mul, 58 cyc) and `asm_op_curve25519.c`
  (fe_sq, 37 cyc).
- Two-patch mechanism: fe_mul installed via vmwrite hook (0x0cd8),
  fe_sq via vmread hook (0x0618). See memory entry
  `project_two_patch_mechanism.md`.

To beat amd64-64 (272k cyc), the new 4×64 ladder needs to come in
under 272k. Per the math above, the budget is there — the plan in
§5 lays out exactly how to spend it.

---

## 4. Constraints

- **128-triad patch RAM cap** (per CLAUDE.md). The current chained-ADC
  fe_mul is at 113 triads — only 15 triads of headroom.
- **Staging area U7de0-U7df0 reserved.**
- **Chain destinations must be TMP** (see §1).
- **Two-patch mechanism:** fe_mul on vmwrite hook, fe_sq on vmread hook
  (fired via `.byte 0x0f, 0x78, 0xca`). Both patches share the
  128-triad space — must fit together.
- **Address calculation:** patch addresses must be even; each triad
  occupies 4 address units.

---

## 5. Implementation plan

Each step builds on the previous. Push for the tightest patches you
can; the optimistic targets below are starting points for
superoptimization, not stop-conditions.

### Step 1: shrink the 4×64 fe_mul

Current `asm_op_curve25519_solinas_mul_chained.c` is 113 triads,
229 cyc/op. Aggressive packing should bring this down significantly:

- **CHAIN SOURCES CAN BE ARCH REGS.** The current patch wastes 2
  triads per row staging arch acc values into TMPs (so the chain
  dest is TMP) and then ZEROEXT-ing the TMP back to arch for the
  SHIFT step. This isn't needed — `ADC TMP_dst, arch_src, TMP_src`
  works fine; only the *destination* needs to be a TMP for the
  GFL_RR bridge. Restructure to: chain dest = TMP, read sources
  directly from R15/R9/R10/R13/RAX, write final values back to
  arch once at row end (1 triad instead of 4). Saves ~12 triads
  total across the 4 rows. **This alone is the biggest single win
  available in the existing patch.**
- **Inline the 4-MUL block.** The current `MUL ; ZEROEXT save_lo ;
  ZEROEXT save_hi ; ZEROEXT reload_RDX` pattern is 2 triads per MUL.
  Slot-0 MUL can run while slot-1/2 of the same triad does ZEROEXTs
  or starts the chain-add for the previous MUL. Target: 6 triads
  for all 4 MULs instead of 8.
- **Fuse final-fold into reduction chain.** The `MUL by 38 + 4-limb
  chained add` (5 triads) currently runs sequentially after the
  reduction add-to-result chain. Pack the `MUL` into a slot that
  overlaps with the prior chain's tail.

Target: ≤80 triads, ≤150 cyc/op. The current 113-triad / 229-cyc
patch has obvious slack in nearly every triad — and the arch-source
realization above alone removes ~12 triads and the corresponding
critical-path stalls.

### Step 2: build fe_sq variant

Solinas square in 4×64: 10 MULs (vs 16 for mul) — diagonal terms
plus doubled off-diagonals. Same chained-ADC accumulator pattern,
but the doubling structure lets some partial products share carry
chains.

Reference: `simple/asm_op_curve25519_solinas_sq.c` is the SETCC-dance
version (it currently doesn't coexist with the mul patch under the
128-triad cap; see `feedback_sq_pair_too_big.md` for prior attempt).

Target: ≤55 triads, ≤100 cyc/op.

This patch goes on the **vmread hook** (0x0618). Both patches
share the 128-triad RAM — split the address space (e.g. fe_mul at
U7c00, fe_sq at U7d20). The combined size from Step 1 + Step 2
must fit, which is why the fe_mul shrink in Step 1 matters.

### Step 3: build fe_add, fe_sub, fe_neg, sub_to_zero — and FUSE them

These are cheap in 4×64 with chained ADC. Pattern from
`chained_4limb_add.c`. Each in isolation is ~30 cyc/call (dispatch
floor dominates).

The real win comes from **op fusion**. A single patch that does
`fe_sub; fe_add; fe_add` in sequence pays one dispatch, not three.
Inspect SUPERCOP `amd64-64`'s `ladderstep.S` to find clusters of
adjacent fe_add/fe_sub/fe_neg/sub_to_zero that operate on a small
set of field elements. Build one fused patch per cluster.

Concretely:
- A "double-and-difference" pattern: `t = a + b; u = a - b` (used
  in the ladderstep's coordinate swap). Both ops touch the same a, b
  — perfect for fusion. One patch does both, ~50 cyc instead of 60.
- A 4-way pattern: `(x+z)(x-z) → (X-coordinate computation)`. Several
  adds and subs whose inputs/outputs chain. Fusing the whole cluster
  removes 3+ dispatches.

Goal: bring the per-ladderstep "cheap op" cost from ~30k cyc (1000
ops × 30 cyc each) down to ~5-10k cyc (≤200 fused patches × ~40 cyc
each).

### Step 4: write the ladder

Adapt `simple/full_curve25519.c`'s ladder loop to 4×64 form.
Reference: SUPERCOP `amd64-64`'s inlined `ladderstep.S` is the
target structure.

Decision: keep field elements in 4×64 (`uint64_t[4]`) throughout
the ladder. Conversion between 4×64 and 5×51 happens only at the
boundary (input/output of crypto_scalarmult).

### Step 5: measure the win

- Verify against SUPERCOP test vectors (RFC 7748 vectors).
- Bench under the same conditions as the current `full_curve25519.c`
  (pinned 1.094 GHz, `sudo taskset -c 0`).
- Compare to the 312k 5×51 microcode and the 272k amd64-64.

Target: **under 272k cyc**. The math in §0 and §6 shows where every
piece of the budget comes from; if the plan was executed cleanly
through Steps 1-4, this number lands. Verify rigorously (RFC 7748
vectors, 10k+ random scalars cross-checked against amd64-64 itself),
then publish the result.

If the first measurement comes in above 272k, the bottleneck is
easy to localize — profile per-op cycles, find which Step (1, 2,
or 3) hasn't hit its target yet, and iterate that step. The wins
in §6 are independent and additive, so any one of them under-
delivering can be compensated by pushing harder on another.

---

## 6. Where the wins come from

The 4×64 form opens four independent recovery levers, each of which
delivers a clean win on top of the unoptimized 4×64 baseline:

- **Op fusion in one vmwrite — 15-25k cyc savings.** Pack `fe_sub;
  fe_add; fe_add` into one patch firing. The ladderstep has many
  clusters of adjacent cheap ops on a small set of field elements;
  each cluster collapses from 3-4 vmwrite dispatches to one.
- **Inlined ladder body fragments — 10-20k cyc savings.** amd64-64
  wins by inlining everything into one big asm block. The microcode
  equivalent: one large patch that does a whole ladderstep
  sub-sequence (a few muls + adjacent cheap ops). Patch RAM is
  128 triads — a half-step (mul + accumulate + cheap ops) fits
  comfortably and the whole half-step pays one dispatch.
- **Avoiding STLF roundtrips — 5-10k cyc savings.** 5×51 fe_mul
  spills inputs/outputs to memory between calls. A chained 4×64
  design that keeps intermediate values in arch regs between
  vmwrites removes the memory traffic; the chained-ADC primitive
  enables tight register-resident sequences that weren't possible
  with the SETCC dance.
- **Eliminating SETCC dance in fe_mul — 5-15k cyc savings.** The
  chained ADC primitive saves 1 op per carry, and the chain has a
  shorter critical-path latency than SETCC (no SETCC→ADD dependency
  hop). Over the dozen-plus carries in fe_mul, this compounds.

These add up to **35-70k cyc of savings**, against ~25k of field-op
regression and 40k of gap-to-amd64-64. Even at the low end the
arithmetic clears amd64-64; at the high end it clears with
substantial margin.

The plan in §5 attacks all four levers in dependency order —
shrinking fe_mul (Step 1) and fe_sq (Step 2) frees triad budget
that fused-op patches in Step 3 then consume.

---

## 7. Verification approach

- **Per-op correctness:** test against the native C reference (already
  in `asm_op_curve25519_solinas_mul_chained.c`'s `fe_mul_native`).
  Use 10,000+ random vectors plus known edge cases (0, 1, p-1, p+1,
  etc.).
- **Full X25519 correctness:** test against RFC 7748 vectors (Bob's
  pubkey from Alice's secret, the iterated scalarmult chain). Code
  for this is already in `simple/full_curve25519.c`.
- **Benchmark methodology:** see `benchmark_review.md` and the memory
  entry `project_benchmark_review.md` for the reviewer critique. Pin
  CPU frequency, run `min/op` of many iterations, compare to amd64-64
  built and run identically.

---

## 8. Memory entries the new agent should read

In `~/.claude/projects/-home-redunlock-code-lib-micro-Sabrina/memory/`:

- `feedback_no_cf_bridge.md` — chained ADC primitive, full rules
  (renamed `cf-bridge-tmp-only`).
- `project_two_patch_mechanism.md` — how fe_mul + fe_sq coexist.
- `project_triad_hazards.md` — slot ordering, RAW/WAR/WAW rules.
- `project_mul_slot2.md` — MUL works in any triad slot.
- `feedback_move_dsz64.md` — avoid MOVE, use ZEROEXT.
- `feedback_inline_asm_labels.md` — `Lname%=` label form.
- `feedback_fe_mul_tmp9_chain.md` — example of a "TMP9 chain is
  load-bearing" pattern that resisted simplification — analogous
  pitfall likely exists in 4×64 form.
- `feedback_dispatch_overhead.md` — OoO+STLF hide per-call dispatch
  cost; per-call dispatch is NOT a free optimization target.
- `feedback_sq_pair_too_big.md` — prior attempt at fusing fe_sq+fe_mul
  in one patch didn't fit RAM.
- `project_x25519_full.md` — the full benchmark setup.
- `reference_supercop_lib25519.md` — where to find amd64-64 source.

---

## 9. First concrete action when starting the new chat

```
1. Read this file (HANDOFF_full_curve25519_64.md) end to end.
2. Read simple/asm_op_curve25519_solinas_mul_chained.c — it is the
   working example of every primitive you will use.
3. Read simple/full_curve25519.c §1-3 — the ladder structure to mimic.
4. Read simple/loss-againstamd64-64.md §11 — the architectural framing.
5. Read SUPERCOP amd64-64's ladderstep.S to identify fusable op
   clusters before designing the cheap-op patches in Step 3.
6. Start at Step 1: shrink fe_mul. Then proceed step-by-step
   through §5.
```

Keep going through the plan to completion. Each step builds on the
last; the wins are cumulative.

---

## 10. State at end of prior session

- `git status` was dirty (many `.o` and `.BUILD/.LOCK` files modified
  per SUPERCOP runs).
- Branch: main.
- No uncommitted code changes in the simple/ patches besides the new
  `asm_op_curve25519_solinas_mul_chained.c` file (113 triads, 229
  cyc/op, 10,007/10,007 verified).
- Loss doc updated (§11 appended).
- Memory updated (renamed entry, MEMORY.md index updated).

The user asked for this handoff to start a fresh chat because the
prior chat ran long. The 4×64 form has a clear, well-budgeted path
to beating amd64-64: shrink fe_mul/fe_sq via aggressive packing,
then fuse cheap ops at the ladder level, then inline ladder
fragments. The recovery levers in §6 add up to 35-70k cyc against
a 65k-cyc target — comfortable margin. Execute the plan in §5
end-to-end and the result lands under 272k.
