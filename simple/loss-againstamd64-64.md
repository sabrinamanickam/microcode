# Why our microcode X25519 loses to amd64-64

**Conclusion.** Our microcode X25519 runs in 312k cycles on Goldmont N3350.
SUPERCOP's `amd64-64` runs the same computation in 272k cycles — a 40k
cycle (15%) gap. We characterized the gap, found the architectural root
cause, and empirically verified that the obvious workaround makes things
strictly worse. The gap is **structural** to Goldmont's microcode
flag-routing, not a tooling weakness we can engineer around.

This document walks the entire investigation, including dead ends, with
the empirical data that supports each step.

---

## 1. The `adc` instruction is amd64-64's key primitive

`amd64-64` represents a curve25519 field element as four saturated
64-bit limbs and chains additions across limbs using x86's hardware
add-with-carry:

```asm
add  rax, [b]        ; lo:  sets CF
adc  rcx, [b+8]      ;      reads previous CF, adds, sets new CF
adc  rdx, [b+16]
adc  r8,  [b+24]
```

Each `adc` is one instruction (~1 cycle on Goldmont). The full carry
chain across 4 limbs is ~4 cycles. Inside `amd64-64`'s inlined
`ladderstep.S`, the `adc` pattern fires **321 times per ladder step**
(counted with `objdump -dM intel`). That single primitive carries most
of amd64-64's whole-stack performance.

---

## 2. The microcode equivalent: SETCC dance

Goldmont microcode does have an `ADC_DSZ64` micro-op, but its semantics
make it useless for chaining as we'll see. Our production patches
instead propagate carry via the **SETCC dance**:

```
ADD_DSZ64_DRR(TMP0, a, b)        ; TMP0 = a + b, sets TMP-domain CF for TMP0
SETCC_CONDB_DR(TMP_c, TMP0)      ; TMP_c ∈ {0,1} from TMP0's CF
ADD_DSZ64_DRR(hi, hi, TMP_c)     ; add carry as a value, not a flag
```

Three micro-ops per propagated carry, versus one hardware instruction.
Goldmont microcode has at least three separate flag registers
(established in §4 below):

- **Domain #1 (TMP-CF)** — per-TMP, written by `ADD → TMP`, read by `SETCC_CONDB_DR`.
- **Domain #2a (READAFLAGS-visible)** — a 32-bit arch RFLAGS shadow, written by `GENARITHFLAGS_R/RR`, read by `READAFLAGS_DR`.
- **Domain #2b (ADC carry-in)** — captured at hook entry from x86 RFLAGS, read by `ADC_DSZ64_*`.

The carry-bridge that would make `adc` chains work in microcode is a
write path from #1 (or #2a) into #2b. We searched and couldn't find one
— see §5.

---

## 3. The 4-month-old false-positive

`Sabrina/tests/adc_findings.md` (2026-04-17) claimed `GENARITHFLAGS_R(TMP)`
was the bridge, based on a 3-triad 128-bit-add that passed two probes:

| a (lo)    | b (lo) | sum (lo) | true CF | result hi |
|-----------|--------|----------|---------|-----------|
| 1         | 1      | 2        | 0       | 0 ✓       |
| 0xFFFF…FF | 1      | **0**    | 1       | 1 ✓       |

Both passes coincide on the canonical `0xFF…F + 1 = 0` case: overflow
*and* low half wrapping exactly to zero. Two hypotheses survive both
probes:

- **H1:** `GENARITHFLAGS_R(TMP)` writes arch CF from TMP's domain-#1 CF (a real bridge).
- **H2:** `GENARITHFLAGS_R(TMP)` writes arch CF from `TMP == 0` (not a CF bridge).

The original probe set couldn't tell them apart. We added the missing
probes in `test_genarithflags_semantics.c`:

