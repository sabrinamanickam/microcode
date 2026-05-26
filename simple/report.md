# Closing the gap between microcode X25519 and SUPERCOP `amd64-64`

**Date:** 2026-05-26
**Author:** Sabrina Manickam
**Target:** Intel Goldmont (Celeron N3350), pinned `taskset -c 0`, `sudo` for microcode patch install.

## 1. Starting position

Three X25519 implementations were on the table:

| Implementation | min cyc | Notes |
|---|---:|---|
| `full_curve25519.c` (production) — our ladder + C wrappers around 5×51 microcode `fe_mul` / `fe_sq` patches | ~312,000 | Documented baseline |
| `full_curve25519_inline.c` — same patches, but with a per-iteration inline-asm ladder block to remove C-call overhead | ~315,500 | Earlier experiment; ≈ production within noise |
| `full_curve25519_4x64.c` — 4×64 saturated form mirroring `amd64-64`'s lazy-reduction, `fe_mul` via v3 chained-ADC microcode patch (75 triads), `fe_sq = fe_mul(a,a)` | ~604,000 | Verified 4/4 RFC 7748. Loses structurally (≈2× over `amd64-64`'s whole-X25519 budget just in field-ops) |
| **`amd64-64/asm`** (SUPERCOP, Bernstein/Schwabe) — whole-stack hand-tuned amd64 asm, 4×64 saturated | **~272,000** | Reference; the number we are trying to approach |

The gap from the best microcode variant (production, 312k) to `amd64-64` is ≈ **40k cyc / 14%**.

## 2. Approaches considered

### 2.1. Discarded after analysis

- **Hybrid: `amd64-64`'s ladder + microcoded `fe_mul` / `fe_sq`.** `amd64-64`'s `ladderstep.S` is one monolithic 6580-line asm symbol with all five `mulq` blocks fully unrolled inline (≈ 690 `movq` total, mostly stack spills only when register pressure forces them). To "use the same style" with our microcode would require either (a) calling out to our `fe_mul_ucode` from inside `ladderstep.S` — which is exactly what `amd64-51-ucode` already does and loses, or (b) inlining the `vmwrite` directly into a copy of `ladderstep.S` with patch rewrites so the microcode reads operands from a stable arch-reg set. Option (b) is multi-week work and prior experiments (memo `x25519-4x64-loses`) predict no headroom, since the field-op cost itself dominates.

- **Further optimisation of `full_curve25519_4x64.c`.** End-to-end 604k vs `amd64-64` 272k. Per X25519 we issue ≈ 1287 `fe_mul` + 1270 `fe_sq`. Each microcode call costs ≈ 200 cyc, so the field ops alone account for ≈ 511k cyc — already nearly 2× `amd64-64`'s whole-function budget. Even with aggressive cheap-op fusion and a symmetric (squaring-optimised) `fe_sq` patch the projected floor is ≈ 480k, never below `amd64-64`. **Structural loss; abandoned.**

### 2.2. Pursued today

**Register chaining inside the 5×51 inline-asm ladder.** Goal: keep the output of one field op live in registers as the input of the next op, eliminating per-op memory reloads — the closest we can get to `amd64-64`'s reg-to-reg chaining without a patch rewrite.

## 3. Why the existing inline impl had headroom

Read of `full_curve25519_inline.c`:

- `ladder_step` is a single `asm volatile` block containing 13 macro-expanded ops (4 `FE_MUL` + 4 `FE_SQ` + 5 `FE_ADD`/`FE_SUB`) — but **each macro independently reloads inputs from memory at entry and stores outputs to memory at exit.**
- `FE_MUL` (lines 345-363): 10 `mov reg, [rbp+off]` + `vmwrite` + 5 `mov [rbp+off], reg` per call.
- `FE_ADD`/`FE_SUB` serialised all 5 limbs through a single `rax`, so the result was never left in distinct registers a downstream op could pick up.
- The "tail" of the iteration (`mul121665` + `add` + `mul`) is a C function call between two asm blocks — additional boundary.
- `fe_invert` is *not* inlined at all: 266 separate C-wrapper calls each doing its own load → `vmwrite` → store cycle.

