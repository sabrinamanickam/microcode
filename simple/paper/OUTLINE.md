# Cryptographic Primitives in Custom x86 Microcode — Annotated Outline & Claim→Evidence Map

**Target venue:** IACR TCHES (CHES)
**Framing (decided):** Feasibility + microarchitecture study. Thesis below.
**Scope (decided):** Two case studies — X25519 field arithmetic and Keccak-f[1600]. The
multi-curve work (P-256, P-224, secp256k1, poly1305) is *out of the featured scope*; it may
appear in one sentence as "we have applied the same method to N other primitives" with a
forward pointer, but no tables.
**Platform:** Intel Celeron N3350 (Goldmont), single core, `taskset -c 0`, microcode patch RAM
made writable via the Goldmont red-unlock (Ermolov/Goryachy lineage).

---

## 0. The one-sentence thesis (everything serves this)

> The x86 ISA is an abstraction boundary with a price; by authoring cryptographic primitives
> directly in Goldmont's microcode we measure that price directly, and find it is **small and
> bounded by the microarchitecture, not the ISA** — custom microcode beats the best hand-tuned
> *same-representation* code by only single-digit percentages, and the ceiling is set by patch-RAM
> size, per-firing dispatch tax, and one-µop-per-cycle issue, not by anything the compiler is
> leaving on the table.

The paper's value is **not** "microcode crypto is a deployable accelerator" (it is not — it needs
a red-unlocked debug CPU). The value is: (1) the **first** demonstration that nontrivial crypto
*can* be expressed in custom x86 microcode at all; (2) a **reverse-engineered programming model**
for the Goldmont microcode engine usable by future researchers; (3) a **quantified headroom
result** with a clean explanation of where the limit comes from.

State this explicitly and early (end of §1) so reviewers don't spend their review hunting for the
deployment story that isn't the point.

---

## 1. Introduction

**Goal:** motivate, state contributions, preview results, disarm the "who cares / not deployable"
reflex in the same breath.

**Beats:**
- Hook: crypto implementors fight for single-digit % on field arithmetic (fiat-crypto, CryptOpt,
  Bernstein–Schwabe hand-asm). All of them stop at the ISA. What's underneath?
- Microcode is the CPU's own implementation layer, normally opaque and vendor-locked. The Goldmont
  red-unlock makes its patch RAM writable, turning the microcode engine into a programmable target.
- Prior red-unlock work (Ermolov et al., Chip Red Pill, IntelTXE-PoC) used this to *inspect* or
  *backdoor* the CPU — RNG hooks, debug, trojans. **No one has used it to compute.** We implement
  real cryptographic arithmetic and a full hash permutation in microcode and measure them against
  the best code that exists for this chip.
- Contributions (bulleted, 4):
  1. First nontrivial cryptographic computation in custom x86 microcode: a 5×51 X25519 field
     multiplier/squarer (66/42 triads) and a fully looped 24-round Keccak-f[1600] (108 triads),
     both verified against reference vectors.
  2. A reverse-engineered, documented programming model for the Goldmont microcode engine — the
     execution/flag/memory/register semantics that any future microcode-computation work needs,
     plus a generator+simulator methodology that validates patches before they touch fragile hardware.
  3. Same-silicon, same-driver performance evaluation: microcode is ~6.5% faster than hand-tuned
     asm field ops for X25519 (cleanest comparison) and ~7% faster than the fastest scalar Keccak.
  4. A characterization of *when* descending to microcode helps and why the win is capped — the
     first measurement of the performance headroom below the x86 ISA for symmetric and
     public-key primitives.
