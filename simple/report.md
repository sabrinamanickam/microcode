# Closing the gap between microcode X25519 and SUPERCOP `amd64-64`

**Date:** 2026-05-26
**Author:** Sabrina Manickam
**Target:** Intel Goldmont (Celeron N3350), pinned `taskset -c 0`, `sudo` for microcode patch install.

---

## 1. Starting position

Four X25519 implementations were in play:

| Implementation | min cyc | Notes |
|---|---:|---|
| `full_curve25519.c` (production) — our ladder + C wrappers around 5×51 microcode `fe_mul` / `fe_sq` patches | ~312,000 | Documented baseline |
| `full_curve25519_inline.c` — same patches with a per-iteration inline-asm ladder block to remove C-call overhead | ~315,500 | Earlier experiment; ≈ production within noise |
| `full_curve25519_4x64.c` — 4×64 saturated form mirroring `amd64-64`'s lazy-reduction, `fe_mul` via v3 chained-ADC microcode patch (75 triads), `fe_sq = fe_mul(a,a)` | ~604,000 | Verified 4/4 RFC 7748. Loses structurally |
| **`amd64-64/asm`** (SUPERCOP, Bernstein/Schwabe) — whole-stack hand-tuned amd64 asm, 4×64 saturated | **~272,000** | The number we are trying to approach |

**Starting gap from our best (production, 312k) to `amd64-64`: ≈ 40k cyc / 14%.**

---

## 2. Approaches considered

The day's investigation considered many directions; not all were pursued. This section lists each idea, the reasoning, and the verdict.

### 2.1. Rejected after analysis

#### Hybrid: `amd64-64`'s ladder + microcoded `fe_mul`/`fe_sq`

`amd64-64`'s `ladderstep.S` is one monolithic 6,580-line asm symbol with all five `mulq` blocks fully unrolled inline (≈ 690 `movq` total, mostly stack spills only when register pressure forces them). To "use the same style" with our microcode would require either:

- (a) calling out to our `fe_mul_ucode` from inside `ladderstep.S` — which is exactly what the existing `amd64-51-ucode` hybrid already does, and that loses; or
- (b) inlining the `vmwrite` directly into a copy of `ladderstep.S` with patch rewrites so the microcode reads operands from a stable arch-reg set.

Option (b) is multi-week work and the prior `x25519-4x64-loses` finding predicted no headroom, since the field-op cost itself dominates regardless of how the surrounding asm is laid out. **Abandoned.**

#### Further optimisation of `full_curve25519_4x64.c`

End-to-end 604k vs `amd64-64` 272k. Per X25519 we issue ≈ 1,287 `fe_mul` + 1,270 `fe_sq`. Each microcode call costs ≈ 200 cyc in this build, so the field ops alone account for ≈ 511k cyc — already nearly 2× `amd64-64`'s whole-function budget. Even with aggressive cheap-op fusion and a symmetric (squaring-optimised) `fe_sq` patch the projected floor is ≈ 480k, never below `amd64-64`. **Structural loss; abandoned.**

#### B1 — fused `add_then_sq` patch replacing `fe_sq`

Proposed as a way to reduce vmread/vmwrite count by absorbing `FE_ADD` into a fused patch with the following `FE_SQ`. **Discarded on closer inspection** because `FE_ADD`/`FE_SUB` are already pure inline asm in the ladder (no microcode call). The "ADD+SQ" pair uses ONE microcode call total today; fusing it into a patch still uses ONE microcode call, while *adding* ~5–8 triads to the patch for the in-patch ADC that previously overlapped with OoO. Net effect: small loss, not a gain.

This was a real "we caught the bug in the analysis before writing code" moment — a useful one, since it would have been ~4h of wasted patch work.

#### B2 — SQ+MUL fusion at step 13→14

Proposed as a way to fuse the only ladder adjacency where SQ's output feeds directly into MUL's input (`z3' = sq(t0); z3 = mul(x1, z3')`). Patch size ≈ 36 (sq) + 54 (mul) + ~5 marshaling = ~95 triads. Eliminates 1 vmwrite/iter × 255 iters = 255 calls. Expected saving: 255 × ~12 cyc ≈ ~3k cyc.

The blocker is the 128-triad RAM cap. Installing the fused patch would force dropping `fe_mul` (or `fe_sq`), which means the 11 non-fused muls in `fe_invert` (and the 4 unfused muls per ladder iter) would have to route through the fused patch with a "dummy sq input" — paying ~36 cyc each, ~14k cyc total added. **Net loss; rejected.**

#### Phase A fusion (compute `AA` and `BB` in one patch)