| a (R9)        | b (R10)       | TMP0 (sum) | true CF | got RAX | discriminates |
|---------------|---------------|------------|---------|---------|---------------|
| 1             | 1             | 2          | 0       | 0       | both H1/H2    |
| 0xFFFF…FF     | 1             | 0          | 1       | 1       | both H1/H2    |
| **0xFFFF…FE** | **3**         | **1**      | **1**   | **0**   | **H1 fails**  |
| **0xFFFF…FE** | **5**         | **3**      | **1**   | **0**   | **H1 fails**  |
| **0xDEAD…**   | **0xCAFE…**   | **0xA9AC…** | **1**  | **0**   | **H1 fails**  |
| 0x8000…       | 0x8000…       | 0          | 1       | 1       | both H1/H2    |
| **0**         | **0**         | **0**      | **0**   | **0**   | **H2 fails**  |

Three rows have `CF=1, TMP≠0` (rules out H1). One row has `TMP=0, CF=0`
returning 0 (rules out H2). Neither hypothesis is right. Subsequent
`readaflags_probe.c` showed `GENARITHFLAGS_R` actually writes a believable
CF into Domain #2a — but that register is *not* what ADC reads.

---

## 4. The three-domain flag architecture (empirically established)

`does_add_update_eflags.c` and `readaflags_probe.c` together produced
clean data showing **three separate flag registers**:

| | Domain #1 (TMP-CF) | Domain #2a (READAFLAGS) | Domain #2b (ADC carry-in) |
|---|---|---|---|
| Written by | `ADD → TMP_x` | `GENARITHFLAGS_R/RR` | x86 `popfq` before `vmwrite` (one-shot at dispatch) |
| Read by | `SETCC_CONDB_DR` | `READAFLAGS_DR` | `ADC_DSZ64_*` |
| Lifetime | per TMP, persists across triads | patch lifetime | dispatch-frozen for whole patch |
| ADD writes it? | **yes** | no | no |

Key evidence:
- `simple_ADC_r64.c` confirmed ADC reads the dispatch-time entry CF correctly when the wrapper sets it via `popfq`. ADC is functional one-shot.
- `intra-triad-adc.c` ran ADD-then-ADC in the same triad. Slot-0 ADD's CF-out did not reach slot-1 ADC. Both ADCs in a triad read the same frozen entry CF.
- `does_add_update_eflags.c` did ADD-overflow followed by `READAFLAGS`. The arch flags `READAFLAGS` returned were unchanged from entry. **ADD does not touch *any* arch flag register**; it stays inside Domain #1.
- `readaflags_probe.c` P4 (ADD ; `GENARITHFLAGS_R` ; `READAFLAGS`) returned flags `0x57`: CF=1, ZF=1, AF=1, PF=1. So `GENARITHFLAGS_R` *does* write a faithful CF into Domain #2a — but the prior ADC-side tests prove ADC ignores Domain #2a entirely.

---

## 5. Every candidate write-path into Domain #2b failed

We searched the visible opcode space for any micro-op that writes the
register ADC reads from. None work.

- **`GENARITHFLAGS_R/RR(...)`** — writes Domain #2a (visible to `READAFLAGS`), not #2b. ADC keeps reading the frozen entry CF.
- **`MOVEINSERTFLGS_DSZ64_DRR`, `MOVEMERGEFLGS_DSZ64_DRR`** — `test_adc_chain.c` Q4: same broken pattern as `GENARITHFLAGS_R`; ADC unaffected.
- **`MOVETOCREG_DSZ64_RI(reg, CORE_CR_EFLAGS)`** — `test_movetocreg_eflags.c`: ADC unaffected for all overflow inputs.
- **`MOVETOCREG_OR/AND/BTS/BTR(reg, CORE_CR_EFLAGS)`** — `cr_cf_probe.c`: ALL RMW variants leave Domain #2b unchanged. Bit-level write to `CORE_CR_EFLAGS` has no effect on ADC's read.
- **`MOVETOCREG_*` to nearby CR IDs** (0x7fd, 0x7ff, 0x7fc, etc.) — same null result.

The Domain #2b register is loaded **exclusively** at hook entry from
x86 RFLAGS. No micro-op in our search updates it. That makes ADC a
strict one-shot primitive: useful if x86 code computes a single carry
with `add`/`adc`/`stc`/`clc` before `vmwrite`, then a *single* ADC in
the patch consumes it. Chains of two or more ADCs all read the same
frozen entry value.

