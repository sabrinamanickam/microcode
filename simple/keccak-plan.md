# Plan: Beat SUPERCOP `crypto_hash/keccakc1024/x86_64_asm` with Microcode

## Context for the assistant reading this

You are helping me implement Keccak-f[1600] in Intel microcode (patch RAM) on a Goldmont CPU, with the goal of beating the SUPERCOP `x86_64_asm` reference implementation of `crypto_hash/keccakc1024`.

I have prior microcode experience: I've successfully implemented `fe_sq` (Curve25519 squaring) in microcode hooked on `vmwrite`, using the `patch_ucode()` / `hook_match_and_patch()` infrastructure. The macro names (`ADD_DSZ64_DRR`, `MUL_DSZ64_DRR`, `SHL_DSZ64_DRI`, `SHR_DSZ64_DRI`, `NOTAND_DSZ64_DRR`, `ZEROEXT_DSZ64_DR`, `OR_DSZ64_DRR`, `SETCC_CONDB_DR`, etc.) and triad structure (3 uops + 1 seqword, ending with `NOP_SEQWORD` or `END_SEQWORD`) follow the conventions in `include/ucode_macro.h` and `include/patch.h`.

### Confirmed environment

- **CPU**: Goldmont (Intel Atom 506c9, matches the SUPERCOP "wooden" benchmark machine)
- **ROL/ROR**: single-uop rotate primitive exists in `ucode_macro.h`
- **ANDN / NOTAND**: confirmed (`NOTAND_DSZ64_DRR` used in `fe_sq`)
- **BMI1/BMI2**: available in hardware (native side)
- **Patch RAM headroom**: ~10 dead patches in slots 1-18 can be cleared, freeing both uop slots and seqword bank entries

### Patch RAM architecture (from prior testing)

- 0x200 uop slots total (U7C00–U7DFF), each holds one micro-op
- Seqword address space is separate: 128 entries per bank, 4 banks selected by `(addr - 0x7c00) % 4`
- One triad = 3 uops + 1 seqword = 4 uop slots + 1 seqword slot
- `SEQ_GOTO0` works across non-contiguous regions; TMP registers survive cross-region jumps
- 4-aligned starts required for triad placement
- The binding constraint is usually **seqword bank space**, not uop slots
- See `patch_ram_findings.md` (separate document) for the full free-region map and the seqword arithmetic

### Baseline target

SUPERCOP `crypto_hash/keccakc1024/x86_64_asm` on Goldmont: ~46k cycles for the full harness call. **The relevant comparison is per-permutation cycles**, which on Goldmont is roughly 1100-1300 cycles for `KeccakF1600` in hand-tuned scalar assembly. This is the number to beat.

The SUPERCOP harness includes sponge wrapper overhead (absorb/squeeze/pad) which we won't be microcoding — only the permutation. So the real win condition is: per-permutation microcode cycles < per-permutation `x86_64_asm` cycles, and the wrapper overhead doesn't swallow the gain.

## Why this should work on Goldmont specifically

1. **Goldmont scalar is the SOTA on this microarch** — the SUPERCOP benchmark shows scalar implementations (`x86_64_asm`, `opt64lcu24`, `opt64lcu6`) all clustered at 46-47k cycles, while SSE variants land at 60-100k+ because Goldmont's 128-bit SSE is too narrow to overcome cross-lane shuffle costs. There's no AVX2 baseline to lose to.

2. **Register pressure** — Keccak state is 25 lanes × 64 bits. Native x86-64 has ~13 usable GPRs, forcing constant L1 spill traffic. Microcode TMP0–TMP15 plus the 14 GPRs available inside the patch gives ~29 registers — enough to hold most of the state resident across an entire round. This is a real architectural advantage native code cannot access.

3. **Goldmont has weak OoO** — it hides memory latency less well than Haswell+. Eliminated L1 round-trips help more here than on big cores.

4. **3-uop triad slots** — Keccak's wide-parallel steps (θ broadcast, χ across rows) have plenty of independent ops per cycle, well-suited to packing 3-wide.

Expected win: **5-25% over scalar asm**. This is not a `fe_sq`-style 3× blowout. Keccak is already near hardware-optimal on scalar; we're chasing a modest constant-factor improvement.

## Phase 0 — Establish the baseline (do first)

**Goal:** know the exact per-permutation cycle count on the actual hardware before writing any microcode.

