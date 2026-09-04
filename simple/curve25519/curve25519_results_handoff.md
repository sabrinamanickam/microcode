# Handoff: Curve25519 Performance Evaluation for the CHES/TCHES Paper

## Purpose of this handoff

This document fixes the intended presentation of the Curve25519/X25519 results.

The evaluation should make the paper's strongest positive result clear without hiding the main negative result. In particular, the paper should distinguish between:

1. a **controlled field-arithmetic comparison**, where only multiplication and squaring change;
2. the **saturated 4×64 result**, where microcode performs substantially worse because the 128-triad patch capacity prevents a dedicated squarer;
3. an **end-to-end comparison**, where complete implementations differ in representation, ladder structure, inversion, packing, and field arithmetic;
4. a **decomposition experiment**, which explains how much of the final 5×51 result comes from the field backend and how much comes from ladder integration.

The first table should therefore show both the successful 5×51 result and the unsuccessful 4×64 saturated result. The paper should be explicit from the beginning that microcode is beneficial in the representation that fits the patch budget, but not in the saturated representation.

The benchmark source for the numbers below is `RESULTS.md`.

The presentation style can follow Niederhagen and Pham, *Improving ML-KEM and ML-DSA on OpenTitan*, IACR TCHES 2026. In particular, their software tables report cycle counts in thousands, round to three significant figures, state the statistic used, and give normalized values relative to a baseline. We should follow that style where it fits our experiment, but we must not copy their iteration count unless our own benchmark harness confirms it.

---

# 1. CPU and Timing Setup

Use the following setup consistently in the methodology and in all table captions or notes that need it.

## Processor

- CPU: **Intel Celeron N3350**
- Microarchitecture: **Goldmont**
- Nominal frequency: **1.10 GHz**
- Benchmark core: **core 0**
- Process pinning: `taskset -c 0`

## Frequency configuration

- CPU frequency governor: **userspace**
- Pinned/requested frequency: **1,094,400 kHz**
- Turbo: **disabled**
- `no_turbo = 1`
- Delivered core frequency under load: **1,100 MHz**
- TSC/RDTSC rate: **1,094 MHz**

The delivered core frequency and TSC frequency are not identical.

The measured conversion is

\[
\frac{f_{\mathrm{core}}}{f_{\mathrm{TSC}}}=1.00548.
\]

This ratio was obtained using APERF/MPERF under load and verified before the benchmark sweep.

## What the raw measurements are

The benchmark records elapsed values using `RDTSC`.

The raw numbers in `RESULTS.md` are therefore **TSC ticks**, not exactly core cycles.

For final paper tables, if the column is called `cycles`, multiply every absolute timing by

\[
1.00548.
\]

The correction does **not** change any performance ratio because the same multiplicative factor applies to every contender.

Do not mix corrected and uncorrected numbers in the paper.

### Recommended convention

Report:

> **median core kcycles per X25519**

where the raw TSC measurement has first been multiplied by 1.00548.

Round the displayed value to **three significant figures**, following the style used in the TCHES 2026 OpenTitan paper.

Examples:

- 304,946 TSC ticks → 306,617 core cycles → **307 kcycles**
- 300,482 TSC ticks → 302,129 core cycles → **302 kcycles**
- 271,328 TSC ticks → 272,815 core cycles → **273 kcycles**

Keep exact values in the artifact or appendix if desired.

## Compiler sweep

The benchmark sweep contains 24 compiler/optimization configurations:

- GCC 11
- GCC 12
- GCC 13
- Clang 14
- Clang 17
- Clang 18

with:

- `-O`
- `-O2`
- `-O3`
- `-Os`

The full sweep is useful for robustness but should not dominate the main paper.

## Selection and comparison rules

Use two distinct rules, because they answer different questions.

### Absolute end-to-end performance

For a complete implementation, report the **lowest median** observed across the 24 compiler/optimization configurations.

This is the number used for the headline absolute performance of each contender.

### Controlled backend comparisons

When making a causal claim about the field backend, compare matched compiler configurations.

For each compiler/optimization configuration, compute

\[
\frac{C_{\mathrm{comparison}}}{C_{\mathrm{microcode}}}.
\]

