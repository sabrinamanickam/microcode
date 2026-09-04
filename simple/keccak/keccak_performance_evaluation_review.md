# Improving the Keccak Performance Evaluation

The current evaluation establishes that the microcode implementation is faster, but it does not yet establish three things reviewers will want: statistical robustness, which optimizations produce the improvement, and whether the improvement carries over beyond an isolated permutation.

The implementation discussion should also correct the instruction-set statement: Goldmont lacks **BMI1**, which contains `ANDN`, not BMI2.

## What Is Lacking

### 1. The headline is based on minima

Use median cycles as the primary result. Minima are useful diagnostically, but they are vulnerable to noise and favorable sampling.

Using the reported medians:

- Microcode: 1917 cycles
- `opt64lcu24`: 2051 cycles
- Median speedup: $2051/1917 = 1.070\times$
- Cycle reduction: $6.5\%$

### 2. There is no variability measurement

Report an interquartile range, a 5th--95th percentile interval, or a confidence interval. A median of 1917 cycles is more persuasive when accompanied by its distribution.

### 3. The timed scope is insufficiently explicit

State whether the measurement includes:

- hooked-instruction dispatch;
- the 25 input loads and 25 output stores;
- native function-call overhead;
- wrapper register preservation;
- cache warm-up; and
- microcode installation cost, which is presumably excluded.

### 4. The baseline-selection procedure needs clarification

The paper selects the best of 24 compiler configurations. Explain whether the selection criterion is the lowest median or the lowest minimum. It should be the lowest median. For assembly implementations, clarify that the compiler affects only the wrapper.

### 5. The evaluation shows correlation, not causation

The final paragraph attributes the speedup to whole-state residency, resident $D$, fused transport, and move-free $\chi$, but none of these design choices is isolated experimentally.

### 6. Table 6 is not strong evidence

Compiler stability is unsurprising because the permutation body is fixed microcode. Replace Table 6 with an ablation table and move the complete compiler sweep to an appendix or the artifact.

### 7. The claim is broader than the experiment

The reported experiment measures one single-state Keccak-f[1600] permutation on one Goldmont processor. Either add SHA3-256 results for representative message lengths or state this limitation explicitly. Do not describe the result as general Keccak or SHA-3 throughput unless that has been measured.

### 8. The latency conclusion is not demonstrated

The observation of 1.37 cycles per triad does not by itself prove that dependency latency is the limiting factor. Support the claim with ablations, performance counters, or a scheduling model. Otherwise, soften it to:

> Performance appears limited by dependency latency and round-loop overhead.

## Recommended Main Comparison Table

Make the median the primary statistic, show dispersion, and retain only meaningful single-state baselines in the main paper. Move MMX and SHLD variants to supplementary results.

| Implementation | Method | Median cycles | 5th--95th percentile | Microcode speedup | Selected build |
|---|---|---:|---:|---:|---|
| **Microcode** | Looped, full-state resident | **1917** | *measure* | 1.000x | GCC 12 `-O2` wrapper |
| `opt64lcu24` | Scalar C, lane complementing, 24-round unroll | 2051 | *measure* | **1.070x** | GCC 11 `-Os` |
| OpenSSL | CRYPTOGAMS scalar assembly | 2053 | *measure* | 1.071x | GCC 11 `-O` wrapper |
| `x86_64_asm` | Van Keer scalar assembly | 2073 | *measure* | 1.081x | GCC 11 `-Os` wrapper |
| `xkcp_g64lc` | XKCP scalar, lane complementing | 2160 | *measure* | 1.127x | GCC 12 `-O2` |
| `xkcp_g64` | XKCP scalar, no lane complementing | 2270 | *measure* | 1.184x | Clang 18 `-Os` |
| `simple` | Portable reference C | 2371 | *measure* | 1.237x | GCC 11 `-O3` |

Suggested caption:

> **Table X:** Median cycles per complete Keccak-f[1600] permutation on the Goldmont N3350. Each value is computed from [number] measurement batches of [number] consecutive permutations after [warm-up procedure]. The timed region [state exactly what it includes]. The interval gives the 5th--95th percentiles of the batch measurements. “Microcode speedup” is the contender's median divided by the microcode median; values above one indicate that microcode is faster. For each contender, the build with the lowest median over the 24-configuration sweep is reported. Compiler configurations shown for assembly implementations apply only to their native wrappers.

## Recommended Ablation Table

Replace the current compiler-stability table with a measured ablation. This would connect the claimed implementation contributions to the final performance result.

| Variant | Optimization removed | Triads/round | Memory micro-ops/round | Median cycles | Slowdown |
|---|---|---:|---:|---:|---:|
| **Final implementation** | -- | **56** | **18** | **1917** | -- |
| Serial parity reduction | Balanced $\theta$ schedule | 56 | 18 | *measure* | *measure* |
| `NOT` + `AND` | Native microcode `NOTAND` | *measure* | 18 | *measure* | *measure* |
| Spill $D$ | Resident-$D$ allocation | *measure* | *measure* | *measure* | *measure* |
| Conservative cycle schedule | Cross-slot cycle-walk packing | *measure* | 18 | *measure* | *measure* |

The `NOT`-plus-`AND` variant is particularly useful. After correcting BMI2 to BMI1 in the implementation text, this experiment would directly quantify the advantage of the microcode `NOTAND` operation that is unavailable to native Goldmont code.

Only include variants that can actually be executed. If a variant does not fit in the 128-triad patch, report that as a capacity result rather than presenting an estimated cycle count.

## Recommended Evaluation Structure

The performance-evaluation subsection should proceed in this order:

1. Define the timed region and statistical procedure.
2. Report the median comparison: 1917 versus 2051 cycles, or a $1.070\times$ speedup.
3. Present the main baseline table.
4. Present the ablation and explain which design choices materially affect performance.
5. State the limitations: one processor, a single-state permutation, and exclusion of patch-installation cost.
6. Optionally report end-to-end SHA3-256 results.

## Suggested Headline Result

> The microcode implementation has a median cost of 1917 cycles per permutation, compared with 2051 cycles for the fastest native baseline. It therefore requires 6.5% fewer cycles, corresponding to a $1.070\times$ speedup.

This is methodologically stronger than the current minimum-based claim while preserving essentially the same result.