---

## 6. Building 4×64 microcode anyway: empirically a 3.7× loss

The natural workaround is to switch our representation from 5×51
unsaturated to 4×64 saturated, matching amd64-64 algorithmically and
absorbing the SETCC-dance overhead per limb. We did this for `fe_mul`
in `asm_op_curve25519_solinas_mul.c` — a 119-triad Solinas-style
4×64 multiplication patch that passes 11008/11008 vector tests
(known + 10k random + 1k chain + commutativity).

Per-call timing on the same Goldmont N3350:

| `fe_mul` implementation | min cyc/op | notes |
|---|---:|---|
| Our 5×51 microcode | **~58** | Dettman unsaturated, the production patch |
| Native `-O3` 4×64 C | 132 | GCC schoolbook |
| Fiat-crypto 4×64 | 224 | reference verified-correct baseline |
| **4×64 Solinas microcode** | **215** | this representation in our microcode framework |

**4×64 microcode is 3.7× slower per call than our 5×51 microcode.**
Projecting to a full X25519 ladder using 4×64 microcode for both fe_mul
and fe_sq (a single Solinas patch is 119 triads and a second wouldn't
fit alongside, so `sq` would have to be `mul(a, a)`):

- `9 fe_mul × 215 cyc × 255 steps ≈ 493k cyc` field-ops portion
- + invert chain (~265 ops × 215 ≈ 57k)
- + framework/cswap (~170k)
- **≈ 720k cyc total**

For comparison: current ours/ucode = 312k cyc, amd64-64 = 272k cyc.
Switching us to 4×64 microcode makes the gap **wider by 2.3×**, not
narrower.

The mechanism is exactly the SETCC-dance overhead. The 119-triad
Solinas patch is roughly:
- ~88 triads of schoolbook (16 muls + lo/hi accumulation)
- ~30+ triads of carry-propagation chains via SETCC dance

In amd64-64, those 16 carries become 16 hardware `adc` instructions at
~1 cyc each. In microcode they become 16 × 3-op SETCC dances ≈ 48 ops
≈ 16+ triads sitting on the critical accumulator dependency chain,
which OoO cannot reorder around. The 9 saved multiplications (16
schoolbook vs 25 for 5×51 Dettman) save roughly 45 cycles. The added
carry-propagation cost adds *more* than that back. Net: 4×64 microcode
is a loss.

The unsaturated 5×51 representation amortizes carry tracking across
fewer, shorter dependency chains per c-section, which is why it wins
in this microcode environment. Hardware ADC inverts the trade-off; our
microcode lives on the wrong side of the inversion.

---

## 7. Other workarounds we considered

Each of these was investigated; none change the bottom line.

- **Staging buffer.** `project_stgbuf.md`: STADSTGBUF stores are ~free, but LDSTGBUF loads are ~30 cyc each and do not pipeline. For any state used on the hot path, plain memory I/O dominates because Goldmont's OoO+STLF overlaps it with the microcode patch body. Stgbuf is only useful for write-once / never-read-back state, which the ladder doesn't have.

- **In-patch looping.** Already used: `fe_sq_ucode_n` keeps a squaring chain in registers across iterations and saves ~12.7 cyc/iter, exploited in the inversion chain. Looping the whole ladder body inside one patch needs 200+ triads and busts the 124-triad RAM cap.

- **Combining ops in one `vmwrite` firing (sq-pair, mul-pair).** Would save ~27 cyc × 2 pairs/step × 255 steps ≈ 14k cyc per pair-fusion type. Two-iteration sq-pair needs 55 triads minimum (see `feedback_sq_pair_too_big.md`) and the cap is 54 with the current fe_mul size. We tried trimming fe_mul by 1 triad to make room: the trim regressed perf by 5k cyc because the TMP9 carry chain is load-bearing for ILP (`feedback_fe_mul_tmp9_chain.md`). Mul-pair would need ~132 triads, hopeless.

- **Inlined ladderstep.** A single huge patch firing the entire ladder body would eliminate per-call mode-switch tax (estimated 14-27 cyc × ~9 firings × 255 steps ≈ 30-60k cyc). It needs 200+ triads. Won't fit.