Then report the geometric mean of those 24 paired ratios in the text or in a dedicated robustness column.

This avoids attributing compiler-selection effects to microcode.

## Statistic

The current result file establishes that the headline statistic is the **median**.

The final methodology must also state the exact number of timed repetitions used to obtain each median.

**TODO: recover the exact repetition count from the benchmark harness before submission. Do not invent it.**

The TCHES 2026 OpenTitan paper reports the median of 10,000 iterations, but that is only a presentation precedent. We must report our own actual count.

## Details that must not be claimed until confirmed from the harness

Before final submission, verify and document:

- exact repetition count;
- whether and how `RDTSC` is serialized;
- whether timing-call overhead is subtracted;
- cache warm-up policy;
- input-generation policy;
- whether measurements use fixed or varying inputs;
- any explicit pre-benchmark warm-up iterations.

Do not infer these from the current `RESULTS.md`.

---

# 2. Recommended Main-Text Table Structure

The main text should contain **three performance artifacts**.

1. **Table 1: Controlled field-arithmetic comparison across representations**
2. **Table 2: End-to-end X25519 performance**
3. **Table 3 or Figure: 5×51 integration/decomposition**

The full compiler sweep and dispersion data belong in the appendix.

---

# 3. Table 1: Controlled Field-Arithmetic Comparison Across Representations

## Role of this table

This should be the **first and main Curve25519 performance table**.

Its purpose is to answer:

> What happens when we change the field multiplication and squaring backend while holding the ladder and surrounding implementation fixed?

The table should show **both**:

- the positive 5×51 result;
- the negative saturated 4×64 result.

This is important. The paper should not first advertise the 5×51 win and reveal the 4×64 loss later. The capacity limitation is part of the result.

## Critical experimental rule

There are two separate controlled blocks.

The ladder is identical **within each block**, but the 5×51 and 4×64 blocks do not use the same framework or representation.

Therefore:

- comparisons **within** the 5×51 block are causal field-backend comparisons;
- comparisons **within** the 4×64 block are causal field-backend comparisons;
- absolute numbers should **not** be used to causally compare one block against the other.

The table caption must say this explicitly.

---

## 3.1 5×51 block

### Ladder to use

Use the **common backend-neutral C Montgomery ladder** used by:

- `ucode/C-ladder`
- `a51ops/C-ladder`
- `ours/cryptopt`
- `ours/fiat`
- `ours/hand-C`

This is the correct ladder for the main 5×51 comparison.

Do **not** use:

- our final register-chained inline-assembly ladder;
- the amd64-51 monolithic qhasm ladder.

The common C ladder is the only setup in which all five field backends can be substituted while leaving the surrounding X25519 computation unchanged.

### What is held constant

Within this block, keep constant:

- Montgomery ladder;
- driver;
- Fermat inversion;
- conditional swap;
- packing/encoding;
- representation, 5×51;
- surrounding source;
- compiler/optimization configuration for paired comparisons.

Only the implementations of field multiplication and squaring change.

### Backends to report

1. **microcode 5×51**
2. fiat-crypto
3. CryptOpt
4. hand-written C using `__uint128_t`
5. Bernstein–Schwabe amd64-51 field assembly used as per-operation calls

### Best-median absolute values

Raw values from `RESULTS.md` and core-corrected values:

| Backend | Raw TSC ticks | Corrected core cycles | Paper value |
|---|---:|---:|---:|
| **microcode 5×51** | 304,946 | 306,617 | **307 kcycles** |
| fiat-crypto | 354,868 | 356,813 | **357 kcycles** |
| CryptOpt | 379,832 | 381,913 | **382 kcycles** |
| hand-written C | 381,186 | 383,275 | **383 kcycles** |
| amd64-51 field asm | 383,343 | 385,444 | **385 kcycles** |

The best medians above may come from different compiler configurations. That is acceptable for the absolute `kcycles` column as long as the selection rule is stated.

### Paired robustness result

Across the 24 matched compiler/optimization configurations, microcode wins in **24/24 configurations against every evaluated 5×51 backend**.

Paired geometric-mean speedups:

- vs fiat-crypto: **1.196×**
- vs CryptOpt: **1.220×**
- vs amd64-51 field assembly: **1.223×**
- vs hand-written C: **1.242×**

These paired geometric means are stronger evidence for the field-backend claim than best-vs-best ratios.

---

## 3.2 Saturated 4×64 block

### Ladder to use

Use the **amd64-64 framework with its C `ladderstep.c`** for both rows.

The two rows must use the same C ladder object source.

This block compares:

- `amd64-64/asm-Clad`
- `amd64-64/ucode`

Do **not** use the native monolithic amd64-64 qhasm ladder in this controlled block.

The native qhasm implementation belongs in the end-to-end table.

### What is held constant

Within the 4×64 block, keep constant:

- amd64-64 framework;
- C ladder;
- driver;
- inversion;
- packing;
- conditional swap;
- saturated 4×64 representation.

Only the field backend changes.

### Values

| Backend | Raw TSC ticks | Corrected core cycles | Paper value |
|---|---:|---:|---:|
| **amd64-64 field asm, C ladder** | 306,387 | 308,066 | **308 kcycles** |
| **4×64 microcode, C ladder** | 513,782 | 516,598 | **517 kcycles** |

The microcode implementation therefore requires approximately

\[
\frac{513782}{306387}\approx 1.677
\]

times as many cycles as the assembly backend in this controlled comparison.

Across the matched compiler sweep, the corresponding result is approximately **1.679× as many cycles**.

Avoid the phrase `1.677× slower`.

Use:

> The 4×64 microcode backend requires 1.677× as many cycles as the assembly backend.

or:

> Replacing the 4×64 assembly field operations with microcode increases the cycle count by 67.7%.

### Explain the reason directly in the table note

The negative result is tied to patch capacity.

The 4×64 saturated multiplier occupies **75 triads**.

Under the **128-triad patch limit**, this leaves insufficient capacity for a separate optimized squarer.

The microcode implementation therefore computes

\[
\mathrm{sq}(a)=\mathrm{mul}(a,a).
\]

This limitation should appear in the table caption or immediately following paragraph, not several pages later.

The correct conclusion is not that microcode field arithmetic is always faster.

The correct conclusion is:

> Microcode improves the 5×51 implementation when the specialized field operations fit within the patch budget, but the patch-capacity limit prevents the saturated 4×64 implementation from using a competitive arithmetic design.

---

## 3.3 Recommended Table 1 layout

A good main-text layout is:

| Representation | Common ladder/framework | Field backend | kcycles/X25519 | Relative cycles |
|---|---|---|---:|---:|
| **5×51** | common C ladder | **microcode** | **307** | **×1.00** |
| 5×51 | common C ladder | fiat-crypto | 357 | ×1.16 |
| 5×51 | common C ladder | CryptOpt | 382 | ×1.25 |
| 5×51 | common C ladder | hand-written C | 383 | ×1.25 |
| 5×51 | common C ladder | amd64-51 asm | 385 | ×1.26 |
| **4×64 saturated** | amd64-64 C ladder | **assembly** | **308** | **×1.00** |
| 4×64 saturated | amd64-64 C ladder | microcode | 517 | ×1.68 |

`Relative cycles` should be normalized **within each representation block**.

Lower is better.

The 5×51 block uses microcode as ×1.00.

The 4×64 block uses assembly as ×1.00.

This prevents a misleading single normalization across two different frameworks.

### Suggested caption

> **Table X: Controlled X25519 field-arithmetic comparison.** Cycle counts are median core kcycles per X25519, rounded to three significant figures. Within each representation block, the ladder and surrounding implementation are identical and only field multiplication and squaring change. The 5×51 rows use our common C Montgomery ladder. The 4×64 rows use the same amd64-64 C ladder. Relative cycle counts are normalized within each block. The 4×64 microcode implementation uses multiplication for squaring because the 128-triad patch capacity does not permit a dedicated squarer.

### Text immediately after Table 1

The first paragraph should say both sides of the result.

Suggested content:

> With the 5×51 representation fixed, microcode outperforms every evaluated ISA-level field backend. The advantage persists across all 24 matched compiler and optimization configurations, with geometric-mean speedups between 1.196× and 1.242×. This result does not extend to the saturated 4×64 representation. With the amd64-64 C ladder held fixed, the microcode backend requires 1.677× as many cycles as the assembly backend. The 128-triad patch capacity prevents the 4×64 implementation from including both its 75-triad multiplier and a dedicated squarer, forcing squaring through multiplication.

This should be the main performance claim.

---

# 4. Table 2: End-to-End X25519 Performance

## Role

This table answers a different question:

> How fast is the complete implementation compared with complete existing implementations?

This table is **orientation and overall performance**, not causal attribution.

Nothing is held constant across all rows.

Implementations may differ in:

- representation;
- ladder structure;
- degree of assembly fusion;
- inversion;
- field operations;
- packing;
- code organization.

The table and prose must say that directly.

## Ladder rule

For this table, each implementation should use its **own complete optimized/native configuration**.

That means:

### This work

Use:

- 5×51 representation;
- our final **register-chained inline-assembly ladder**;
- microcode field operations.

Raw best median: 300,482 TSC ticks  
Corrected: 302,129 core cycles  
Paper value: **302 kcycles**

### Bernstein–Schwabe amd64-64

Use its **native monolithic qhasm/assembly implementation**.

Do not force it onto a C ladder in this table.

Raw: 271,328  
Corrected: 272,815  
Paper value: **273 kcycles**

This must appear above our row because it is faster.

### Other complete implementations

Use their best observed complete configurations.

Recommended rows:

| Complete implementation | Representation | Raw TSC ticks | Corrected paper value |
|---|---|---:|---:|
| Bernstein–Schwabe amd64-64 asm | 4×64 | 271,328 | **273 kcycles** |
| **this work** | **5×51** | **300,482** | **302 kcycles** |
| amd64-51 framework + microcode | 5×51 | 315,202 | 317 kcycles |
| donna c64 | 5×51 | 335,805 | 338 kcycles |
| fiat-crypto | 5×51 | 354,868 | 357 kcycles |
| Bernstein–Schwabe amd64-51 asm | 5×51 | 356,042 | 358 kcycles |
| CryptOpt | 5×51 | 379,832 | 382 kcycles |
| hand-written C | 5×51 | 381,186 | 383 kcycles |

## Recommended table layout

| Implementation | Representation | kcycles/X25519 | Relative cycles |
|---|---|---:|---:|
| amd64-64 asm | 4×64 | **273** | ×0.903 |
| **this work** | **5×51** | **302** | **×1.00** |
| amd64-51 + microcode | 5×51 | 317 | ×1.05 |
| donna c64 | 5×51 | 338 | ×1.12 |
| fiat-crypto | 5×51 | 357 | ×1.18 |
| amd64-51 asm | 5×51 | 358 | ×1.18 |
| CryptOpt | 5×51 | 382 | ×1.26 |
| hand-written C | 5×51 | 383 | ×1.27 |

Normalize `Relative cycles` to this work if that column is retained.

Lower is better.

The most important visual fact is that **amd64-64 is the fastest complete implementation**.

Do not bury that fact.

### Suggested caption

> **Table Y: End-to-end X25519 performance.** Cycle counts are median core kcycles per X25519, rounded to three significant figures. Each row uses the implementation's best observed compiler configuration. These are complete implementations and differ in representation, ladder structure, inversion, field arithmetic, and code organization. The table therefore establishes overall performance but does not isolate the effect of microcode.

## Recommended prose

State explicitly:

- our final implementation is not the fastest overall;
- it is competitive with complete software implementations;
- it is faster than the evaluated 5×51 complete implementations;
- the fastest overall implementation is the saturated amd64-64 assembly implementation;
- Table 1 explains why the 4×64 representation cannot benefit from the current patch capacity.

Avoid implying that the end-to-end gap is entirely caused by field multiplication.

---

# 5. Table 3 or Figure: 5×51 Integration and Ladder Decomposition

## Role

This artifact explains why the field-level win in Table 1 does not translate directly into the same end-to-end ratio.

Use the **amd64-51 framework** for all rows.

This is a decomposition experiment.

## Rows and ladders

