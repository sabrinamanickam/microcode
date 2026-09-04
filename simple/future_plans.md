# Future Plans — Microcode + CryptOpt: New Directions

Brainstorm and decisions for what to build next with the microcoded-kernels
methodology, combining it with CryptOpt, and extending it to AMD CPUs.

**Context:** the existing work microcodes cryptographic field arithmetic and
Keccak on Intel Goldmont (Celeron N3350) via the red-unlock. Prior CryptOpt
experiment — supplying CryptOpt a custom `mulx` (microcode-emulated on a
non-BMI2 CPU) — did **not** speed anything up, because the **~7-cycle patch-RAM
redirect (firing) tax** swamped the single instruction's worth of work.

---

## 1. The governing principle: work-per-firing, not ops-per-firing

The firing tax is fixed at ~7 cyc dispatch (~12 cyc *effective* once frontend
drain + mandatory operand memory traffic are counted). The only question that
matters for any patch:

> **useful work `W` per firing ÷ ~12 cyc overhead → is that ratio big enough to win?**

| pattern | W per firing | overhead | verdict |
|---|---|---|---|
| single `mulx` replacement | ~3–5 cyc | 200%+ | **structurally dead** |
| one `fe_mul` (5×51) | ~40 cyc | ~30% | marginal net win |
| Keccak, 24 rounds/firing | ~1900 cyc | <0.5% | **clean win** |

**The rule:** a patch amortizes the tax iff it wraps either (a) a whole
expensive primitive with an **internal loop over rounds or data blocks**, or (b)
a **batch** of independent work. Single-ISA-instruction replacement can never
win — the granularity is wrong, and no amount of making the instruction faster
fixes it. The robust lever is *loop the data inside the firing* (like Keccak
loops rounds), not reduce the tax.

---

## 2. Where microcode actually has an edge (it is µarch-specific)

Every good project should exploit at least one of the two structural advantages
microcode has over the *specific* weak core it runs on:

1. **No ADX/BMI2 → native can't run parallel carry chains.** Microcode's
   per-TMP flag domains + SETCC recover them. This is *why* the Goldmont X25519
   field ops already beat hand-asm ~7%. → favors **multi-precision integer
   arithmetic**.
2. **32 registers (16 GPR + 16 TMP, + RSP as a 33rd) vs native's 16, and
   looping inside one firing.** This is the Keccak win (full state residency,
   MOV-free χ). → favors **register-pressure-bound round functions**.

**Targeting filter (Goldmont has AES-NI, PCLMULQDQ, SHA-NI).** AES, GHASH,
SHA-256 are off the table — can't beat hardware. Fair game (no HW on this chip):
**Keccak/SHA-3 (done), ChaCha20, Poly1305, BLAKE2/3, SHA-512, PQC (ML-KEM/
ML-DSA — no AVX2 either).**

---

## 3. Idea catalog (Intel/Goldmont + CryptOpt tracks)

### A. Retarget CryptOpt to superoptimize the *triad schedule*
The microcode triad layer **is** a µarch with a cost model (1 µop/cyc issue,
fully-sequential intra-triad, MUL lat 6/tput 2.5, carry via SETCC→TMP, 3
ops/triad, 128-triad budget). Feed CryptOpt's randomized-local-search the DAG of
a 5×51 `fe_mul`/`fe_sq` and let it search triad packings.
- **Amortizes:** the 7-cyc tax is paid once per field op and is invariant to the
  schedule — pure upside.
- **Payoff:** our notes project a **~20% X25519 win from rescheduling the mul
  body to hide the carry chain** — currently unrealized (the 66-triad body was
  hand-scheduled). This cashes it.
- **Leverage:** CryptOpt internals + existing field-op patches + the
  `keccak_gen.py` triad simulator as the cost/verify oracle.
- **Novelty:** "superoptimizing microcode" is new; a paper section on its own.