- **Cheaper hook instruction.** Currently `vmwrite`/`vmread`. Plausibly some other hookable opcode (cpuid, rdrand, syscall, ...) dispatches faster, saving a slice of the mode-switch tax across every firing. Worth a microbench if someone is curious, but the upside is bounded by mode-switch component (~10-15k cyc).

---

## 8. What we did accomplish

Microcode is a real win against the best hand-tuned **same-radix** x86 assembly. With 5×51 Dettman field arithmetic:

| Implementation | min cyc/X25519 | vs ours |
|---|---:|---:|
| ours/hand-C (5×51, `__uint128_t`, GCC -O3) | 389,496 | 1.25× slower |
| ours/fiat-crypto (5×51, autogen verified C) | 357,946 | 1.15× slower |
| **amd64-51/asm** (5×51 hand-tuned x86 asm, Bernstein/Schwabe) | **355,982** | **1.14× slower** |
| **ours/ucode** (5×51 microcode) | **312,260** | **1.00×** |
| amd64-64/asm (4×64 saturated x86 asm with hardware ADC) | 272,128 | 0.87× — wins by 15% |

Numbers from `simple/results.md` at pinned 1.094 GHz.

The microcode beats SUPERCOP's 5×51 hand-tuned x86 assembly by **14%**
in an apples-to-apples comparison (same algorithm, same representation,
same prime). It also beats fiat-crypto by 14% and naive `__uint128_t` C
by 25%. The 15% deficit only appears against the 4×64-saturated
amd64-64, which uses a representation that microcode cannot match
because of the missing ADC bridge.

---

## 9. Honest framing for the paper

The framing this evidence supports:

> **Microcode beats hand-tuned same-radix assembly by 14%.** The
> further 15% to `amd64-64` comes from `amd64-64`'s use of hardware
> `adc` to chain carries across saturated 64-bit limbs. The
> `ADC_DSZ64` micro-op exists on Goldmont but is structurally
> one-shot: its carry-in register is captured at hook entry from x86
> RFLAGS and is then frozen — every probed write path into it
> (GENARITHFLAGS_R/RR, MOVEINSERTFLGS, MOVEMERGEFLGS, all six
> MOVETOCREG variants to CORE_CR_EFLAGS) fails to update what the
> next ADC reads. Without a chainable in-patch ADC, the saturated
> 4×64 representation is forced to use the 3-op SETCC-dance per
> carry, and the resulting overhead more than outweighs the
> mul-count win. We verified this empirically: a 4×64 Solinas
> microcode `fe_mul` runs at 215 cyc/op vs our 5×51 microcode at
> ~58 cyc/op, a 3.7× regression that projects to ~2.3× worse than
> amd64-64 at the full X25519 level. The gap is not a microcode-tooling
> failure but a microcode-architecture property: Goldmont decodes `adc`
> before microcode dispatch and never exposes the flag-routing valve
> that hardware `adc` uses.

That is a concrete, defensible architectural finding — the limitation
is a *specific named structural feature* of the Goldmont microcode
flag-domain split, not a vague "primitives don't work."

---

## 10. Artifacts

Tests (all in `Sabrina/tests/` unless noted):

- `test_genarithflags_semantics.c` — disproved the GENARITHFLAGS_R bridge hypothesis with the right discriminator probes (§3).
- `test_movetocreg_eflags.c` — confirmed `MOVETOCREG → CORE_CR_EFLAGS` doesn't update ADC's carry-in (§5).
- `cr_cf_probe.c` — confirmed all six MOVETOCREG variants (plain, OR, AND, BTS, BTR) leave Domain #2b unchanged (§5).
- `test_adc_chain.c` — broader probe of chain attempts and MOVEINSERTFLGS/MOVEMERGEFLGS (§5).
- `simple_ADC.c`, `simple_ADC_tmp.c`, `simple_ADC_r64.c` — confirmed ADC reads dispatch-time arch CF correctly when properly set (§4).
- `intra-triad-adc.c` — confirmed ADD's CF-out doesn't reach slot-1 ADC even in the same triad (§4).
- `does_add_update_eflags.c` — confirmed ADD does not touch any arch flag register (§4).
- `readaflags_probe.c` — established the three-domain architecture by triangulating what each flag-related op writes and what `READAFLAGS` reads (§4).
- `adc_findings.md` — the prior (now-superseded) 2026-04-17 investigation.