| Variant | Ladder | Field ops | Raw TSC ticks |
|---|---|---|---:|
| amd64-51 native | qhasm, monolithic | qhasm asm | 356,042 |
| C-ladder control | C, per-op calls | qhasm asm | 386,845 |
| C-ladder + microcode | C, per-op calls | microcode | 328,807 |
| chained ladder + microcode | inline asm, register chained | microcode | 315,202 |

Corrected and rounded:

| Variant | kcycles/X25519 |
|---|---:|
| amd64-51 native | **358** |
| C-ladder control | **389** |
| C-ladder + microcode | **331** |
| chained ladder + microcode | **317** |

## Interpretation

The transitions isolate three effects.

### 1. Native qhasm ladder → C ladder

356,042 → 386,845 raw ticks.

This is the cost of losing the native ladder/field-operation fusion.

It is not a microcode cost.

### 2. Assembly field ops → microcode field ops

386,845 → 328,807 raw ticks.

The ladder and framework are held fixed.

This is the controlled microcode field-op improvement in the amd64-51 framework:

**1.177× speedup** for the selected medians.

Across matched configurations, the geometric-mean speedup is approximately **1.166×**.

### 3. C ladder → register-chained inline-assembly ladder

328,807 → 315,202 raw ticks.

This recovers part of the ladder overhead by avoiding unnecessary register/materialization costs.

Selected-median improvement: **1.043×**.

## Presentation

A waterfall or transition figure may communicate this better than a table.

If page space is tight, use a figure with:

`358 → 389 → 331 → 317 kcycles`

and labels:

- loss of qhasm fusion;
- replace asm field ops with microcode;
- register-chain the ladder.

If a table is used, retain the `ladder` and `field ops` columns so the control is obvious.

## Important limitation

The final row here is `amd64-51/ucode` at 315,202 raw ticks, not the canonical `ours/ucode` implementation at 300,482 raw ticks.

Do not conflate them.

This decomposition is performed inside the amd64-51 framework specifically to isolate the effects.

---

# 6. What Should Not Be a Separate Main Table

## Do not make a separate main table for 4×64 alone

The controlled 4×64 result should already appear in Table 1.

A separate 4×64 table would duplicate the most important negative result.

Discuss the 75-triad multiplier and lack of a dedicated squarer directly after Table 1.

## Do not put the full 24-configuration matrices in the main text

They are audit data.

Move them to the appendix.

## Do not put the complete dispersion table in the main text

Keep:

- median;
- p10;
- p90;
- minimum if desired;
- selected configuration;

in an appendix table.

In the main text, one sentence can summarize that the observed spreads are smaller than the important inter-contender gaps.

---

# 7. Appendix Structure

Recommended appendix organization:

## Appendix A: Full compiler sweep

Include the 24 compiler/optimization configurations for:

- complete implementations;
- common-C-ladder 5×51 backends;
- 4×64 C-ladder control pair.

## Appendix B: Paired ratios

Include the per-configuration ratios and geometric means.

This is where the 24/24 win result can be audited.

## Appendix C: Dispersion

For each selected configuration, report:

- median;
- minimum;
- p10;
- p90;
- p90 − p10;
- compiler/optimization setting.

If the exact number of timed repetitions is not already obvious from the methodology, repeat it in the caption.

---

# 8. Recommended Reporting Language

## Use

- `median core kcycles/X25519`
- `relative cycles`
- `requires 1.677× as many cycles`
- `provides a 1.223× speedup`
- `reduces the cycle count by X%`
- `with the ladder held fixed`
- `within the 5×51 representation`
- `within the saturated 4×64 representation`
- `complete implementations differ in representation and code structure`

## Avoid

- `1.677× slower`
- calling raw RDTSC ticks `cycles` without applying the 1.00548 correction;
- claiming the end-to-end table isolates microcode;
- comparing a native qhasm ladder against a C-ladder microcode implementation and attributing the entire difference to field arithmetic;
- calling amd64-51 assembly a universal `best ISA-level implementation`;
- hiding the 4×64 negative result until late in the section.

---

# 9. Recommended Evaluation Narrative

The Curve25519 evaluation should follow this order.