Patch would take `x2, z2` and internally compute `A = x2+z2`, `B = x2-z2`, `AA = sq(A)`, `BB = sq(B)`. Size ≈ 87 triads. Eliminates 2 vmreads/iter × 255 = 510 calls; saving ≈ 6k cyc.

Same 128-triad RAM blocker. Replacing `fe_sq` means the 254 pure sqs in `fe_invert` route through Phase A with `z2 = 0`, paying for two squarings to get one result. Cost: ~38 extra cyc × 254 = ~9.6k cyc added on `fe_invert`. **Net loss; rejected.**

#### Putting "more of the ladderstep" in one microcode call

Considered putting all 9 microcode ops of a ladderstep (5 mul + 4 sq) into one patch. Triad cost: 5 × 54 + 4 × 36 + adds/subs ≈ **439 triads**, well over the 128-triad cap. Also hits register-pressure issues — outputs alone (AA, BB, E, DA, CB, x3', z3, x2', z2) total > 16 GPRs. **Architecturally impossible on Goldmont.**

### 2.2. Pursued

- **§3: Register chaining inside the 5×51 inline-asm ladder.** Done; banked +3.24%.
- **§4: Inlined `fe_invert` with SQ chains.** Done; banked +1.01% on top.
- **§5: vmwrite dispatch-cost probe.** Done; gives the empirical number that justifies the rejections above.

---

## 3. Register chaining in the ladder (`full_curve25519_inline2.c`)

### 3.1. Why the existing inline impl had headroom

Read of `full_curve25519_inline.c`:

- `ladder_step` is a single `asm volatile` block containing 13 macro-expanded ops (4 `FE_MUL` + 4 `FE_SQ` + 5 `FE_ADD`/`FE_SUB`) — but **each macro independently reloads inputs from memory at entry and stores outputs to memory at exit.**
- `FE_MUL` (lines 345–363): 10 `mov reg, [rbp+off]` + `vmwrite` + 5 `mov [rbp+off], reg` per call.
- `FE_ADD`/`FE_SUB` serialised all 5 limbs through a single `rax`, so the result was never left in distinct registers a downstream op could pick up.
- The "tail" of the iteration (`mul121665` + `add` + `mul`) is a C function call between two asm blocks — an additional boundary.
- `fe_invert` is **not** inlined at all: 266 separate C-wrapper calls each doing its own load → `vmwrite` → store cycle.

So while the C-call layer was already removed, the inter-op **memory traffic** (which `amd64-64` largely avoids) was not. Hypothesis: closing that gap is worth real cycles.

### 3.2. Design

Forked `full_curve25519_inline.c → full_curve25519_inline2.c`. Changes:

1. **Restructured `FE_ADD` and `FE_SUB`** to leave the 5 result limbs in registers `{rdi, rsi, r12, r11, r14}` — the exact register set `FE_SQ`/`FE_MUL` expect for their `a` slot. Memory store of the result is retained for downstream non-chained consumers.

2. **Tightened `FE_SUB`.** The limb-1..4 bias constant (`0xFFFFFFFFFFFFE`) is loaded into `rcx` once and reused, saving 3 `mov rcx, imm64` per `FE_SUB` call.

3. **New chain-aware macros:**
   - `FE_SQ_FROM_REGS(out)` — assumes `a[0..4]` already in `{rdi, rsi, r12, r11, r14}`. Skips the 5 input loads.
   - `FE_MUL_FROM_REGS_A(out, b)` — assumes `a[0..4]` already in input regs; loads only `b`.

4. **Hand-scheduled the ladder iteration** to use chained variants in **7 places**:

   | step | chained pair | saves |
   |---|---|---|
   | 1→2 | `A = ADD(x2,z2); AA = SQ(A)` | 5 loads |
   | 3→4 | `B = SUB(x2,z2); BB = SQ(B)` | 5 loads |
   | 7→8 *(reordered)* | `D = SUB(x3,z3); DA = MUL(D,A)` | 5 loads |
   | 6→9 *(reordered)* | `C = ADD(x3,z3); CB = MUL(C,B)` | 5 loads |
   | 10→11 | `t0 = ADD(DA,CB); x3' = SQ(t0)` | 5 loads |
   | 12→13 | `t0 = SUB(DA,CB); z3' = SQ(t0)` | 5 loads |
   | tail | `t0 = ADD(AA,t0); z2 = MUL(E,t0)` *(uses commutativity: t0·E = E·t0)* | 5 loads |

   The 6/7 reorder is the key trick: scheduling `D = SUB` immediately before `DA = MUL` (chain) and `C = ADD` immediately before `CB = MUL` (chain) gives **two** chains in this section where the original schedule allowed only one.