- Results preview sentence with the three headline numbers (see Claim→Evidence map C1, C2, C3).
- Honesty paragraph: not deployable (red-unlock); the X25519 win is representation-specific (we
  beat 5×51 code but the 4×64 saturated algorithm is faster and doesn't fit patch RAM — §6/§7);
  Keccak is the unambiguous win.

**Evidence pointers:** see consolidated map below. **Gaps/TODO:** decide whether to name the chip
vendor-neutrally ("a low-power x86 core") or explicitly (Goldmont/N3350). Recommend explicit —
reproducibility is a strength, and the red-unlock is already public.

---

## 2. Background

**Goal:** give a CHES reader (crypto-fluent, not necessarily microarchitecture-fluent) exactly the
primitives and the platform facts needed, no more.

### 2.1 The two primitives
- **X25519 / Curve25519:** Montgomery ladder, RFC 7748; the 5×51 unsaturated radix-2^51
  representation; the per-iteration field-op mix (5 mul + 4 sq + adds/subs + cswap); Fermat
  inversion as an addition chain. (Source: `simple/design.md` §"Montgomery ladder step", `curve_basics.md`.)
- **Keccak-f[1600]:** the 24-round permutation, θ/ρ/π/χ/ι, 25 64-bit lanes, KAT anchor
  `0xF1258F7940E1DDE7`. (Source: `keccak/docs/keccak-explained.md`, `keccak_understand.md`.)

### 2.2 Microcode and the Goldmont red-unlock
- What microcode is; patch RAM / match-and-patch as the legitimate vendor erratum mechanism.
- Red-unlock: how the CPU enters the state where CRBUS/LDAT writes to patch RAM are possible
  (cite Ermolov/Goryachy, IntelTXE-PoC). Keep it to "this prior work makes patch RAM writable; we
  build on top." Do NOT re-derive the unlock.
- The match-and-patch hook: 32 entries, each redirects a ROM µaddress to patch RAM; we hook a
  benign trigger (`vmwrite` 0x0cd8 / `vmread` 0x0618) so that executing that instruction in
  userland runs our patch. (Source: lib-micro `source/patch.c` `hook_match_and_patch`,
  `include/ucode/match_and_patch_hook.h`; the Explore agent's mechanism report.)
- **Note the dual-mapping constraint** (consecutive µaddresses share a hook entry) only if it
  matters to the narrative — probably a footnote.

**Gaps/TODO:** get the exact lib-micro citation/DOI/URL and the precise CPUID steppings supported
(0x506C9 / 0x506CA per the mechanism report). Confirm whether the lib-micro toolkit is the user's
own or a third party — affects how §2.2 and Related Work attribute it.

---

## 3. The Goldmont microcode programming model (the µarch contribution)

**Goal:** this is the section that makes the paper reusable by others and establishes the
"feasibility + µarch" identity. Present the engine as a *target architecture* with its ISA-like
rules, each rule tied to the probe that established it. This is where the "we reverse-engineered a
machine" story lives.

### 3.1 Triad structure and patch RAM budget
- A patch = sequence of triads; each triad = 3 µops (slots 0/1/2, sequential semantics) + 1
  sequencing word. 128-triad ceiling at U7c00–U7dfc; staging region reserved → ~120–128 usable
  (the bootstrap-reclaim trick recovers the last 8). (Source: `microcode_findings.md` §1;
  memory `project_patch_ram_bootstrap_reclaim`.)
- µop encoding (48-bit, opcode/dst/src/imm/CRC), seqword encoding — summarize, full table in an
  appendix. (Source: mechanism report; lib-micro `include/inst.h`, `opcode.h`, `seqword.h`.)

### 3.2 Execution semantics (intra-triad)
- Fully sequential within a triad: slot k reads everything slots <k wrote. RAW/WAR/WAW all
  confirmed for arch + TMP regs across every slot pair. WAW: later slot wins.
  (Source: `microcode_findings.md` §3; memory `project_triad_hazards`; `tests/test_raw_war_waw.c`.)
- This is what lets us pack 3 dependent ops per triad (the carry-chain "triple-pack").

### 3.3 The flag domains — the single most important non-obvious fact
- Two (really three) independent flag domains: per-TMP internal ALU flags (set by ADD/SUB→TMP,
  read by SETCC), frozen architectural RFLAGS (read by ADC/branches, restored at END_SEQWORD),
  and a stuck conditional-execution domain. **ADC is unusable**; every carry chain must route
  through ADD→TMP + SETCC_CONDB. (Source: `microcode_findings.md` §4–5; `EXPERIMENTS.md`
  "Flag Architecture on Goldmont (Definitive)"; `adc_findings.md`; memory `feedback_no_cf_bridge`.)
- Tell the debugging story briefly (ADD→RAX silently reads CF=0; the 51-bit limbs masked the bug
  because they never overflow 64 bits) — it's both instructive and credibility-building.

### 3.4 Register file and the RSP trick
- 32 registers: 16 GPR (0x20–0x2f) + 16 TMP (0x30–0x3f). TMPs do NOT persist across firings; arch
  regs do. (Source: `microcode_findings.md` §3; CLAUDE.md.)
- RSP usable as a 32nd data register if saved/restored once per firing — the move that freed the
  register Keccak needed. (Source: `keccak_understand.md` Decision 5; `keccak/probes/probe_rsp.c`.)

### 3.5 Memory and control flow
- Memory ops (LDZX/STAD), 8-bit *signed* offset (±127 B = ±15 lanes → BASE_LANE centering), ≤1
  mem-op/triad (more hangs the machine), SEG_DS. Store-to-load forwarding works in-patch.
  (Source: `keccak_understand.md` Decision 4; probes `probe_offset.c`, `probe_seg.c`, `probe_triad.c`.)
- MOVE traps to an ~800k-cyc slow path → banned; use ZEROEXT. (memory `feedback_move_dsz64`.)
- Loop primitive: intra-triad XOR-sets-ZF + backward UJMPCC CONDNZ, count-up (SUB-imm reversed).
  This is what makes *looped* microcode (not just straight-line) possible — prerequisite for Keccak.
  (Source: `keccak_understand.md` Decision 4 & loop-control; memory `project_microcode_loops`;
  `probe_loop.c`, `ujmp_test.c`.)

### 3.6 Methodology: generator-first, simulator-validated
- We never hand-write triads at scale. `keccak_gen.py` emits an op-IR, simulates it on a
  register+memory model that *encodes the hardware constraints* (esp. the signed-offset assertion),
  checks against the reference, and only then lowers to triads. Caught real bugs (signed-offset
  truncation that had crashed the box) in software. (Source: `keccak_understand.md` Decision 3;
  `keccak/keccak_gen.py`.)
- This is a genuine methodological contribution worth its own subsection — "how to develop for a
  machine that crashes when you get it wrong."

**Gaps/TODO:** §3 risks being a grab-bag. Organize as "the engine as a target architecture:
execution model / flags / registers / memory / control flow / how we program it." Each rule = one
probe = one citation. Consider a single summary table "Goldmont microcode: the rules" as the
section's centerpiece.

---

## 4. Case study I — X25519 field arithmetic in microcode

**Goal:** show what a real field multiplier looks like as triads, and the engineering that closed
the gap to hand-asm.

**Beats:**
- The kernel: `fe_mul` (66 triads) and `fe_sq` (42 triads) for 5×51 mod 2^255−19; squaring fits
  more easily because a[i]·a[j] symmetry halves the MAC count. (Source: `design.md`;
  `microcode_findings.md` §7.2; `asm_op_curve25519*.c`.)
- The progressive-accumulation pattern (MAC triad + ACC triad, fully packed, no NOPs in middle
  limbs) and the triple-pack carry chain. (Source: `microcode_findings.md` §6.)
- The wrapper: how operands get into the exact registers the patch expects, the `rbp`=out-ptr
  trick (only reg microcode doesn't touch), "a[0] loaded last" pointer trick, 15 memops/mul.
  (Source: `design.md` §"fe_mul_ucode".)
- Integration: the ladder stays in C/inline-asm; only mul/sq are microcoded; why add/sub/mul121665/
  cswap stay native (don't amortize the ~7-cyc firing tax). (Source: `design.md` §"What's not in
  microcode".)
- The end-to-end optimization arc (this is a strong, honest engineering narrative):
  register-chaining the inline ladder (+3.2%) → inlined Fermat invert with SQ-chains (+1.0%) →
  closing 25% of the gap to amd64-64; and the **negative results** (fused add+sq, SQ+MUL fusion,
  Phase-A fusion all rejected because the 128-triad cap forces them to pay back more in invert than
  they save). (Source: `report.md` §3–§7.) The negative results are valuable — they show the
  ceiling is real, not a lack of effort.

**Evidence pointers:** C4–C7 below. **Gaps/TODO:** decide how much triad-level code to show — one
annotated MAC/ACC triad pair in the body, full listing in appendix or artifact. Confirm final
triad counts (report.md says fe_mul 66 / fe_sq 42; design.md §layer-summary also says 66/42; an
older line says "108 total" = 66+42 ✓).

---

## 5. Case study II — Keccak-f[1600] in microcode

**Goal:** the clean win, and the most "microcode does what native can't" story.

**Beats:**
- Why Keccak is the hard case for microcode (per the µarch rules in §3.7 of microcode_findings:
  "hashing-style primitives don't benefit" — cheap XOR/AND/ROT, vmwrite tax exceeds savings) — and
  why we win anyway. Set this tension up explicitly; it makes the result land.
- The five design decisions (this maps almost 1:1 to a great §5 from `keccak_understand.md`):
  1. Whole permutation in **one firing** — load 25 lanes once, 24 rounds resident, store once.
     Pays the 50 memory triads once/perm, not once/round.
  2. π is a permutation of cycles → a single looped body is correct for all 24 rounds (in-place
     along cycles returns lanes to canonical register slots). This is what makes looping sound.
  3. Generator-first (forward ref to §3.6).
  4. The hardware facts (forward ref to §3.5).
  5. RSP as 32nd register (forward ref to §3.4) — unlocked D-in-registers.
- The three optimizations that built the win, **with measured deltas** (the honest empirical arc):
  D-in-regs via RSP (1.00→0.99×) → MOV-free χ using the freed D-regs as NOTAND scratch (→0.95×,
  the big jump) → balanced θ-parity XOR tree depth 4→3 + RC byte-index off the ι critical path
  (→0.93×). Each attacks the round's dependency chain. (Source: `keccak_understand.md` Part 3.)
- The "native can't do this" point: full register residency + a free 32nd register + MOV-free χ
  are exactly the moves a register-starved native implementation is denied. That's *where the win
  comes from*, and it's the paper's cleanest illustration of the thesis.
- The honest ceiling: chain-bound, can't unroll (128-triad cap), so each trick is only a few %.

**Evidence pointers:** C2, C8. **Gaps/TODO:** reconcile the two cycle scales — `keccak_understand.md`
quotes 874 vs 942 (true core cycles, same-process) while `KECCAK_RESULTS.md` quotes 1910 vs 2047
(TSC-rate). Same 0.93× ratio. Decide which to lead with (recommend: lead with the ratio + the
same-process true-cycle pair, present the matrix at TSC-rate as the SUPERCOP-style table, and
explain the relationship once).

---

## 6. Evaluation

**Goal:** rigorous, reviewer-proof measurement. This section must visibly answer the
benchmark_review.md critique.

### 6.1 Methodology
- CPU pinned (`taskset -c 0`), frequency pinned (userspace governor, 1.094 GHz, no_turbo handling).
  (Source: `RESULTS.md` header.)
- **The TSC-vs-core-clock pitfall** stated as a methodological point: rdtsc ticks at ~1.1 GHz while
  the core bursts to 2.4 GHz, so cross-run absolute "cycles" are meaningless; only pinned-base or
  same-process-ratio numbers are valid. This is *why* we report the way we do. (Source:
  `keccak_understand.md` Part 3 preamble; `benchmark_review.md` item 2.) — **This also resolves the
  May-vs-June 2× discrepancy: all featured numbers come from the pinned RESULTS.md / same-process
  Keccak harness; the older benchmark_results.md absolutes are turbo-contaminated and are NOT cited.**
- Same-process back-to-back interleaving for Keccak (`asm_op_keccak_vs.c`); SUPERCOP-style
  compiler×flag matrix (gcc-11/12/13, clang-14/17/18 × O3/O2/Os/O = 24 configs), best-per-contender.
  (Source: `RESULTS.md`, `KECCAK_RESULTS.md`.)
- Verification: RFC 7748 vectors incl. 1000-iteration chain for X25519; Keccak KAT
  `0xF1258F7940E1DDE7`. State these as correctness gates, not afterthoughts.

### 6.2 X25519 results
- **Lead with the cleanest comparison (same-ladder, amd64-51 framework):** only `fe25519_mul`/
  `square` differ. ucode ~6.5% faster than Bernstein–Schwabe asm. (RESULTS.md same-ladder amd64-51.)
- Same-ladder our framework: ucode vs fiat / hand-C / cryptopt (ucode fastest by 16–20%). (RESULTS.md.)
- End-to-end (orientation, not the load-bearing claim): ucode-inline beats donna/fiat/cryptopt;
  **loses to amd64-64** by ~11%. Explain immediately (next bullet / §7).

### 6.3 Keccak results
- The 24-config matrix; microcode 0.93× the fastest scalar; we benchmarked *all* SUPERCOP variants
  to prove `x86_64_asm`/`opt64lcu24` is the genuine fastest baseline (SHLD variants are 4–5× worse
  on this Atom-class core — a nice aside about the chip). (KECCAK_RESULTS.md.)

**Gaps/TODO (must-do before submission):** (a) single test vector → rotate ≥10 (scalar,point)
pairs for X25519 (review item 6); (b) report IQR/p90 not just min/median (item 10); (c) `isolcpus`/
`chrt -f 99` or at least disclose (item 3); (d) re-run X25519 same-ladder with the *same-process
interleave* used for Keccak to get a ratio that doesn't depend on cross-run frequency state — this
would make the X25519 claim as bulletproof as the Keccak one and is probably the single highest-value
remaining experiment.

---

## 7. Discussion — when does descending to microcode pay off?

**Goal:** turn the two case studies into the general headroom result. This is the intellectual payoff.

**Beats:**
- The firing-cost model: ~7 cyc pure dispatch/call measured directly (probe_vmwrite_cost), plus
  frontend-drain + mandatory operand memory traffic ≈ ~12 cyc/call effective; per-X25519 ≈ 2560
  calls → ~18k of ~30k residual gap is dispatch. (Source: `report.md` §5; memory
  `project_inline2_perop_profile`, `project_microcode_firing_latency`.)
- The three structural ceilings: (1) 128-triad patch RAM (can't unroll, can't fuse without paying
  back), (2) per-firing tax (favors few firings over much work each), (3) no operand persistence
  across firings (forces memory round-trips unless everything is in one firing). (Sources:
  `microcode_findings.md` §7; `report.md` §2, §7.1.)
- The decision rule we extracted: microcode wins iff the whole computation fits in one firing AND
  the per-call work is *expensive* (MUL, long carry chains) in *high count*; squaring fits where
  multiplication doesn't; Solinas/independent carry chains parallelize, Montgomery/linear chains
  saturate TMP pressure. (Source: `microcode_findings.md` §7.)
- Why X25519-5×51 wins but amd64-64-4×64 wins overall: the 4×64 *saturated* algorithm is
  structurally fewer-and-cheaper ops, but it does not fit patch RAM (75-triad mul + lazy reduction,
  and per-call it's ~604k end-to-end). Microcode can only win within a representation it can hold;
  it cannot adopt the better algorithm. **This is the crisp statement of the headroom result:
  microcode buys you a constant-factor (single-digit %) on a *fixed* algorithm, not the freedom to
  run a better one.** (Source: `report.md` §1–2; memory `project_x25519_4x64_loses`.)
- Keccak as the contrast: there the better implementation strategy (full register residency) was
  *only* achievable in microcode (32nd register, MOV-free χ), so microcode wins outright.
- Honest forecast of remaining levers (Bernstein–Yang inversion: +1–2.5%, ~3 days, gated on a
  1-day probe). (Source: `report.md` §8.) — keep brief, it's future work.

**Gaps/TODO:** this section's strength is generality; resist re-listing curve specifics. The
"constant-factor not better-algorithm" framing is the sentence reviewers will quote — make it sharp.

---

## 8. Security implications (concise — feasibility/µarch is the spine, this broadens appeal)

**Goal:** give the CHES side-channel/security audience something, without diluting the thesis.

**Beats:**
- Microcode crypto trojans: if crypto can be *implemented* in microcode, it can be *subverted* in
  microcode (a malicious patch on the same hook, invisible to ISA-level audit). The benign-trigger
  hooking we use (`vmwrite`) is exactly the mechanism a trojan would use.
- Constant-time below the ISA: microcode arithmetic is straight-line, branch-free, data-independent
  by construction (and Keccak/field-mul here are CT). Discuss whether microcode could *guarantee*
  CT that survives compiler recompilation — and the converse risk that microcode-level
  data-dependent behavior is invisible to source/binary CT tooling. Tie to the BY-inversion
  valgrind-CT plan as the one place secret-dependent control flow could enter. (Source: `report.md` §8.4.)
- Scope honesty: we are not proposing a defense; we are mapping the surface.

**Gaps/TODO:** keep to ~0.75 page. Don't overclaim a side-channel result we didn't measure.

---

## 9. Related work

**Goal:** nail novelty. Three lineages to position against.

- **Microcode reverse-engineering / red-unlock:** Ermolov, Goryachy, Positive Technologies; Chip
  Red Pill; IntelTXE-PoC; Koppe et al. "Reverse Engineering x86 Processor Microcode" (USENIX Sec
  2017, AMD K8/K10); custom-microcode-for-defense work (e.g. microcode-based Spectre mitigations,
  hardening). **Our delta:** prior work inspects/patches/backdoors; we are first to use microcode
  for substantial *cryptographic computation* and to benchmark it competitively.
- **High-performance ECC/field arithmetic:** Bernstein–Schwabe (amd64-51/amd64-64), fiat-crypto
  (Coq-verified), CryptOpt (superoptimized asm), donna. **Our delta:** these are all at/above the
  ISA; we provide the first below-ISA datapoint and a clean same-silicon comparison against them.
- **Keccak implementation:** Keccak Code Package / XKCP, SUPERCOP scalar variants, the Van Keer ROL
  asm. **Our delta:** first microcode Keccak; beats the fastest scalar on this core.
- **Benchmarking methodology:** SUPERCOP. We follow its matrix/best-per-contender discipline.

**Gaps/TODO:** collect exact citations/DOIs. Confirm there is truly no prior "compute in x86
microcode" result (Koppe et al. do toy computation on AMD; distinguish: theirs is a RE
demonstration on AMD K8/K10, not competitive crypto on a modern Atom core). The novelty claim
hinges on this distinction — state it precisely.

---

## 10. Conclusion + artifact

- Restate the thesis as a measured result. Two primitives, two clean same-silicon wins over the
  best ISA-level code, one general headroom characterization.
- Artifact: the generator (`keccak_gen.py`), the patches, the benchmark harnesses, reference
  vectors — TCHES encourages artifacts; the simulator-validated generator is a strong artifact.

---

# Consolidated Claim → Evidence Map

Legend: **status** ✓ = number/fact verified in a result doc I read; ⚠ = needs a confirming re-run
or a citation before it can go in the paper; ✗ = known gap.

| # | Claim (as it would appear) | Evidence (file / number) | Status |
|---|---|---|---|
| C1 | Microcode `fe_mul`/`fe_sq` are ~6.5% faster than Bernstein–Schwabe amd64-51 hand-asm field ops, same ladder/driver/CPU. | `RESULTS.md` same-ladder amd64-51 table: geomean ratio a51/asm = **1.069** vs a51/ucode 1.000; best min 355,975 vs 332,323. (Older `benchmark_results.md` says 5.25% — turbo-contaminated absolutes, ratio consistent.) | ✓ (⚠ tighten via same-process re-run) |
| C2 | Microcode Keccak-f[1600] is ~7% faster than the fastest SUPERCOP scalar (0.93×). | `KECCAK_RESULTS.md`: 1910 vs 2047 cyc/perm (TSC-rate), ratio **0.933×**; `keccak_understand.md`: 874 vs 942 true core cyc, same ratio, same-process interleave. KAT `0xF1258F7940E1DDE7`. | ✓ |
| C3 | End-to-end microcode X25519 beats donna/fiat/cryptopt/hand-C but loses to amd64-64 (4×64). | `RESULTS.md` end-to-end: ucode-inline best **300,197**; donna 335,736; amd64-64 **271,314** (geomean ucode-inl vs amd64-64 = 0.892, i.e. amd64-64 ~11% faster). | ✓ |
| C4 | `fe_mul` = 66 triads, `fe_sq` = 42 triads; together 108 of ~120–128 usable. | `report.md` §7.1; `design.md` layer summary. | ✓ |
| C5 | Microcode `fe_mul` does 15 L1 memops/call; ladder ≈80k memops/X25519, not bandwidth-bound. | `design.md` §"fe_mul_ucode", §"per-iteration memory traffic". | ✓ |
| C6 | The inline-ladder register-chaining + inlined-invert optimizations banked +4.2% and closed ~25% of the gap to amd64-64 (40k→30k). | `report.md` §3,§4,§6 (315,548→302,212 min). | ✓ |
| C7 | Patch-fusion strategies (add+sq, SQ+MUL, Phase-A) are net losses because the 128-triad cap forces paying them back in `fe_invert`. | `report.md` §2.1, §7. | ✓ (analysis-based, not measured — label as such) |
| C8 | The Keccak win was built in measured steps: D-in-regs (0.99×) → MOV-free χ (0.95×) → θ-tree+RC-byte-index (0.93×). | `keccak_understand.md` Part 3. | ✓ |
| C9 | Per-`vmwrite` dispatch overhead ≈ 7 cyc, measured directly. | `report.md` §5.3 (8.97 cyc total − 1 cyc/triad). | ✓ |
| C10 | ADC is unusable in patches; carry chains must go through ADD→TMP + SETCC (two flag domains). | `microcode_findings.md` §4–5; `EXPERIMENTS.md` flag section; `adc_findings.md`. | ✓ |
| C11 | RSP is usable as a 32nd data register (save/restore once/firing); it unlocked D-in-registers for Keccak. | `keccak_understand.md` Decision 5; `probe_rsp.c` (PASS). | ✓ |
| C12 | Intra-triad semantics fully sequential (RAW/WAR/WAW, all slot pairs, arch+TMP). | `microcode_findings.md` §3; `tests/test_raw_war_waw.c`. | ✓ |
| C13 | Memory: 8-bit signed offset, ≤1 mem-op/triad, SEG_DS; MOVE traps to ~800k-cyc slow path. | `keccak_understand.md` Decision 4; probes; memory `feedback_move_dsz64`. | ✓ |
| C14 | Looped microcode works (UJMPCC CONDNZ + intra-triad XOR-sets-ZF, count-up). | `keccak_understand.md` loop-control; `probe_loop.c`; memory `project_microcode_loops`. | ✓ |
| C15 | Generator+simulator catches hardware-fatal bugs (signed-offset truncation) in software before hardware. | `keccak_understand.md` Decision 3; `keccak_gen.py`. | ✓ |
| C16 | This is the first use of red-unlocked x86 microcode for competitive cryptographic computation. | Novelty claim — needs Related Work survey to defend (Koppe et al. = RE/toy on AMD; Ermolov = inspect/backdoor). | ⚠ verify in §9 |
| C17 | Frequency pinned; min≈median (noise floor <0.02%). | `RESULTS.md` header; `benchmark_results.md` §caveats. | ✓ |

---

# Pre-submission experiment/fix checklist (ordered by value)

1. **[High] Same-process interleaved X25519 benchmark** mirroring `asm_op_keccak_vs.c`, so the
   field-op claim (C1) rests on a same-process ratio instead of cross-run mins. Closes review items
   2+3 for X25519 in one move.
2. **[High] Rotate ≥10 (scalar, point) test vectors** through the X25519 bench (review item 6).
3. **[Med] Report IQR/p90** alongside min/median everywhere (item 10).
4. **[Med] `isolcpus=0` + `chrt -f 99`** (or disclose absence) (item 3).
5. **[Med] One clean numeric scale**: pick pinned-base core cycles as the canonical unit, present
   TSC-rate matrices as such, and add one paragraph relating them. Purge contaminated absolutes.
6. **[Low] Exact citations/DOIs** for Related Work (esp. Koppe et al., Ermolov, fiat-crypto,
   CryptOpt, SUPERCOP, XKCP) and the lib-micro toolkit attribution.
7. **[Low] Decide artifact contents** and licensing for the red-unlock-dependent code.

---

# Open framing questions for the author (not blocking the outline)

- **Title direction?** e.g. "Below the ISA: Cryptographic Primitives in Intel Goldmont Microcode"
  or "How Much Performance Lives Below the x86 ISA? A Microcode Study of X25519 and Keccak."
- **Name the chip explicitly?** Recommend yes (reproducibility; unlock already public).
- **How much triad-level code in the body vs. appendix/artifact?**
- **Security section depth** — single page, or expand if a reviewer-pool skews security?