### B. Beat amd64-64 on X25519 via a CryptOpt-scheduled fused sq+mul
The one embarrassment in current results: X25519 end-to-end **loses to amd64-64
by ~11%**, cause = *firing count* (~2560 firings). `probe_sqmul` found a fused
sq+mul ≈1.15× a single op, projecting a fewer-firings ladder at **~242k
(a win vs amd64-64's 271k)** — but "contingent on scheduling." (A) removes that
contingency. Concrete target: **flip X25519 from losing to winning.**

### C. ChaCha20 — the next Keccak
ARX cipher, 20 rounds, 16-word state + temps. Hits *both* edges: native spills
the state (16 GPRs isn't enough), microcode holds it in 32 regs and loops 20
rounds/firing.
- **Amortizes:** W ≈ 500 cyc/block → ~2% overhead.
- **Feasibility: easier than field arithmetic** — only ADD (mod 2³²), XOR, ROL
  (already proven via `probe_rol`); no MUL, no carry chains, no flag-domain pain.
- **Wins:** no HW competitor; same clean "does what native can't" narrative as
  Keccak. BLAKE2b/BLAKE3 are the same family for a third data point.

### D. ChaCha20-Poly1305 AEAD in one firing per message
We already have a working **Poly1305 mul patch**. New part = the message loop:
load key+acc once, `loop { load 16B block; mul-accumulate mod 2¹³⁰−5 }` over the
whole message in one firing, store once (Keccak's "loop the data" trick applied
to a MAC). Combine with (C) → the **full ChaCha20-Poly1305 AEAD** (TLS 1.3 /
WireGuard), the one AEAD with *no* Goldmont hardware. Much more
"deployable-flavored" story.
- **Amortizes:** 1 firing per ~1 KB message → overhead <1%.

### E. `ucodegen` — automated verified microcode patcher (the tooling spine)
Generalize `keccak_gen.py` into a retargetable toolchain:
```
DSL / fiat-crypto IR / CryptOpt IR  →  op-IR
  →  simulate on the Goldmont model (flag domains, 1 mem-op/triad, 8-bit signed
     offset, MOVE-banned, 128-triad budget)  →  check vs reference
  →  schedule triads  (deterministic  OR  CryptOpt-search from (A))
  →  emit ucode_t[] + wrapper  →  hardware KAT
```
The scheduling pass *is* project (A). A fiat-crypto front-end gives
*formally-derived, empirically-validated* microcode — a strong crypto-venue
artifact. Every kernel it emits is a whole primitive → outputs amortize by
construction.

### F. Moonshot: microcoded NTT for ML-KEM / ML-DSA (post-quantum)
Highest novelty/impact, highest risk. Goldmont has no AVX2, so lattice PQC (which
leans on AVX2 for 16-way NTT) is slow → real headroom. NTT butterfly = regular
loop of modular mul-add (16-bit coeffs mod q=3329), loopable in a firing,
register-hungry (32 regs help). Risk: no carry-chain advantage here and native
can also register-hold butterflies, so the win is less certain. But "first
microcoded PQC primitive" is a paper by itself.

### What to skip
- Any **single-instruction** microcode op CryptOpt sprinkles into x86 — dead on
  firing granularity (the mulx lesson).
- **AES, GHASH, SHA-256** — HW-accelerated on Goldmont; unwinnable.
- **Reducing the 7 cyc itself** — likely HW-fixed; amortization is the lever.

---

## 4. The AMD direction

Every "amd64" in the current tree is the *architecture* name (Bernstein's
amd64-51/amd64-64), not AMD silicon — so AMD *CPUs* are new ground. There are
**two lineages**, a decade apart, leading to opposite projects:

**1. AMD K8/K10 (Athlon 64 / Phenom, ~2004–2013) — the Koppe lineage.**
Koppe et al., *"Reverse Engineering x86 Processor Microcode"* (USENIX Sec 2017).
These CPUs do **no signature check** on microcode updates → patch RAM is freely
writable, **no exploit needed**. µop format, match-register hooking, and an
assembler are public. Most reproducible microcode target that exists. No AES-NI
on Phenom II → software AES/SHA are the only option, so a microcoded AES round
could be a clean win (old/slow core, but anyone can buy the hardware).

**2. AMD Zen 1–4 (Ryzen / EPYC, 2017–2024) — the EntrySign / zentool lineage.**
Google's 2025 disclosure: AMD's Zen microcode-update signature used an *example
CMAC key from a NIST publication*, so arbitrary microcode can be signed and
loaded; Google released **`zentool`**. Puts custom microcode on a **modern, wide,
high-IPC core** for the first time. Caveat: a *patched* vulnerability (AGESA/
microcode updates close it) → "works on un-updated firmware you control"; fine
for research, weaker as deployment.

### The key insight: nothing ports for free — the edge is µarch-specific
The Goldmont wins come from weaknesses microcode routes *around*:

| Goldmont weakness microcode exploits | Zen? | K10? |
|---|---|---|
| No ADX → no parallel carry chains natively | **Gone** (Zen has ADX/BMI2) | Survives (no ADX) |
| Only 16 GPRs → register-starved | **Gone** (AVX2/512 add SIMD state) | Survives (16-GPR, no AVX) |
| Low IPC (~1 µop/cyc) | **Gone** (Zen is 5–6 wide) | Survives-ish (~3-wide) |

Honest prediction for **Zen**: the field-arithmetic advantage likely
**collapses**, because the ISA already exposes what microcode was routing around.
**That collapse is not a failure — it is the strongest result.** It sharpens the
thesis into a falsifiable law:

> **Microcode crypto wins exactly when the ISA underserves the µarch — and not
> otherwise.** Goldmont hides parallel carry chains and registers the µarch has;
> Zen doesn't. Measure both and you *prove* where the headroom comes from.

---

## 5. DECIDED DIRECTION — Zen custom instructions, driven by CryptOpt

**Hardware:** AMD Zen (Ryzen/EPYC) via EntrySign/zentool.
**Goal:** revisit the failed custom-`mulx` experiment on a core where it might
work — measure Zen's firing tax; if low enough, add the fused op CryptOpt *wants*
but the ISA lacks, and let CryptOpt schedule it.

### Reframe: mulx failed because it was the wrong point on a spectrum
- **1 µop/firing (mulx):** tax swamps it. Dead.
- **whole field-op/firing:** amortizes, but CryptOpt isn't in the loop (just
  hand-generated microcode) — the existing Goldmont approach.
- **a tuned N-op micro-kernel/firing that CryptOpt schedules:** the unexplored
  sweet spot. The "custom instruction" is really a small superoptimized bundle
  (e.g. 4–8 fused MACCs), so tax/N is small *and* CryptOpt orchestrates bundles.

**Elegant point: CryptOpt's cost-driven search is the amortization oracle.** Give
it the custom op with its measured cost (tax included); its randomized search
emits it only if it beats `mulx+adcx+adox`. The "does the 7 cyc amortize?"
question is answered per-op, automatically, by the tool we already know.

### FIRST experiment (no microcode needed — can kill the project cheaply)
Answer with CryptOpt alone: **is CryptOpt's best Zen schedule for the target
field-op ALU/issue-port-bound, or integer-multiply-throughput-bound?**
- **Multiply-throughput-bound** (Zen ~1 int-mul/cyc; 5-limb schoolbook ≈ 25 muls
  ≈ 25 cyc unavoidable) → **no µop fusion can help**; premise dead, learned free.
- **ALU-port / µop-issue-bound** (adcx/adox/add carry work saturating ALU ports
  while the multiplier has slack) → a custom op folding add+carry *into* the
  multiply µop (offloading ALU ports) has real room. **Greenlight.**

CryptOpt already reports port pressure. Run it on `curve25519_mul`/`square` (and
secp256k1) for Zen, inspect the bottleneck. This one analysis decides viability
using only existing expertise.

### The Zen firing-tax model (gating experiment #2, if greenlit)
Port the Goldmont probe methodology via zentool:
1. Hook a trigger with an N-µop patch, sweep N, fit `cycles(N) = intercept +
   slope·N`. Intercept = MS entry/exit tax (Zen's "7 cycles"); slope = per-µop
   issue cost.
2. **The asymmetry that could make Zen win where Goldmont lost:** Goldmont issues
   microcode at ~1 µop/cyc; Zen's MSROM issues *wider* (~4 µops/cyc — confirm).
   A custom op replacing **three** x86 µops (`mulx`+`adcx`+`adox`) with one MSROM
   µop can beat native even against a modest tax — opposite of the 1-µop mulx.
   *Replace 3, not 1* is the whole bet.
3. Try several trigger opcodes for the cheapest hook; check the killers below.

### What the custom op should be (pending Zen RE)
A **fused integer multiply-accumulate with a third carry chain**:
`macc(hi, lo_acc, a, b) = a*b + acc`, producing a carry usable beyond CF/OF. ADX
gives native exactly *two* parallel carry chains; schoolbook accumulation has
more parallelism than two chains express, and the add+carry work pressures the
ALU ports. A µop that is one issue slot *and* supplies a 3rd+ accumulation
resource attacks both. Whether Zen microcode can express this in one µop is the
key RE unknown (the µop map itself is a paper contribution).

### The two-level CryptOpt idea (ambitious version)
CryptOpt operates at both levels: **schedules the bundles** in outer asm
(respecting the tax) *and* **superoptimizes the microcode inside each bundle**
(the triad-scheduler, project A). Co-design loop: CryptOpt's search says which
fused op most relieves the port bottleneck → implement it in microcode → repeat.
"Software/microcode co-design where the ISA is malleable" is a novel framing.

### Honest risks (state up front)
- **Throughput-bound not port-bound** → the free first experiment kills it (most
  likely failure mode; why it's first).
- **No fused multiply-add µop in Zen's repertoire** → "replace 3 µops" weakens;
  RE answers this.
- **Serialization:** if a patched instruction can't execute speculatively/OoO on
  Zen, it serializes the frontend in a hot loop and eats the win. Verify patched
  instrs stay pipelined.
- **Patched vuln:** reproducibility caveated to specific firmware; state plainly
  (like the Goldmont red-unlock caveat).
- **Harder verification:** less-documented µop semantics → need a Zen simulator
  (à la `keccak_gen.py`) built on RE'd semantics before hardware.

### Suggested first moves
1. **[free, decisive]** Run CryptOpt on Zen for the field ops; read the
   bottleneck port. Go/no-go for the whole idea.
2. **[setup]** Stand up zentool on the Zen box + a recovery story (bad patch =
   hang; want a second machine or watchdog).
3. **[gating]** Measure the tax + MSROM issue slope with a no-op/N-op patch sweep.
4. **[RE]** Map the µop repertoire, hunting the fused-MACC / 3rd-carry primitives.

### Open items to verify before committing hardware time
Exact Zen specifics drive steps 1 and 3 and need confirming (not from memory):
- zentool's current op set / assembler capabilities.
- Zen MSROM issue width.
- Zen adcx/adox/mulx port assignments and integer-multiply throughput.
- Whether patched instructions execute speculatively on Zen.

A cited deep-research brief on these would de-risk the plan cheaply.

---

## References / lineage
- Koppe et al., "Reverse Engineering x86 Processor Microcode," USENIX Security
  2017 (AMD K8/K10, freely-writable microcode).
- Google / EntrySign (2025): AMD Zen microcode signature bypass; `zentool`.
- Ermolov/Goryachy lineage: Intel Goldmont red-unlock (the current platform).
- CryptOpt: https://github.com/0xADE1A1DE/CryptOpt (randomized-search
  superoptimizer for field arithmetic; ships per-µarch cost models incl. Zen).
- Existing project docs: `design.md`, `report.md`, `keccak/docs/keccak_understand.md`,
  `paper/OUTLINE.md`.