### 3.3. Verification

RFC 7748 § 5.2 vectors:

```
Vector 1 (Alice/Bob low-order test):  PASS
Vector 2 (mid-range scalar/point):    PASS
Iterated test, 1 round:               PASS
Iterated test, 1000 rounds:           PASS  (output 684cf59b…32c51 matches RFC)
```

### 3.4. Measured result

| Variant | min cyc | median cyc | p90 cyc |
|---|---:|---:|---:|
| `full_curve25519_inline`  (baseline) | 315,548 | 315,776 | 321,623 |
| `full_curve25519_inline2`  (5 chains only) | 307,560 | 307,772 | 318,227 |
| `full_curve25519_inline2`  (7 chains + reorder) | **305,312** | **305,457** | **311,373** |

**Improvement vs baseline: −10,236 cyc on min / −10,319 on median / −10,250 on p90 ≈ 3.2 % each.**

### 3.5. Breakdown of the 10k saving

- **≈ 5k cyc — `FE_SUB` tightening.** The original macro reissued `mov rcx, IMM64` five times per call. The new version issues it twice. Per X25519: 4 `FE_SUB`/iter × 255 iter × 3 saved movs ≈ 3,060 mov-eliminations.
- **≈ 5k cyc — register chains across 7 op-pairs.** Each chain eliminates 5 memory reloads (35 per iter × 255 = 8,925 load-eliminations), partly hidden by store-to-load forwarding (STLF), netting ≈ 5k actual cycles.

This contradicts an earlier internal note that claimed inline-asm restructuring of the 5×51 ladder produced "zero cycles" of improvement — that observation was about removing the C-wrapper boundary, which doesn't address inter-op memory traffic. Once the chains are added, real cycles fall out.

---

## 4. Inlined `fe_invert` with SQ chains

### 4.1. Motivation

After §3, `fe_invert` was still calling per-op C-wrappers (`fe_sq_wrap`/`fe_mul_wrap`) — 266 separate calls, each doing its own load → vmread → store cycle. The Fermat addition chain has long runs of consecutive squarings (5, 10, 20, 10, 50, 100, 50, 5) where the output of one sq feeds the input of the next — perfect candidates for register chaining.

### 4.2. Design

Replaced `fe_invert` with a single function whose body is one large inline-asm block:

1. Allocated `invert_state_t` on stack with offsets for `z, z2, z9, z11, t, t0, t1, t2, t3`.
2. New chain macros analogous to ladder's:
   - `INV_SQ_LOAD(a)` — load `a` from memory into sq input regs.
   - `INV_SQ_OP` — execute sq with input already in regs (no load), output to sq output regs.
   - `INV_SQ_RENAME` — rename sq output regs `{rdi, r9, r10, rbx, rax}` to sq input regs `{rdi, rsi, r12, r11, r14}`. `rdi` is already correct (`h[0] = a[0]`); 4 movs cover the other 4 limbs.
   - `INV_SQ_STORE(out)` — store sq output to memory.
   - `INV_MUL(out, a, b)` — full standalone mul.
3. Sq-chains of length N expressed via GAS `.rept N-2` between `INV_SQ_CHAIN_START` and `INV_SQ_CHAIN_END`. The 100-sq chain becomes 4 lines of inline asm.
4. Deleted `fe_mul_wrap` and `fe_sq_wrap` (no longer used).
5. The post-invert `x2 = x2 * z2` step (called once per X25519) is also inlined as a small asm block.

### 4.3. Verification

RFC 7748 § 5.2 — all 4 vectors pass, including the 1000-iteration consistency test.

### 4.4. Measured result

| Variant | min cyc | median cyc | p90 cyc |
|---|---:|---:|---:|
| `full_curve25519_inline2` (ladder chains only) | 305,312 | 305,457 | 311,373 |
| `full_curve25519_inline2` (ladder chains + inlined `fe_invert`) | **302,212** | **302,269** | **309,425** |

**Further improvement: −3,100 cyc on min / −3,188 on median / −1,948 on p90 ≈ 1.0%.**

Combined with §3, **total saving vs baseline: −13,336 cyc min (4.22%) / −13,507 median (4.28%)**.

---

## 5. The vmwrite dispatch-cost probe

### 5.1. Why a probe

By §4 the gap to `amd64-64` had shrunk from 40k to 30k cyc. To decide whether further fusion (cramming more work into a single microcode call) could close it, the per-call cost of `vmwrite`/`vmread` had to be measured directly — not estimated from the X25519 macro number.

### 5.2. Setup