Production microcode and benchmark:

- `simple/full_curve25519.c` — full X25519 with 5×51 microcode field ops (the 312k figure).
- `simple/asm_op_curve25519_solinas_mul.c` — standalone 4×64 Solinas `fe_mul` microcode patch (119 triads, 215 cyc/op).
- `simple/asm_op_curve25519_solinas_sq.c` — standalone 4×64 Solinas `fe_sq` (does not coexist with the mul patch under the 128-triad RAM cap).
- `simple/results.md` — measured cycle counts at pinned 1.094 GHz governor.

---

## 11. 2026-05-22 update: CF bridge discovered, but doesn't close the gap

§3 and §9 above concluded that no write path into Domain #2b
(ADC's carry-in) exists. **That conclusion was incomplete.** An
exhaustive 24-arrangement probe (`tests/genflagsrr_exhaustive.c`)
turned up one combination that does write the carry ADC reads:

```
ADD/ADC TMP = src1 + src2          ; sets TMP-CF for TMP
GENARITHFLAGS_RR(TMP, TMP)         ; SAME TMP twice — bridges TMP-CF → arch CF
ADC TMP_next = src3 + src4         ; reads arch CF, writes new TMP-CF
GENARITHFLAGS_RR(TMP_next, TMP_next)
...
```

The `RR` form with both operands the same TMP appears to extract the
destination TMP's own TMP-CF and publish it as arch CF. This was missed
in the earlier probes which used distinct operands or arch registers.

**Verified scope** (`tests/chain_length.c`, 2026-05-22): chains of this
form scale cleanly to N=32 limbs (12/12 probes across N=4, 8, 16, 32).
Per-limb cost is ~8 cyc amortized (`tests/chained_4limb_add.c`).

**Important caveat — TMP destinations only** (`tests/arch_gfl_chain.c`,
2026-05-22): the same chain pattern with arch-register destinations
silently drops the carry on overflow. `GENARITHFLAGS_RR(arch, arch)`
does not bridge the prior ADD/ADC's TMP-CF — it either re-runs the add
or leaves arch CF unchanged. So the primitive works, but every
destination in the chain must be a TMP register.

### Why §9's bottom line still holds

A 4×64 Solinas `fe_mul` rewritten to use chained ADC throughout
(`simple/asm_op_curve25519_solinas_mul_chained.c`, 113 triads,
verified across 10,007 random pairs) measures **229 cyc/op** — actually
*slightly slower* than the 215-cyc SETCC-dance version of the same
algorithm. The per-carry op-count savings (chained ADC: 2 ops/carry
vs. SETCC dance: 3 ops/carry) are eaten by:

- The `GENARITHFLAGS_RR` latency itself counts as an op in the chain.
- TMP destinations require ZEROEXT to arch at chain end (the
  accumulator state lives in arch regs across rows for the SHIFT
  structure), adding 2 triads per row.

The 4×64 representation still loses to production 5×51 Dettman
`fe_mul` (58 cyc/op) by ~4×, and 5×51's carry chains use SHR/AND not
ADC — the new primitive doesn't apply there. The 40k-cyc X25519 gap
to `amd64-64` is unchanged by this discovery.

The chained-ADC primitive *is* useful in absolute terms — for cheap
4-limb operations (`fe_add` / `fe_sub` / `sub_to_zero` in a saturated
representation), it gives clean per-call costs around 30-40 cyc once
the patch-dispatch floor is paid. But in `curve25519` specifically,
those ops are already native C in the 5×51 representation and aren't
on the critical path.

### Revised framing

The right framing is now:

> Goldmont microcode does expose one in-patch CF bridge —
> `GENARITHFLAGS_RR(TMP, TMP)`, which publishes a TMP's CF to arch
> CF and enables chained `ADC_DSZ64`. The primitive works only when
> chain destinations are TMP registers; arch destinations silently
> leak. We tested a 4×64 Solinas `fe_mul` using this primitive
> (`asm_op_curve25519_solinas_mul_chained.c`, 113 triads, 10,007/10,007
> verified): 229 cyc/op, slightly slower than the SETCC-dance form
> (215) and 4× slower than our production 5×51 Dettman (58). The
> primitive is not the bottleneck and the 40k-cyc `amd64-64` gap
> remains structural.

Artifacts added in this round:

- `tests/genflagsrr_exhaustive.c` — 24-arrangement probe that found the bridge.
- `tests/genflagsrr_bridge_verify.c` — confirmed bridge across 8 input cases.
- `tests/chain_length.c` — verified N=4, 8, 16, 32 chains scale cleanly.
- `tests/chained_4limb_add.c` — measured ~8 cyc/limb amortized.
- `tests/arch_gfl_chain.c` — documented the TMP-only scope.
- `simple/asm_op_curve25519_solinas_mul_chained.c` — full chained-ADC `fe_mul` (113 triads, 229 cyc/op).

## 12. End-to-end X25519 measurement (2026-05-22, follow-up session)

Acting on the HANDOFF_full_curve25519_64.md plan, the chained-ADC `fe_mul`
was iterated and an end-to-end 4×64 X25519 implementation was built and
benchmarked. Empirical conclusion: **the 4×64 microcode form cannot close
the gap to amd64-64 on X25519.**

### Field-op shrinks delivered

| version | triads | cyc/op | technique |
|---------|--------|--------|-----------|
| v1 (prior session)     | 113 | 229 | first cut chained-ADC |
| v2 — packed MUL block + merged writeback/SHIFT | 95 | 209 | uses confirmed slot-1/2 MUL + intra-triad RAW/WAR |
| **v3 — intra-triad chain packing** | **75** | **200** | newly verified `{ADC, GFL_RR, ADC}` in one triad bridges CF |

The intra-triad CF bridge was tested in
`tests/test_adc_gfl_adc_intratriad.c` (2026-05-22) and works for the
same-TMP-twice GFL_RR pattern — slot 2's ADC reads the arch CF set by
slot 1's GFL_RR. This shrinks chain triads ~33% and is the main lever
that took fe_mul from 95 → 75 triads.

### End-to-end measurement

`simple/full_curve25519_4x64.c` — full X25519 using v3 fe_mul, fe_sq via
`fe_mul(a, a)` wrapper, and native-C 4×64 cheap ops (fe_add / fe_sub /
fe_mul121665 matching SUPERCOP amd64-64's lazy-reduction pattern).

RFC 7748 verification: 4 / 4 passed (both standard vectors plus the
1-iteration and 1000-iteration self-tests; the 1000-iter output matches
`684cf59b…32c51` exactly).

Bench (1.094 GHz pinned, `sudo taskset -c 0`, 100 reps):

```
min:    604,532 cyc
median: 610,675 cyc
p90:    615,339 cyc
```

For reference:
- 5×51 microcode (production): **312,000 cyc**
- SUPERCOP amd64-64 (target):  **272,000 cyc**

The 4×64 chained-ADC microcode X25519 is **1.94× slower than 5×51
production** and **2.22× slower than amd64-64**.

### Why the handoff's arithmetic was off

The HANDOFF_full_curve25519_64.md plan §0 claimed a ~25k-cyc field-op
regression vs 5×51, leaving ~15k headroom inside the 40k-cyc
amd64-64 gap, with §6 listing 35–70k cyc of recoverable savings from
fusion + inlining + STLF elimination. That arithmetic doesn't survive
contact with the actual call count.

The handoff's "~255 fe_sq + ~250 fe_mul + ~1000 fe_add/sub-class ops"
count under-states the ladder by ~5×. The actual per-X25519 cost is:
- Ladder body: 5 fe_mul + 4 fe_sq + 8 fe_add/sub per iteration × 255 iterations
  = 1275 fe_mul + 1020 fe_sq + 2040 add/sub
- `fe_invert`:  ~12 fe_mul + 250 fe_sq (Fermat addition chain)
- Per X25519 totals: **~1287 fe_mul + ~1270 fe_sq + ~2300 cheap ops.**

Even with v3's 200-cyc fe_mul, 1287+1270 = 2557 field-mul/sq ops × 200 cyc
= **511k cyc just on field ops**, before any cheap-op or invert overhead.
A symmetric fe_sq at the handoff's ~150-cyc target would cut roughly
65k from that, leaving the field-op subtotal at ~440k — still 1.6× over
amd64-64's total budget. Fusion of cheap ops would shave at most another
30k. There is no plausible combination of recovery levers that bridges
a 300k cyc structural gap.

### Why the structural gap is so large

Per call, 4×64 microcode and 5×51 microcode look roughly comparable
(200 vs 127 cyc for mul; ~200 vs ~80 for sq via wrapper). The order-of-
magnitude regression comes from the *number* of calls × per-call cost.
SUPERCOP amd64-64 wins by inlining the entire ladderstep into one
register-resident asm block — no per-call dispatch overhead at all.
The 5×51 microcode form pays per-call dispatch but uses a 51-bit limb
that lets each fe_mul finish in 58 cyc standalone. The 4×64 microcode
form pays the same per-call dispatch but each fe_mul costs more than
3× as much.

### Strategic conclusion

The CF bridge primitive (`GENARITHFLAGS_RR(TMP, TMP)`) and its newly-
confirmed intra-triad packing are real, useful tools — they make the
chained-ADC pattern viable for 4×64 microcode at competitive per-op
cycle counts. But they don't change the underlying arithmetic: at ~2557
field-op calls per X25519, even a best-case 4×64 microcode fe_mul/fe_sq
stays decisively behind amd64-64's inlined ladderstep.

The honest framing for the paper is the same as before — Goldmont
microcode offers competitive 5×51 fe_mul/fe_sq primitives but cannot
match amd64-64's whole-ladder inlining for X25519 specifically. The
4×64 form's structural appeal (saturated limbs, ADC primitive) is
real, but it doesn't translate to a competitive end-to-end X25519
implementation on this microarchitecture.

### Artifacts from this round

- `tests/test_adc_gfl_adc_intratriad.c` — proves intra-triad
  `{ADC, GFL_RR(TMP,TMP), ADC}` bridges CF (2026-05-22).
- `simple/asm_op_curve25519_solinas_mul_chained_v2.c` — 95 triads,
  209 cyc/op (MUL-block + writeback-shift packing).
- `simple/asm_op_curve25519_solinas_mul_chained_v3.c` — **75 triads,
  200 cyc/op** (adds intra-triad chain packing).
- `simple/full_curve25519_4x64.c` — end-to-end X25519 using v3
  microcode + native-C 4×64 cheap ops. 4/4 RFC 7748 verified.
  Bench: **604k cyc/op** (min) vs amd64-64's 272k.

## 13. Returning to 5×51: inline-asm rewrite is also a dead end (2026-05-22, same session)

After §12 established that 4×64 microcode can't close the gap from the
"more ADC-friendly form" direction, the natural next question was whether
the 5×51 *production* form's 40k cyc deficit to amd64-64 could be closed
by eliminating per-call wrapper overhead (each fe_mul/fe_sq in the C
ladder pays a wrapper around the `vmwrite`/`vmread` patch trigger).

### The probe

`simple/bench_fe_mul_wrapper.c` (2026-05-22) — measured three patterns
for chained fe_mul:

| pattern | min cyc/op |
|---|---:|
| A. standalone tight loop (constant inputs) | 125 |
| B. C-wrapper chained (`out = out*b`, mem dep) | 130 |
| C. inline-asm chained (data in regs across calls) | 120 |

The `B − C = 10 cyc/op` looked like recoverable wrapper cost.
**This was a misinterpretation.**

### The rewrite

`simple/full_curve25519_inline.c` — 5×51 X25519 with the entire ladder
body (4 fe_sq + 5 fe_mul + 6 fe_add/sub + 2 cswaps) packed into one
giant inline-asm block per iteration. `ladder_state_t` struct on stack,
field elements accessed via `[rbp + N]` offsets. 4/4 RFC 7748 vectors
verified.

Bench (100 reps, taskset -c 0):
```
min:    312786 cyc
median: 312865 cyc
```

**Identical to production's 312k cyc.** No measurable savings.

### Why the probe's 10 cyc/op didn't transfer

Three reasons, all empirically verified by the null result:

1. **The probe was chained — the ladder isn't.** The probe's "savings"
   came from reusing the patch's output registers as the next call's
   input registers (no memory at all between calls). The X25519 ladder's
   fe_mul calls take *independent* inputs from various stack-resident
   field elements; reg-reuse savings don't apply.

2. **GCC already inlines at -O3.** `fe_mul_ucode`/`fe_sq_ucode` are
   file-static in `full_curve25519.c`; with `-O3` and the function
   defined in the same translation unit, GCC inlines them at call
   sites. The C-call overhead I thought I was eliminating was already
   eliminated by the compiler.

3. **OoO + STLF really does hide the memory roundtrip.** The original
   `feedback_dispatch_overhead.md` memory entry was correct. The per-op
   load/store pattern is not on the critical path.

### Conclusion: the 40k gap is structural

Per-fe_mul timing analysis:
- 5×51 microcode fe_mul: 66 triads × ~1 cyc/triad ≈ **~125 cyc/op**
- amd64-64 native fe_mul: ~200 instructions × 2 IPC ≈ **~80 cyc/op**
- Per-call gap: ~30–45 cyc
- Across 2557 X25519 field ops: ~75–115k cyc (raw)

The observed 40k gap is *less* than the raw structural gap because
microcode wins back ground via SETCC-dance's parallel TMP-flag chains
(which native amd64-64 can't replicate efficiently). But the residual
40k *is* the unavoidable cost of microcode dispatch being slower per
op than native code on Goldmont.

**There is no software optimization that closes this gap by attacking
the wrapper or call overhead.** The only remaining levers are:

1. **Bernstein-Yang inverse** instead of Fermat. Saves ~10-15k cyc on
   `fe_invert`. Multi-day implementation (must be written for Goldmont
   since lib25519's amd64-safegcd needs AVX2). Even at the high end,
   doesn't fully close 40k.
2. **Patch micro-optimization** (shave 2-4 triads from fe_mul/fe_sq):
   ~5k cyc.
3. **Field representation change** (e.g., 4×56 or other unsaturated):
   speculative multi-week project.

### Honest framing for the paper (revised)

> Goldmont microcode produces a fe_mul/fe_sq at competitive
> per-operation cycle counts (5×51 fe_mul at 58 cyc standalone, 125 cyc
> in-ladder), and end-to-end X25519 lands at 312k cyc — 15% slower than
> SUPERCOP `amd64-64`'s 272k. The gap is structural: microcode patches
> issue at ~1 cyc/triad while the OoO frontend issues native ~2 ops/cyc,
> giving native a ~30-45 cyc edge per field operation × 2557 ops per
> X25519. Wrapper overhead and patch dispatch are not the bottleneck
> (empirically verified by a bench probe and a full inline-asm rewrite
> that produced 0 savings). The chained-ADC primitive
> (`GENARITHFLAGS_RR(TMP,TMP)` + intra-triad packing) is a real and
> reusable microcode primitive but does not by itself close the
> structural gap on this microarchitecture.

### Artifacts from this round

- `tests/test_adc_gfl_adc_intratriad.c` — intra-triad CF bridge proof.
- `simple/asm_op_curve25519_solinas_mul_chained_v2.c` — 95 triads, 209 cyc.
- `simple/asm_op_curve25519_solinas_mul_chained_v3.c` — 75 triads, 200 cyc.
- `simple/full_curve25519_4x64.c` — 4×64 X25519, 604k cyc, 4/4 RFC.
- `simple/bench_fe_mul_wrapper.c` — wrapper-overhead probe (A/B/C patterns).
- `simple/full_curve25519_inline.c` — inline-asm 5×51 ladder, 312k cyc, 4/4 RFC.