Tasks:
1. Pull SUPERCOP source for `crypto_hash/keccakc1024/x86_64_asm` and `opt64lcu6`. Extract the `KeccakF1600` symbol.
2. Write a microbenchmark mirroring `bench_sq.c`: rdtsc-fenced batches of 1000 calls, 100 reps, min and avg.
3. Run pinned to one core (`taskset -c 0`) on Goldmont.
4. Also benchmark a **no-op microcode hook** (patch that immediately returns) to measure pure dispatch cost. If this is >15% of baseline permutation time, the hook strategy needs reconsideration before continuing.

**Deliverables before moving on:**
- Baseline per-permutation cycles for `x86_64_asm` (min, avg)
- Baseline per-permutation cycles for `opt64lcu6` (sanity check)
- No-op hook overhead in cycles
- Decision: proceed (hook overhead acceptable) or pick a different hook target

## Phase 1 — Free patch RAM (1 hour)

**Goal:** consolidate free regions into one contiguous block of ≥55 uop slots with ≥50 seqword entries available in its bank.

Tasks:
1. Audit slots 1-18 in the match-and-patch table. Identify which are dead (~10 expected).
2. Clear dead slots: zero destination, source, and enable bits.
3. Re-dump patch RAM; recompute free regions (see `patch_ram_findings.md` for the dump format and seqword arithmetic).
4. Verify one region near U7d7c can hold 50+ triads. If not, clear more, or fall back to `init_match_and_patch()` reset (Option D in findings).

**Notes:**
- Slot 14 (`U209c → U28d8`) is unusual — its target isn't in patch RAM. Likely a permanent fix, leave it.
- Slot 31 (the IN fix) is always needed.
- After clearing, the `do_fix_IN_patch()` call may still be required at startup.

## Phase 2 — Round body on paper (2-3 hours)

**Goal:** triad-accurate pseudo-microcode for one round, hand-verified against Keccak reference.

### State layout

Map 25 lanes A[x,y] (x,y ∈ 0..4) to registers:
- GPRs: RDI, RSI, RBX, RCX, RDX, RBP, R8, R9, R10, R11, R12, R13, R14, R15 (14 lanes)
- TMPs: TMP0–TMP10 (11 lanes) — total 25 ✓
- Reserve: TMP11, TMP12 for parity C[x] / broadcast D[x] scratch
- Reserve: TMP13 for round constant
- Reserve: TMP14 for round counter
- Reserve: TMP15 as general scratch

### Triad budget (estimated)

| Step | Triads | Notes |
|------|--------|-------|
| θ parity (compute C[0..4]) | 5-7 | Tree XOR depth 3; depends on 3-input XOR availability |
| θ broadcast (D[x] = C[x-1] ⊕ ROL(C[x+1], 1)) | 3 | Fuse ROL+XOR in 3-uop slots |
| θ apply (25× A[x,y] ⊕= D[x]) | 9 | 3 XORs per triad |
| ρ (25 ROLs by ρ-constants) | 9 | One ROL per uop slot, 3 per triad |
| π + χ fused (ANDN + XOR, output to permuted dest) | 17 | π is naming-only, no data movement |
| ι (A[0,0] ⊕= RC) | 1 | Single XOR with TMP13 |
| Loop control (dec counter, fetch RC, cond GOTO) | 3 | 3rd triad ends with SEQ_GOTO0_COND |
| **Total per iteration** | **~47** | Plus ~3 triad prologue, ~3 triad epilogue |

### Key design tricks

1. **π as renaming, not data movement.** Lane (x,y) after ρ+π lands at output position (y, 2x+3y mod 5). Hardcode which destination register receives each computed lane. Use two-round alternation if a single mapping doesn't close the cycle: even rounds read from set A and write set B, odd rounds read B write A. Adds zero triads.

2. **Fuse ρ into χ inputs where possible.** Each χ output needs three rotated inputs. If the ROL+ANDN can pack into a single triad (1 ROL + 1 ANDN + 1 XOR = 3 uops), some lanes collapse to fewer triads. Check whether ANDN can read a ROL output produced in the same triad — likely not, but worth checking the latency rules.

3. **Round constants:** prefer URAM-indexed lookup (`fetch RC[TMP14]` into TMP13). Fallback: LFSR computation in microcode (~3 extra triads/round) if URAM indexing isn't available or is slow.