## Paragraph 1: Measurement setup

State:

- Goldmont Celeron N3350;
- core pinning;
- fixed-frequency setup;
- turbo disabled;
- RDTSC timing;
- APERF/MPERF-derived correction;
- compiler sweep;
- median statistic;
- exact repetition count once confirmed.

## Paragraph 2: Controlled field arithmetic

Introduce Table 1.

State that the table has two controlled representation blocks.

For 5×51, all backends use the common C ladder.

For 4×64, both backends use the same amd64-64 C ladder.

Then give both outcomes immediately:

- 5×51 microcode wins;
- 4×64 microcode loses.

## Paragraph 3: Robustness of the 5×51 result

Report:

- 24/24 matched-configuration wins against every 5×51 backend;
- geometric-mean paired speedups.

This is the strongest causal evidence.

## Paragraph 4: Explain the 4×64 failure

Connect it to the implementation section:

- 75-triad multiplier;
- 128-triad patch limit;
- no dedicated squarer;
- squaring becomes multiplication.

Frame this as a capacity result, not an embarrassing outlier.

## Paragraph 5: End-to-end performance

Introduce Table 2.

State that the fastest complete implementation is amd64-64 assembly.

Then position this work:

- 302 kcycles;
- faster than the evaluated complete 5×51 alternatives;
- slower than the native saturated 4×64 implementation.

Do not attribute those complete-implementation differences to field arithmetic alone.

## Paragraph 6: Decomposition

Use Table 3 or the waterfall figure to show:

- native fusion benefit;
- field-op benefit;
- register-chained ladder recovery.

This explains how the controlled result maps into the final implementation.

---

# 10. Central Claim to Preserve

The Curve25519 result should not be summarized as:

> Microcode is faster than software for Curve25519.

That is too broad and contradicted by the 4×64 experiment.

A defensible summary is:

> In the 5×51 representation, microcode field multiplication and squaring outperform all evaluated ISA-level backends when the surrounding X25519 implementation is held fixed. The benefit persists across all 24 compiler and optimization configurations. The result does not extend to the saturated 4×64 representation, where the 128-triad patch capacity prevents a dedicated squarer and makes the microcode backend substantially slower than assembly. Our final 5×51 implementation remains competitive end to end but does not outperform the native amd64-64 implementation.

This is the honest story and should be visible from the first results table.

---

# 11. Final Table Checklist

## Table 1 — main controlled result

**Title:** Controlled X25519 field-arithmetic comparison

### 5×51 group
- ladder: our common backend-neutral C ladder;
- field backends: microcode, fiat, CryptOpt, hand-C, amd64-51 field asm;
- only mul/sq differ;
- report 307, 357, 382, 383, 385 kcycles;
- show relative cycles;
- report 24/24 matched wins and paired geomeans in text.

### 4×64 group
- ladder: amd64-64 C `ladderstep.c`;
- field backends: amd64-64 asm and microcode;
- report 308 and 517 kcycles;
- explicitly note `sq = mul(a,a)` in microcode because of patch capacity.

## Table 2 — complete implementations

- each implementation uses its own optimized/native ladder;
- first row: amd64-64 asm, 273 kcycles;
- this work: 302 kcycles;
- include representation column;
- state explicitly that the table is not controlled.

## Table 3 / Figure — decomposition

- framework: amd64-51 throughout;
- native qhasm + asm: 358 kcycles;
- C ladder + asm: 389 kcycles;
- C ladder + microcode: 331 kcycles;
- chained inline-asm ladder + microcode: 317 kcycles;
- use to explain fusion loss, microcode gain, and chaining recovery.

## Appendix

- all 24 compiler configurations;
- matched ratios;
- geometric means;
- dispersion;
- exact selected compiler settings;
- raw TSC values if desired for auditability.

---

# 12. One Important Open Methodology Item

Before the paper is finalized, recover the **exact benchmark repetition count** and the precise `RDTSC` serialization/warm-up procedure from the benchmark source.

The current result summary supports the CPU setup, frequency settings, timing rate, correction factor, compiler sweep, median statistic, and selected results, but it does not state those remaining details.

Do not fill them from memory or from another paper.