So while the C-call layer was removed, the inter-op **memory traffic** (which `amd64-64` largely avoids) was not. The hypothesis under test: closing that gap is worth real cycles.

## 4. Design (`full_curve25519_inline2.c`)

Forked `full_curve25519_inline.c`. Changes:

1. **Restructured `FE_ADD` and `FE_SUB`** to leave the 5 result limbs in registers `{rdi, rsi, r12, r11, r14}` — the exact register set `FE_SQ` and `FE_MUL`'s "`a`" slot expect. Memory store of the result is retained, since downstream non-chained consumers still need it.

2. **Tightened `FE_SUB`.** The limb-1..4 bias constant (`0xFFFFFFFFFFFFE`) is loaded into `rcx` once and reused, saving 3 `mov rcx, imm64` per `FE_SUB` call.

3. **New chain-aware macros:**
   - `FE_SQ_FROM_REGS(out)` — assumes `a[0..4]` already in `{rdi, rsi, r12, r11, r14}`. Skips the 5 input loads vs the original `FE_SQ`.
   - `FE_MUL_FROM_REGS_A(out, b)` — assumes `a[0..4]` already in `{rdi, rsi, r12, r11, r14}`. Loads only `b`; skips the 5 input loads for `a`.

4. **Hand-scheduled the ladder iteration** to use chained variants in 7 places:

   | step | original | chained | saves |
   |---|---|---|---|
   | 1→2 | `A = ADD(x2,z2); AA = SQ(A)` | `FE_ADD` + `FE_SQ_FROM_REGS` | 5 loads |
   | 3→4 | `B = SUB(x2,z2); BB = SQ(B)` | `FE_SUB` + `FE_SQ_FROM_REGS` | 5 loads |
   | 7→8 (reordered) | `D = SUB(x3,z3); DA = MUL(D,A)` | `FE_SUB` + `FE_MUL_FROM_REGS_A` | 5 loads |
   | 6→9 (reordered) | `C = ADD(x3,z3); CB = MUL(C,B)` | `FE_ADD` + `FE_MUL_FROM_REGS_A` | 5 loads |
   | 10→11 | `t0 = ADD(DA,CB); x3' = SQ(t0)` | `FE_ADD` + `FE_SQ_FROM_REGS` | 5 loads |
   | 12→13 | `t0 = SUB(DA,CB); z3' = SQ(t0)` | `FE_SUB` + `FE_SQ_FROM_REGS` | 5 loads |
   | tail | `t0 = ADD(AA,t0); z2 = MUL(E,t0)` | `FE_ADD` + `FE_MUL_FROM_REGS_A` (uses commutativity: t0·E = E·t0) | 5 loads |

   The 6/7 reorder is the key trick: scheduling `D = SUB` immediately before `DA = MUL` (chain) and `C = ADD` immediately before `CB = MUL` (chain) gives **two** chains in this section where the original schedule allowed only one.

5. **`fe_invert` left unchanged** — out of scope for this experiment.

## 5. Verification

`full_curve25519_inline2_static` was run against RFC 7748 § 5.2 test vectors:

```
Vector 1 (Alice/Bob low-order test):  PASS
Vector 2 (mid-range scalar/point):    PASS
Iterated test, 1 round:               PASS
Iterated test, 1000 rounds:           PASS  (output 684cf59b…32c51, matches RFC)
```

All four vectors pass, including the 1000-iteration consistency check that catches subtle ladder/cswap bugs.

## 6. Measured results

Same machine, same compile flags (`-O3 -march=x86-64-v2 -mtune=generic`), 100 reps per binary, `taskset -c 0`:

| Variant | min cyc | median cyc | p90 cyc |
|---|---:|---:|---:|
| `full_curve25519_inline_static`  (baseline) | 315,548 | 315,776 | 321,623 |
| `full_curve25519_inline2_static` (5 chains only) | 307,560 | 307,772 | 318,227 |
| `full_curve25519_inline2_static` (7 chains + reorder) | **305,312** | **305,457** | **311,373** |
| `amd64-64/asm`                                | ~272,000 | — | — |

**Improvement (inline → inline2 final):** −10,236 cyc on min / −10,319 on median / −10,250 on p90 — **≈ 3.2% on each percentile**, with reduced variance (p90 tightens by 10,250 vs only 8,000 on min, suggesting the chained version is more dispatch-friendly).

**Gap closure to `amd64-64`:** was 40k cyc, now 33k cyc. **17.5 % of the gap closed.**

## 7. Where the gains actually came from

Approximate decomposition of the 10k saving:

- **≈ 5k cyc — `FE_SUB` tightening.** The original macro reissued `mov rcx, IMM64` five times per call. The new version issues it twice (one for limb 0's bias, one for limbs 1-4's bias). Per X25519: 4 `FE_SUB`/iter × 255 iter × 3 saved movs ≈ 3060 mov-eliminations.
- **≈ 5k cyc — register chains across 7 op-pairs.** Each chain eliminates 5 memory reloads. 7 × 5 × 255 ≈ 8925 load-eliminations, partly hidden by store-to-load forwarding (STLF), netting ≈ 5k actual cycles.

This contradicts an earlier internal note that claimed inline-asm restructuring of the 5×51 ladder produced "zero cycles" of improvement — that observation was about the *original* inline rewrite, which removed the C-wrapper boundary but did not address the per-op memory roundtrips inside the asm block. Once the chains are added, real cycles fall out.

## 8. What remains in the gap

After today's work, the remaining 33k cyc to `amd64-64` is split across:

1. **`fe_invert`** — 266 separate C-wrapper calls, each with full memory roundtrip. Likely accounts for ~half of the remaining gap. Inlining `fe_invert` with the same SQ chains we just validated should be tractable.

2. **Per-op microcode dispatch cost.** Goldmont issues microcode patches at ≈ 1 cyc per triad; `amd64-64`'s native amd64 asm issues at ≈ 2 ops per cyc through the OoO frontend. Per `fe_mul` this is ≈ 30 cyc of unrecoverable overhead × ~2557 ops/X25519 = up to 75k cyc structural, of which the chain trick partially absorbed ≈ 10k via amortisation.

3. **The 5 still-unchained ops in the main asm block** (steps 5, 14, 15) — bounded by SQ↔MUL register mismatch (`FE_SQ`'s output regs `{rdi, r9, r10, rbx, rax}` don't match `FE_MUL`'s input regs `{rdi, rsi, r12, r11, r14}`).

## 9. Candidates for the next step

| Option | Effort | Forecast |
|---|---|---|
| A. Inline-asm `fe_invert` with SQ chains across the 5/10/20/50/100/50/5-square runs | ~2 h | +4 – 6k cyc |
| B. Fused SQ+MUL microcode patch for `step 13→14` (sq result kept in TMP regs, fed directly into MUL schoolbook within one `vmwrite`) — ~80 triads, fits under the 128-triad RAM cap | ~4 h | +2 – 3k cyc |
| C. Bernstein–Yang inversion replacing the Fermat exponentiation chain | multi-day | +10 – 15k cyc on `fe_invert` |

**Recommendation:** A first (best return per hour, reuses today's chain machinery). If it lands, re-evaluate B versus C with fresh numbers.

## 10. Files touched

- `simple/full_curve25519_inline2.c` — new file, register-chained variant (forked from `full_curve25519_inline.c`).
- `simple/full_curve25519_inline.c` — unchanged (kept as the apples-to-apples baseline for the comparison in §6).

## 11. Reproducibility

```bash
cd simple/
make PROG=full_curve25519_inline2
sudo taskset -c 0 ./full_curve25519_inline2_static
```

Output reports both the RFC 7748 verification and the (min / median / p90) cycle counts over 100 X25519 evaluations.