`probe_vmwrite_cost.c`:
1. Install a 1-triad patch hooked to `vmwrite`. The triad does a single `ZEROEXT_DSZ64_DR(RDI, RDI)` (effectively a no-op `mov rdi, rdi` inside the microcode sequencer). Bare-NOP triads have been reported to wedge the sequencer; this is the minimum safe payload.
2. Time `N = 100,000` back-to-back `vmwrite` calls in a tight loop, no other work inside.
3. Compare against three baselines: native `INC reg` (real serial dep), native `mov r32,r32` (eliminated by renamer), empty loop.

### 5.3. Results

| signal | per-call |
|---|---:|
| `vmwrite` + 1-triad patch | 8.97 cyc |
| native `INC reg` (real serial-dep op) | 1.55 cyc |
| native `MOV r32,r32` (eliminated) | 1.00 cyc |
| empty-loop floor | 1.00 cyc |
| **`vmwrite` dispatch overhead (per call, isolated)** | **≈ 7 cyc** |

The 1 triad itself runs at 1 cyc/triad inside the patch, so 8.97 − 1 ≈ **7 cyc per vmwrite of pure dispatch overhead**.

### 5.4. What this number tells us

Per X25519 we issue ≈ 2,560 microcode calls. Total dispatch cost = 2,560 × 7 = **~18k cyc**. The remaining gap to `amd64-64` is ≈ 30k cyc. So dispatch alone accounts for ~60 % of the gap; the other ~12k comes from frontend serialization between vmwrite and surrounding ops (the OoO frontend drains on `vmwrite`, blocking the next op's setup loads from overlapping), and from mandatory memory traffic for operand/result transport.

The forecast for any fusion strategy is bounded by `(saved calls) × ~12 cyc` (where 12 cyc covers dispatch + the local frontend-drain penalty). For SQ+MUL fusion at step 13→14 alone: 255 × 12 = ~3k cyc. Nowhere near enough to recover the 30k gap without dropping existing patches — and that's the move that turns out to lose more in `fe_invert` than it saves in the ladder (§2.1).

---

## 6. Final results

Same machine, same compile flags (`-O3 -march=x86-64-v2 -mtune=generic`), 100 reps per binary, `taskset -c 0`:

| Variant | min cyc | median cyc | p90 cyc |
|---|---:|---:|---:|
| `full_curve25519_inline` (baseline)  | 315,548 | 315,776 | 321,623 |
| `full_curve25519_inline2` — ladder chains (5 only) | 307,560 | 307,772 | 318,227 |
| `full_curve25519_inline2` — ladder chains (7 + reorder) | 305,312 | 305,457 | 311,373 |
| **`full_curve25519_inline2` — + inlined fe_invert SQ-chains (final)** | **302,212** | **302,269** | **309,425** |
| `amd64-64/asm` reference | ~272,000 | — | — |

**Gap closure: 40k cyc → 30k cyc. ~25 % of the original gap closed today.**

### 6.1. Where the remaining 30k cyc lives

| Component | Estimated cyc | Source |
|---|---:|---|
| `vmwrite`/`vmread` dispatch (2,560 calls × ~7 cyc) | ~18k | Measured (§5.3) |
| Frontend serialization + mandatory load/store traffic per call | ~12k | Subtracted residual |
| **Total structural gap** | **~30k** | Matches measurement |

This is bounded below by the call count and the 128-triad RAM cap. Reducing call count via fusion runs into the cap (§2.1).

---

## 7. Candidates considered for further work

| | Effort | Forecast | Verdict / status |
|---|---|---:|---|
| Phase A fusion (compute AA, BB in one patch) | ~10 h | net **−3k cyc** | Rejected: invert pure-sq tax wipes the saving |
| SQ+MUL fusion at step 13→14 | ~6 h | net **−10k cyc** | Rejected: invert pure-mul tax wipes the saving |
| B1 fused add+sq replacing fe_sq | ~4 h | net **−2k cyc** | Rejected: ADD is already inline, no call to fuse |
| Bernstein–Yang inversion | 3–4 days | +3–8k cyc | Deferred; see §8 |

### 7.1. The 128-triad RAM cap

Current patch RAM usage: `fe_mul` 66 triads + `fe_sq` 42 triads = 108 of 128. Staging area at U7de0–U7df0 is reserved, so effective usable ≈ 120 triads. **Free: ~12 triads.** Any new substantive patch must displace one of the existing two — which (per §7) costs more in `fe_invert` than it saves in the ladder.

---

## 8. Bernstein–Yang inversion: scope sketch (deferred decision)

### Algorithm

Replace `z^(p-2)` (Fermat) with a constant-time GCD-based inverse via "divsteps". Maintain `(δ, f, g, d, e)` where `f, g ≤ p` are integers and `(d, e)` track the inverse mod `p`. Each divstep updates the tuple via a 2×2 matrix derived from `sign(δ)` and `g`'s LSB. **Key optimisation:** do 62 divsteps on 62-bit truncations of `(f, g)`, accumulating the transformation matrix in native int registers, then apply that matrix to the full 256-bit `(f, g)` and `(d, e)` once per round. For our 255-bit modulus: 12 rounds × 62 divsteps = **744 divsteps** with **48 modular multiplications** total.

### Forecast on Goldmont

| Component | Estimated cyc |
|---|---:|
| 12 rounds × 62 inner-loop iters × ~30 cyc native | ~22k |
| 48 microcode muls × ~118 cyc | ~5.7k |
| Final extraction + 5×51 ↔ 4×64 conversion | ~0.7k |
| **Total BY** | **~28k** |

vs current Fermat `fe_invert` ≈ 31k cyc.

**Honest forecast: 3–8k cyc savings (1–2.5 %).** Lower than initial intuition because Goldmont native is only ~2× faster than microcode on dependency-chained inner loops — BY's "do mostly native work" advantage shrinks on this CPU.

### Implementation strategy (preferred path)

Port libsecp256k1's `modinv64.c` (~600 lines, mature, constant-time-audited). Adapt for `p = 2^255 - 19`. Wire the 48 matrix-apply multiplications to our existing microcode `fe_mul`. Add 5×51 ↔ 4×64 conversion at entry/exit.

### Verification plan

1. 10,000 random `z` vectors: check `(z · z⁻¹) mod p == 1` and BY output == Fermat output.
2. Edge cases: `z = 1, p-1, 2, (p-1)/2`.
3. **Constant-time audit with `valgrind --tool=memcheck`** marking `z` as secret-tainted — catches data-dependent branches/loads. Non-negotiable.
4. RFC 7748 vectors (must continue to pass).
5. 1M fuzz comparing inline2 vs new output.
6. Differential against `libsodium` X25519 (which uses BY in recent versions).

### Effort

≈ 22–28h, realistically **3 working days**.

### Risk

Constant-time bugs are silent. First-pass BY typically passes RFC 7748 but fails valgrind on a stray data-dependent branch. Budget 4–8h for these. There is also a real chance the savings land at the low end of 3k — i.e., a 3-day investment for ~1 % improvement.

### Suggested pre-commit check

**~1-day matrix-apply probe**: implement just the 48-mul matrix-apply loop with stubbed divsteps and random matrices; measure cycles. If `> 8k cyc`, BY is dead on Goldmont; if `< 4k cyc`, BY is a clear win. This is the right "is the cake real?" test before the 3-day commitment.

---

## 9. Files touched

- `simple/full_curve25519_inline.c` — unchanged (kept as apples-to-apples baseline).
- `simple/full_curve25519_inline2.c` — new file; register-chained ladder + inlined `fe_invert`.
- `simple/probe_vmwrite_cost.c` — new file; measures microcode dispatch overhead.

---

## 10. Reproducibility

```bash
cd simple/

# Final variant (chained ladder + inlined fe_invert)
make PROG=full_curve25519_inline2
sudo taskset -c 0 ./full_curve25519_inline2_static

# Baseline for comparison
make PROG=full_curve25519_inline
sudo taskset -c 0 ./full_curve25519_inline_static

# vmwrite dispatch cost probe
make PROG=probe_vmwrite_cost
sudo taskset -c 0 ./probe_vmwrite_cost_static
```

Each binary reports RFC 7748 verification (where applicable) and (min / median / p90) cycle counts over 100 X25519 evaluations.

---

## 11. Summary

- **Banked: 4.2 % improvement** on the inline-asm 5×51 X25519, with all RFC 7748 vectors verified (including the 1000-iteration consistency test).
- **Gap to `amd64-64` closed by ~25 %**: 40k → 30k cyc.
- **Empirical microcode dispatch cost: ~7 cyc/call** (measured directly, not estimated).
- **Why we stopped here**: the remaining 30k gap is dominated by mandatory dispatch + frontend serialization on the 2,560 microcode calls per X25519. The 128-triad patch-RAM cap prevents reducing the call count via fusion without paying it back (and then some) in `fe_invert`'s pure sqs and muls.
- **The only remaining lever for X25519 on Goldmont** is replacing Fermat with Bernstein–Yang inversion (scoped in §8). Forecast: +1–2.5 % more, at ~3 days of work, with the strong recommendation to run a ~1-day matrix-apply probe first to confirm the savings ceiling on this specific hardware before committing.
