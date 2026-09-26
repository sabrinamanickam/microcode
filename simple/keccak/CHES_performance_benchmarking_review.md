# CHES Performance Benchmarking Review

Your benchmarking methodology is already stronger than what many CHES/TCHES implementation papers report, but there are a few points I would fix before submission because a careful reviewer could legitimately question them.

There does **not appear to be a single mandatory CHES software-benchmarking recipe**. The recurring expectations in accepted implementation papers and the CHES artifact material are reproducibility, an exact description of the target and toolchain, a well-defined timing boundary, robust repeated measurements, correctness checking, and fair baselines.

SUPERCOP is especially relevant to your methodology. It deliberately tries multiple implementations and compiler configurations, checks correctness, and selects the implementation/compiler combination minimizing the median cycle count. So your decision to compile each native contender under all 24 SUPERCOP compiler/optimization configurations and retain its best result is defensible and consistent with established crypto benchmarking practice.

## How your methodology looks

| Issue | Assessment | What I would do |
|---|---|---|
| Same physical processor | **Good** | Keep |
| Single-core pinning | **Good** | Keep |
| Fixed frequency | **Good, wording needs correction** | Say frequency scaling/burst is disabled |
| Frequency verification with APERF/MPERF | **Very good** | Keep and explain conversion |
| RDTSC | **Potential issue** | State how you serialize it |
| 200 batches × 1000 permutations | **Very strong** | Define exactly what one batch measurement means |
| Median as headline statistic | **Good** | Keep |
| \(p_{10}\)-\(p_{90}\) dispersion | **Good** | Keep |
| Minimum | **Fine as supplementary** | Do not make it the headline |
| Interleaving contenders | **Excellent** | Keep; rotating their execution order would be even better |
| Best of SUPERCOP compiler settings | **Good** | Keep; preferably remeasure selected binaries in final sweep |
| Broad native baseline set | **Very good** | Add exact versions/commits |
| End-to-end SHA3 benchmark | **Very good** | Keep |
| Correctness tests | **Good but should appear earlier** | Explicitly state all timed implementations were validated |
| Common end-to-end code | **Excellent** | Keep |
| Exact OS/compiler/microcode environment | **Currently incomplete** | Add it |
| Definition of timed native boundary | **Currently insufficiently explicit** | Add it |

There are **four issues I would consider important enough to fix**.

## 1. Do not say that you disable “turbo” on the N3350

Intel lists the N3350 as having a **1.10 GHz base frequency and a 2.40 GHz Burst Frequency**, while Intel Turbo Boost Technology is unsupported.

So this:

> We pin execution to one core, disable turbo, and fix the core to its 1100 MHz base frequency.

is technically not quite right for this processor.

I would use something like:

> We pin execution to one core, disable dynamic frequency scaling and burst operation, and fix the core to its 1100 MHz base frequency.

That is a small change, but an architecture reviewer may notice the distinction immediately.

## 2. You must state how you serialize `RDTSC`

This is the biggest methodological omission I see.

`RDTSC` is not a serializing instruction. Without appropriate ordering, it can execute before earlier instructions have finished, and later instructions can begin before the timestamp has been read.

Your batching makes this much less damaging because the timer boundary cost/error is amortized over 1000 permutations, but a reviewer can still reasonably ask:

> How was RDTSC ordered with respect to the code under measurement?

If you already use fences or `RDTSCP`, say so explicitly.

If you currently use plain `RDTSC` with no ordering, I would change the benchmark before submission.

## 3. Explain exactly what “200 batches of 1000” means

Your description says:

> each measurement reports the median over 200 batches of 1000 consecutive executions.

That sounds sensible, but it leaves one important ambiguity. I suspect you do:

\[
t_i =
\frac{T_{\mathrm{end}}-T_{\mathrm{start}}}{1000}
\]

for each batch and then report

\[
\operatorname{median}(t_1,\ldots,t_{200}).
\]

If so, say that.

That tells a reviewer immediately that only two timestamp reads occur per 1000 permutations and that timing overhead is therefore heavily amortized.

Your use of median and \(p_{10}\)-\(p_{90}\) is appropriate. You do **not** need to replace this with means and standard deviations.

## 4. Be much more explicit about the timing boundary for the native implementations

You define the microcode boundary nicely:

> The timed region covers the hooked instruction, the redirection into patch RAM, the 25 input loads, the 24 rounds, and the 25 output stores.

Good.

But the corresponding native boundary should be just as explicit. A reviewer needs to know that the native measurement includes the complete call from the same memory-resident input state to the same memory-resident output state.

Otherwise there is an obvious question:

> Does microcode pay its entry, load, store, and exit costs while the native measurements cover only the internal permutation body?

Make the symmetry explicit.

State that every contender begins with the same state available in memory and ends with the resulting state in memory, or whatever your actual contract is. Then state whether call/invocation overhead is included consistently.

## Additional improvements

Your frequency treatment is unusually careful and worth keeping. Because the TSC rate is not identical to the core's operating frequency, an `RDTSC` tick is not literally one core cycle. Your 1100/1094 conversion gives approximately