4. **3-input XOR:** check whether `XOR3` or equivalent exists. If yes, θ parity drops from 7 to 5 triads. If no, the 7-triad version still fits the budget.

### Hand-verify

Pick a known input state (e.g., the all-zero state, or the first test vector from the Keccak team's `KeccakF-1600IntermediateValues.txt`). Walk through the pseudo-microcode by hand. Lane-by-lane comparison against:
- A reference C implementation (e.g., SUPERCOP `simple`)
- Or the Keccak team's published intermediate values

**Gate:** if the round body exceeds 50 triads, compress the ρ+π+χ phase before continuing. That step is 55% of the body and has the most slack.

## Phase 3 — Single-round prototype (4-6 hours)

**Goal:** prove one round works correctly and runs faster than scalar/24.

Tasks:
1. Translate the pseudo-microcode to a real `ucode_t` array. Hardcode the round-0 constant.
2. Hook on `vmwrite` (same pattern as `bench_sq.c`). Caller loads 25 lanes into GPRs and memory, microcode runs one round, microcode writes back.
3. Verify: pick a test state, run reference Keccak for exactly one round, compare lane-by-lane. Iterate until perfect match.
4. Benchmark single round vs (scalar baseline / 24).

**Gate:**
- microcode round < scalar round → ✅ proceed to Phase 4
- microcode round ≈ scalar round (within 10%) → proceed but watch loop overhead
- microcode round > scalar round by 20%+ → ❌ stop, debug. Likely a triad-packing or uop-latency issue. Use `perf` or PMU counters to find the stall.

## Phase 4 — Loop the rounds (3-4 hours)

**Goal:** full Keccak-f[1600] in microcode, verified against KAT vectors.

Tasks:
1. Add loop control triads (decrement, conditional `SEQ_GOTO0`, RC fetch).
2. Implement round constants:
   - **Option A:** URAM table indexed by TMP14. Cleaner. Try first.
   - **Option B:** LFSR computation in microcode. Use if A doesn't work.
3. Test against official Keccak KAT vectors (from `https://keccak.team/files/KeccakF-1600-IntermediateValues.txt` or the SUPERCOP test infrastructure).
4. Benchmark per-permutation cycles. **Compare to baseline.**

**Expected outcome:** 850-1100 cycles/permutation vs ~1100-1300 baseline = 5-25% win.

## Phase 5 — SUPERCOP integration (2-3 hours)

**Goal:** run the actual SUPERCOP harness with microcode-backed `KeccakF1600`.

Tasks:
1. Write a thin C wrapper `KeccakF1600_ucode(uint64_t state[25])` that loads state into the GPRs the microcode expects, issues the hook (`vmwrite`), and writes state back.
2. Drop into SUPERCOP `crypto_hash/keccakc1024/simple/` (or `opt64`), replace the permutation, keep the sponge wrapper.
3. Run SUPERCOP test vectors (`./do-part crypto_hash keccakc1024`).
4. Compare to the `x86_64_asm` baseline numbers.

## Phase 6 — Optimization (only if Phase 4 doesn't clear the win)

In order of expected payoff:

1. **Measure hook entry/exit overhead** with a no-op microcode patch. If >50 cycles, consider hooking a cheaper instruction.
2. **Two-round unroll.** Halves loop-control overhead at cost of ~35 extra triads. Viable only with substantial free patch RAM.
3. **Re-examine ρ+π+χ packing.** This step is the biggest triad consumer. Check whether some lanes can be computed earlier and stored in different uop slots to fill stalls.
4. **Goldmont-specific tuning:** 3-wide decode but only 2 ALU ports. The 3-uop triad model assumes 3 ALUs — if profiling shows the third slot is often empty due to port pressure, accept it. Not much to do.

## Risks and likely failure modes

**Most likely problem:** loop overhead too high. 3 control triads × ~5 cycles each = 15 cycles/round × 24 = 360 cycles just for loop control. If the baseline is 1100 cycles, that's 33% of the budget gone before any actual Keccak work. Mitigations:
- Combine RC fetch with first round-body triad (RC is only needed at ι, which is at the end)
- Unroll 2× if patch RAM allows

**Second most likely:** URAM-indexed read isn't available or has high latency. Fallback to LFSR-computed constants is straightforward but eats ~3 triads/round.

**Less likely but watch for:**
- ANDN can't read a ROL output produced in the same triad (latency dependency). Means ρ must complete before χ starts → adds 1-2 triads per row.
- π renaming hits register pressure: not all (x,y) → permuted destination mappings are free if some destinations are GPRs and some are TMPs, and the next round needs them in the opposite class.
- Goldmont microcode dispatch is slower than expected for tight loops. The hook fires once per permutation but the loop runs 24× inside — that's all microcode, no re-dispatch, so this is unlikely.

## What to do right now

If you're picking this up cold:

1. Read `patch_ram_findings.md` for the patch RAM model and free-region map.
2. Read `bench_sq.c` for the harness style, hook pattern, and triad encoding conventions.
3. Do **Phase 0** first. Get the actual baseline number. Everything else is wasted effort without it.
4. Report back with:
   - Baseline per-permutation cycles (min, avg)
   - No-op hook overhead in cycles
   - Updated free-regions table after clearing slots 1-18
   - Confirmation of `XOR3` / 3-input XOR availability in `ucode_macro.h`
   - Confirmation of URAM-indexed read primitive in `ucode_macro.h`

With those five pieces of information, write the actual `ucode_t` array for one round and proceed to Phase 3.

## Reference files

- `bench_sq.c` — example microcode patch with hook on `vmwrite`, benchmark harness, register-passing convention
- `patch_ram_findings.md` — patch RAM layout, seqword arithmetic, free-region computation, options for fitting larger sequences
- `include/ucode_macro.h` — micro-op macros (`ADD_DSZ64_DRR`, `ROL_*`, `NOTAND_DSZ64_DRR`, etc.) and seqword macros (`NOP_SEQWORD`, `END_SEQWORD`, `SEQ_GOTO0`)
- `include/patch.h` — `patch_ucode()`, `hook_match_and_patch()`, `init_match_and_patch()`
- Keccak reference: https://keccak.team/keccak_specs_summary.html
- SUPERCOP: https://bench.cr.yp.to/supercop.html
- Benchmark target: https://bench.cr.yp.to/web-impl/amd64-wooden-crypto_hash-keccakc1024.html

## Phase 0 results (2026-05-27)

Measured on this Goldmont via `bench_keccak.c` (BATCH=1000, REPS=100, pinned to core 0, rdtsc/rdtscp fenced):

| Implementation | min cyc/perm | avg cyc/perm |
|---|---|---|
| `x86_64_asm` (SOTA scalar)  | **939** | 965 |
| `opt64lcu6` (sanity, Bebigokimisa+u6) | 1011 | 1031 |
| no-op microcode hook (vmwrite, 1-triad `END_SEQWORD`) | **4** | 4 |

Cross-check: both impls produce identical output on a non-trivial test state and non-zero output on the all-zero state.

### Implications

1. **Hook overhead is essentially free.** Plan threshold was 15 % of baseline (~140 cyc). 4 cyc is 0.4 %. Hook strategy stays — vmwrite (0x0cd8) is fine.
2. **The win target is tight.** Baseline = 939 cyc min, i.e. **~39 cyc/round** on scalar asm. The 5–25 % win bracket is 705–892 cyc total, i.e. ~29–37 cyc/round.
3. **Plan's 47-triad/round budget puts us at a loss, not a win.**
   - 47 triads × 24 rounds × 1 cyc/triad + 4 cyc hook ≈ **1132 cyc** (worse than 939 baseline)
   - Even at fe_sq's empirical 0.88 cyc/triad: 47 × 24 × 0.88 ≈ 992 cyc (still worse)
   - To clear the +5 % bar (892 cyc): need ≤ 37 triads/round at 1 cyc/triad, or ≤ 42 at 0.88 cyc/triad.
4. **Conclusion: proceed, but with a stricter triad budget than the plan originally framed.** The ρ+π+χ phase (estimated 17 triads, 55 % of the body) is where the slack lives.

### Primitive availability (confirmed in `include/`)

| Primitive | Form | Status |
|---|---|---|
| `ROL_DSZ64` | `_DRI` (imm count), `_DRR` (reg count) | ✓ used in fe_sq family |
| `NOTAND_DSZ64` (ANDN for χ) | `_DRR` | ✓ |
| `XOR_DSZ64` | `_DRR` | ✓ — but **only 2-input**. No `XOR3`. θ parity stays at ~7 triads (5 cols × 4 XORs / 3 ops per triad). |
| `READURAM_DR(dst, src)` | register-indexed URAM read | ✓ — RC lookup via TMP14 counter is viable (Option A). Latency TBD. |
| `WRITEURAM_RI` / `WRITEURAM_RR` | URAM init | ✓ |
| Reminder: `MOVE_DSZ64_*` traps to slow path. Use `ZEROEXT_DSZ64_DR` for reg-to-reg, `ZEROEXT_DSZ32_DI` for immediates. |||

### Phase 0 deliverables — done

- [x] Baseline `x86_64_asm` cycles: **939 / 965** (min / avg)
- [x] Baseline `opt64lcu6` cycles: 1011 / 1031
- [x] No-op hook overhead: **4 cyc**
- [x] Confirmed: no XOR3, but `READURAM_DR` is available
- [x] Decision: **proceed to Phase 1** with a revised triad budget (target ≤ 40 triads/round, not the originally estimated 47)

## Phase 1 results (2026-05-27)

Reality is simpler than `patch_ram_findings.md` (which was written without `init_match_and_patch()` in the picture). Empirical probe (`probe_keccak_capacity.c`) at U7c00 after init:

- **128 triads execute end-to-end at U7c00**, occupying U7c00 – U7dfc (the entire bank-0 region).
- v1 of the probe hit a wall at 120 because it called `hook_match_and_patch()` *after* `patch_ucode` each iteration. `hook_match_and_patch` writes a 5-triad bootstrap to U7de0-U7df0, invokes it, then leaves it dead — but if we call it *after* writing the user patch, it clobbers triads 120-124 of the patch tail.
- v2 (hook installed once up front, before the loop) runs the bootstrap once, leaving it dead, and the user patch overwrites U7de0-U7dfc freely. **Empirical ceiling: 128 triads** — exactly the theoretical bank-0 seqword limit.
- The same applies to `init_match_and_patch()` and any helper that uses `ucode_invoke`-style bootstraps: their U7de0-U7df0 region is reclaimable as long as the helper is called *before* the production patch is written.

### Implications

1. **No dead-slot clearing needed.** `init_match_and_patch()` already wipes all 32 hook slots; the BIOS-resident uop/seqword junk in patch RAM is orphaned (no hook points there) and gets overwritten cleanly by `patch_ucode(0x7c00, …)`. The pessimistic "50 usable triads across fragments" analysis in `patch_ram_findings.md` was the no-init worst case.
2. **128 triads ≈ 2.5× a single round body.** Critically, this unlocks a **2-distinct-body unroll** (each body ~44 triads, 88 total). A single looped body would need explicit per-round shuffling (+8 triads/round = 52/round × 24 = 1248 triads-issued); a 2-body design closes the π-orbit drift via hardcoded writes in body 1, eliminating the shuffle.
3. **Phase 2 unblocked.** Triad budget for 2-body × 12-iter ≈ 88 triads, comfortably under the 128 cap. Cycle math (below) is now the binding constraint.

### Phase 1 deliverables — done

- [x] Usable triads at U7c00 after init: **128** (empirically verified, v2 probe)
- [x] Confirmed: bootstrap region U7de0-U7dfc is reclaimable when helpers are called before the production patch
- [x] Decision: **proceed to Phase 2** — 2-distinct-body design, target ≤ 44 triads/body

## Phase 2 design (2026-05-27)

### Why 2 bodies, not 1, not 4

π is a 24-cycle on the 24 non-(0,0) lanes (order 24, no smaller period). For a looped body to close:

| K-body | Iterations | π^K = id? | Closure mechanism | Triad cost / iter | Fits in 128? |
|---|---|---|---|---|---|
| 1 | 24 | π¹ ≠ id  | explicit shuffle (+8 triads/round) | ~52 × 24 = 1248 | yes (52 ≤ 128) |
| 2 | 12 | π² ≠ id  | hardcoded writes in body 1 restore canonical | ~88 | **yes (88 ≤ 128) ✓** |
| 3 | 8  | π³ ≠ id  | hardcoded writes in body 2 | ~132 | no |
| 4 | 6  | π⁴ ≠ id  | hardcoded writes (SUPERCOP inplace pattern) | ~176 | no |
| 24| 1  | id       | full hand-rolled | ~1056 | no |

**2 bodies is the unique design point** that closes π-orbit drift with hardcoded writes while fitting in the 128-triad patch RAM.

### Closure mechanism

Let `φ` be the canonical register mapping `(x,y) → R_φ(x,y)`. Two layouts:
- **L0 (canonical):** lane `(x,y)` lives in `R_φ(x,y)`
- **L1 (π-drifted):** lane `(x,y)` lives in `R_φ(π(x,y))` — i.e., to fetch lane `(x,y)`, read register `R_φ(π(x,y))`

Body design:
- **Body 0:** reads from L0 (canonical positions), writes new-state `(x,y)` to `R_φ(π(x,y))` → state ends in L1
- **Body 1:** reads from L1 (drifted positions), writes new-state `(x,y)` to `R_φ(x,y)` → state ends in L0

Both bodies compute the same Keccak round function. They differ **only** in their source/destination register names. Loop closes after every 2 rounds, total 24 rounds in 12 iterations.

### Register allocation (canonical, φ)

```
(0,0)=RDI  (1,0)=RSI  (2,0)=RBX  (3,0)=RCX  (4,0)=RDX     row y=0
(0,1)=RBP  (1,1)=R8   (2,1)=R9   (3,1)=R10  (4,1)=R11     row y=1
(0,2)=R12  (1,2)=R13  (2,2)=R14  (3,2)=R15  (4,2)=TMP0    row y=2
(0,3)=TMP1 (1,3)=TMP2 (2,3)=TMP3 (3,3)=TMP4 (4,3)=TMP5    row y=3
(0,4)=TMP6 (1,4)=TMP7 (2,4)=TMP8 (3,4)=TMP9 (4,4)=TMP10   row y=4

Scratch: TMP11..TMP14  (4 slots, free during θ/ρ/χ)
Persistent: TMP15 = round counter (across iterations and bodies)
```

For body 1, reads come from `R_φ(π(x,y))`. Since π is a 24-cycle on non-(0,0) lanes, body 1's read pattern is the canonical mapping precomposed with π. This is purely a different set of source-register names in each uop encoding — zero runtime cost.

### Per-body op count

| Phase | Ops | Triads (3 ops/triad floor) |
|---|---|---|
| θ parity (5 cols × 4 XOR) | 20 | 7 |
| θ D-broadcast (5 × (ROL+XOR)) | 10 | 4 |
| θ apply (25 XOR) + ρ (24 ROL) — fused | 49 | 17 |
| χ (25 ANDN + 25 XOR), interleaved across rows | 50 | 17 |
| ι (1 XOR, folded into χ tail) | (1) | 0 |
| RC fetch (READURAM_DR) + counter inc | 2 | 1 |
| Loop ctrl (body 1 only: UJMPCC SEQ_GOTO0_COND) | 1 | (in last triad seqword) |
| **Per body total** | **131** | **~46 triads** |

Two bodies = ~92 triads. Comfortably under 128 (36 triads of headroom for scheduling slack).

### Closure correctness

Body 0's writes go to `R_φ(π(x,y))`. After body 0:
- `R_φ(0,0)` holds new lane (0,0) (fixed point, π(0,0) = (0,0))
- `R_φ(0,2)` holds new lane (1,0) since π(1,0) = (0,2)
- ... etc., all 24 non-fixed-point lanes shifted by π

Body 1's `read_for_lane(x,y) = R_φ(π(x,y))` then fetches the right value. Body 1's `write_for_lane(x,y) = R_φ(x,y)` puts it back at canonical. Layout restored after 2 rounds. Loop closes.

### Open scheduling concerns (to surface during Phase 3 implementation)

1. **TMP scratch pressure during θ.** Computing 5 column parities into 5 separate scratch regs requires 5 slots; we have 4 (TMP11-14). Mitigation: pipeline C[x] → D[x] computation so at most 4 C-values live at peak; or recompute C[4] inline in D[3]/D[0]. Worst case +2-3 triads.
2. **READURAM_DR latency unknown.** May serialize a triad. If it costs >1 cyc, fold RC fetch into a non-critical path (e.g., during early θ when only a couple of scratch slots are busy).
3. **Two-body distinct write patterns.** Body 0's destination registers per output lane are `R_φ(π(x,y))`, body 1's are `R_φ(x,y)`. Easy to make table-driven mistakes — Phase 3 starts with a generator script that emits both bodies from the same Keccak round template.
4. **ι folds cleanly only into body 0 if RC[i] and RC[i+1] both need to land on (0,0).** Each body has its own ι and reads its own RC from URAM. Counter increments twice per iteration (once per body).

### Expected cycle cost

- 92 triads × 12 iter = 1104 triads issued + 4 hook overhead
- At 1.0 cyc/triad: **1108 cyc** — 18% LOSS vs 939 baseline
- At fe_sq's 0.88 cyc/triad: **975 cyc** — 4% LOSS vs baseline (essentially a tie)
- If we hit 0.85 cyc/triad: **942 cyc** — TIE
- If we shrink to 80 triads/iter × 12 = 960: 1.0 cpt → 964, 0.88 cpt → **849 cyc** (10% WIN)

The realistic outcome is **a tie or marginal loss**. A win requires both: (a) per-body ≤ 40 triads (8-12 triads under op-count floor — needs creative fusion not yet identified), AND (b) Goldmont microcode dispatch holds at fe_sq's 0.88 cpt or better for Keccak's op mix (no MUL → may run differently).

### Phase 2 deliverables — done

- [x] 2-body design with closure proof (above)
- [x] Register allocation table (canonical mapping φ)
- [x] Per-body op count: 131 ops, ~46 triads
- [x] Total triad budget: ~92 (within 128 cap)
- [x] Cycle projection: tie (best) to 18% loss (worst), unlikely to win > 5%
- [x] Decision: **proceed to Phase 3** with eyes open about the expected outcome

## Phase 3 results (2026-05-30)

Single Keccak-f[1600] round implemented in microcode and **verified correct on hardware** (matches C reference on random + zero-state vectors). Built via a self-verifying generator (`keccak_gen.py`): emits the op-list, simulates it on a register+memory model with the signed-offset constraint, asserts 24/24 rounds match the reference, then emits C (`keccak_round.h` + `_body.h`). Harness: `asm_op_keccak.c`.

**Hardware facts discovered (the hard way) — see memory `keccak-io-harness-seg`:**
- LDZX/STAD work only with **SEG_DS (0x18)**; SEG=3 hangs/crashes the core.
- LDZX/STAD offset field is **8-bit signed** (−128..+127 B = ±16 lanes) → base must be **centered**; a >32-lane buffer can't be covered by one base.
- Immediate field is **16-bit** (build larger constants with SHL).
- ROL_DSZ64 works (24/24 rho amounts); NOTAND(d,a,b)=(~a)&b.
- Intra-triad execution is **fully sequential** incl. 3-deep RAW chains (so dense 3-op packing is valid); store-to-load forwarding works within a patch; **≤1 memory op per triad** (else hang).

**Design that passed:** 30-lane buffer, base `RCX=&buf[14]` centered (offsets −112..+120), kept valid the whole round. State in 13 GPR + 12 TMP; θ-C in 5 scratch regs; θ-D stored to buffer lanes 25–29; apply does in-place π via backward cycle-following (the key insight: B lands at **canonical** register positions, so `REG[i]`=lane `i` before and after — **no π drift, the round loops trivially**); χ in-place per row; ι immediate XOR.

**Phase 3c benchmark (single round, incl. 50 I/O triads): 53 cyc min / 58 avg, 108 triads = 0.49 cyc/triad.** This is ~2× better than the 1 cyc/triad assumed in Phase 2, and flips the outlook:

| | triads | est. cyc |
|---|---|---|
| single round + I/O (measured) | 108 | 53 |
| compute only, per round (×24) | ~58 | ~28 |
| **looped permutation** (24×compute + I/O once + loop ctrl) | ~115 looped | **~740–1040** |
| baseline `x86_64_asm` | | **939** |

**Revised projection: ~20% win to small loss**, hinging on memory-op cost. Moving D from buffer into registers (kills 25 D-loads/round) likely secures the win.

### Phase 3 deliverables — done
- [x] Correct single round on hardware (108 triads)
- [x] Self-verifying generator (`keccak_gen.py`)
- [x] Per-triad cost measured: **0.49 cyc/triad**
- [x] Confirmed π self-resolves → looping needs no 2-body trick
- [x] Decision: **proceed to Phase 4** (loop the rounds); outlook now favorable