\[
\frac{1100}{1094}=1.00548.
\]

So calling it `1.005` is fine if deliberately rounded, but explicitly state whether **the cycle counts in the tables have already been multiplied by this factor**.

Use “TSC ticks” for the raw values and “core cycles” only after conversion.

Your interleaving is particularly good. Running all contenders in the same process and interleaving them batch by batch controls thermal and temporal drift much better than benchmarking implementation A completely and then implementation B.

If your ordering is fixed as A, B, C, … for every batch, rotating or randomizing the contender order between batches would remove even the possibility of systematic ordering bias. I would regard that as an enhancement rather than a requirement.

Your compiler search is also fine. There is one refinement that would make your methodology especially hard to criticize:

1. Use the 24 configurations to **select** each implementation's best configuration.
2. Run a new final interleaved measurement using only those selected binaries.

That separates configuration selection from final performance measurement.

## Your performance arithmetic is correct

Your main result reports 1896 cycles for microcode and 2055 for the fastest native implementations.

The cycle reduction is

\[
\frac{2055-1896}{2055}=7.74\%.
\]

The speedup is

\[
\frac{2055}{1896}=1.0839\times.
\]

So reporting **7.7% fewer cycles and \(1.084\times\) speedup** is correct.

There is one terminology change I strongly recommend.

Your table calls

\[
\frac{T_\text{native}}{T_\text{microcode}}
\]

**“Relative cost.”**

That is really a **speedup**. A larger value means microcode is faster, which is the opposite of what readers normally expect from “relative cost.”

Rename that column `Speedup` or possibly `Relative performance`.

## Your ablation study is strong

You do not merely count removed operations. You build executable variants, validate them, and measure the resulting hardware behavior.

That is strong experimental design.

The only language I would watch is causal attribution. One-at-a-time ablations measure the **marginal effect of removing a technique from the completed design**. They do not prove that the improvements are independent or additive.

So this conclusion is very defensible:

> Both choices are necessary for the complete kernel to outperform the fastest native implementation in our comparison.

It is slightly better than saying the two choices independently “account for” some total improvement, because there can be interactions between register residency, packing, and scheduling.

## Your end-to-end SHA3 experiment is valuable

Using one common sponge implementation and changing only the permutation backend is a strong design.

You also test NIST vectors, including messages around the SHA3-256 padding boundary, before timing.

That makes the end-to-end result much more convincing than simply extrapolating from permutation cycles.

There is one weakness:

> Every native implementation in this experiment builds under a single configuration rather than the best configuration of the permutation comparison.

If practical, fix that.

The cleanest setup would be to compile the **shared sponge code identically** and compile/link each permutation backend using its previously selected best configuration.

If this is technically impossible, disclose it clearly and avoid using that experiment to estimate the precise speedup over the best possible native SHA3 implementation.

## Add reproducibility information

The `Target Machine` section should also state the exact:

- OS and kernel version
- CPU CPUID/model/stepping
- vendor microcode revision before applying your patch
- SUPERCOP version
- GCC and Clang versions
- OpenSSL version or commit
- XKCP version or commit
- benchmark compiler/linker flags not already captured by the SUPERCOP configuration

For your paper this is particularly important because the research target is microcode itself. Another N3350 with a different stepping or microcode revision may be more consequential than it would be for an ordinary C benchmark.

Your artifact should ideally regenerate:

- raw measurements
- selected compiler configurations
- final medians
- dispersion statistics
- the tables reported in the paper

## Bottom line

I would **keep your overall benchmarking design**.

The following aspects are already strong:

- 200 × 1000 batched measurements
- median as the headline statistic
- dispersion reporting
- fixed operating frequency
- APERF/MPERF verification
- same-process interleaving
- broad native comparison
- executable ablations
- correctness validation
- end-to-end SHA3 testing

Before submission, I would definitely fix these points:

1. Call the N3350 mechanism burst/frequency scaling rather than Turbo Boost.
2. Document `RDTSC` serialization.
3. Define exactly how a batch becomes a cycles-per-permutation measurement.
4. Explicitly state the equivalent timing boundary for native implementations.
5. Rename **Relative cost** to **Speedup**.
6. Add complete reproducibility metadata.

With those changes, the measurement methodology itself should be difficult for a CHES reviewer to attack. The reviewer discussion is then more likely to focus on whether the baselines and claims are appropriate rather than whether the timing experiment is trustworthy.

## Sources consulted

- CHES hardware artifact submission tips and suggestions: https://ches.iacr.org/2022/CHES-hardware-artifact-submission-tips-and-suggestions.pdf
- SUPERCOP benchmarking documentation: https://bench.cr.yp.to/supercop.html
- Intel Celeron N3350 specifications: https://www.intel.com/content/www/us/en/products/sku/95598/intel-celeron-processor-n3350-2m-cache-up-to-2-40-ghz/specifications.html
- Intel Software Developer's Manual: https://cdrdv2-public.intel.com/774492/325383-sdm-vol-2abcd.pdf
- IACR CHES/TCHES artifact archive: https://artifacts.iacr.org/tches/
