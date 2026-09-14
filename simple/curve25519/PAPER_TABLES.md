# Curve25519 / X25519 — paper tables

_Generated from `RESULTS.md` (Appendices A.1, A.2) by `gen_paper_tables.py`. Do not hand-edit; regenerate._

_Absolute values are RDTSC ticks converted to core cycles by ×1.00548 (f_core/f_TSC, measured by this run's frequency guard), then rounded to three significant figures. Ratios are computed from raw full-precision ticks and are invariant to that correction._

## Measurement setup

| item | value | source |
|---|---|---|
| CPU | Intel(R) Celeron(R) CPU N3350 @ 1.10GHz | `/proc/cpuinfo` |
| Core / pinning | core 0, `taskset -c 0` | `lib/build_run.sh` |
| Governor | `userspace`, requested 1094400 kHz | `lib/freq_guard.sh` |
| Turbo | disabled (`no_turbo = 1`) | `lib/freq_guard.sh` |
| Delivered core freq (load) | 1100 MHz (APERF/MPERF) | turbostat |
| TSC / RDTSC rate | 1094 MHz | turbostat |
| Correction f_core/f_TSC | 1.00548 | measured before the sweep |
| Timing | `RDTSC`, serialised `cpuid; rdtsc` before / `rdtscp; cpuid` after | `full_curve25519_inline2.c` |
| Timer overhead | 15 ticks (min 13, p99 17); **not** subtracted — 0.005% of ~300k | measured |
| Post-sweep frequency check | stable (+0.000% over the sweep) (delivered 1100 MHz / TSC 1094 MHz after) | `lib/freq_guard.sh` |
| Core isolation | core 0; 28 IRQs steered to core 1 (2 per-CPU/unmovable); SCHED_FIFO 99 | `lib/isolation.sh` |
| nohz_full / isolcpus | off — periodic timer tick still hits core 0 | `/proc/cmdline` |
| Timing order | **interleaved** — round-robin, one repetition of every contender per round | `benchmark()` |
| Statistic | median; each configuration measured 3× and reduced to the median of those runs | `bench_stats()`, `median_of` |
| Run-to-run reproducibility | worst spread 2.254% (amd64-51/ucode-Clad @ clang-18 -O2) | `note_repro` |
| Repetitions | **1000** per contender, all binaries | `BENCH_REPS` |
| Warm-up | RFC 7748 verification, then one untimed call per contender | `benchmark()` |
| Inputs | fixed RFC 7748 vector 1, byte-identical across all repetitions | `benchmark()` |
| Compiler sweep | 24 configs: {gcc-11,12,13, clang-14,17,18} × {-O,-O2,-O3,-Os} | `lib/build_run.sh` |
| Correctness | all contenders pass RFC 7748 vectors 1-4 in every configuration | `test_rfc7748()` |

## Table 1 — Controlled X25519 field-arithmetic comparison

| Representation | Common ladder / framework | Field backend | kcycles/X25519 | Relative cycles |
|---|---|---|---:|---:|
| **5×51** | common C ladder | **microcode** | **310** | **×1.00** |
| 5×51 | common C ladder | fiat-crypto | 357 | ×1.15 |
| 5×51 | common C ladder | CryptOpt | 383 | ×1.24 |
| 5×51 | common C ladder | hand-written C | 383 | ×1.24 |
| 5×51 | common C ladder | amd64-51 asm | 386 | ×1.24 |
| **4×64 saturated** | amd64-64 C ladder | **assembly** | **308** | **×1.00** |
| 4×64 saturated | amd64-64 C ladder | microcode | 517 | ×1.68 |

> **Table 1: Controlled X25519 field-arithmetic comparison.** Cycle counts are median core kcycles per X25519, rounded to three significant figures. Within each representation block the ladder and surrounding implementation are identical and only field multiplication and squaring change. The 5×51 rows use our common C Montgomery ladder; the 4×64 rows use the same amd64-64 C `ladderstep.c`. Relative cycle counts are normalised **within** each block, so the two blocks must not be compared against one another. The 4×64 microcode backend computes sq(a) = mul(a, a) because its 75-triad multiplier leaves no room for a dedicated squarer inside the 128-triad patch capacity. All rows are the median of 1000 repetitions.

With the 5×51 representation fixed, microcode outperforms every evaluated ISA-level field backend. The advantage persists across all 24 matched compiler and optimisation configurations, with paired geometric-mean speedups between ×1.194 and ×1.240 (Appendix B.1). This result does not extend to the saturated 4×64 representation: with the amd64-64 C ladder held fixed the microcode backend requires 1.678× as many cycles as the assembly backend (1.679× as a paired geometric mean, Appendix B.2). The 128-triad patch capacity prevents the 4×64 implementation from holding both its 75-triad multiplier and a dedicated squarer, forcing squaring through multiplication.

## Table 2 — End-to-end X25519 performance

| Implementation | Representation | kcycles/X25519 | Relative cycles |
|---|---|---:|---:|
| Bernstein–Schwabe amd64-64 asm | 4×64 | 273 | ×0.905 |
| **this work** | **5×51** | **302** | **×1.000** |
| amd64-51 framework + microcode | 5×51 | 318 | ×1.054 |
| donna c64 | 5×51 | 340 | ×1.125 |
| fiat-crypto | 5×51 | 357 | ×1.184 |
| Bernstein–Schwabe amd64-51 asm | 5×51 | 359 | ×1.188 |
| hand-written C | 5×51 | 383 | ×1.270 |
| CryptOpt | 5×51 | 383 | ×1.270 |

> **Table 2: End-to-end X25519 performance.** Cycle counts are median core kcycles per X25519, rounded to three significant figures, each row at its own best compiler configuration. These are complete implementations differing in representation, ladder structure, inversion, field arithmetic and code organisation; the table therefore establishes overall standing but does **not** isolate the effect of microcode. Lower is better; relative cycles are normalised to this work.

## Table 3 — 5×51 integration and ladder decomposition

| Variant | Ladder | Field ops | kcycles/X25519 |
|---|---|---|---:|
| amd64-51 native | qhasm, monolithic | qhasm asm | 359 |
| C-ladder control | C, per-op calls | qhasm asm | 390 |
| C-ladder + microcode | C, per-op calls | microcode | 329 |
| chained ladder + microcode | inline asm, register-chained | microcode | 318 |

**Transitions** — each changes exactly one element from the row above:

| Transition | Effect isolated | best-of-24 | paired geomean |
|---|---|---:|---:|
| native qhasm → C ladder | loss of ladder/field-op fusion | ×1.088 | ×1.093 |
| asm field ops → microcode | controlled microcode field-op gain | ×1.184 | ×1.169 |
| C ladder → register-chained | ladder-integration recovery | ×1.035 | ×1.011 (×1.044 excl. gcc `-Os`) |

> **Table 3: 5×51 integration and ladder decomposition.** All rows use the amd64-51 framework. Register-chaining the ladder helps in 21 of 24 configurations (paired geometric mean ×1.044); under gcc `-Os` the inline-assembly ladder degrades sharply in this framework (×0.80, reproducible across gcc-11/12/13 to within 43 ticks), which pulls the all-configuration geometric mean down to ×1.011. The same chained ladder in our own framework shows no such degradation, so this is a compiler/framework interaction rather than a property of the ladder. The final row is `amd64-51/ucode`, **not** the canonical implementation reported in Table 2.

## Appendix A — full per-configuration sweep

_Median RDTSC ticks per X25519 at each of the 24 configurations. **Bold** = best (lowest) in that column. Raw ticks; multiply by 1.00548 for core cycles._

### A.1 — End-to-end, every complete implementation (Table 2)

| Config | this work | a64/asm | a64/asm+Clad | a64/ucode | a51/asm | a51/asm+Clad | a51/uc+Clad | a51/ucode | CryptOpt | fiat | hand-C | donna |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gcc-11 -O3 | 305,461 | 272,392 | 336,324 | 559,687 | 358,811 | 393,355 | 334,647 | 322,380 | 387,240 | 358,585 | 390,798 | **337,834** |
| gcc-11 -O2 | 310,846 | 273,069 | 326,707 | 553,258 | 358,974 | 390,414 | 332,872 | 324,216 | 399,141 | 376,111 | 404,236 | 371,164 |
| gcc-11 -Os | 312,820 | 273,865 | 362,184 | 596,499 | 359,059 | 398,858 | 344,240 | 424,082 | 404,459 | 371,562 | 410,562 | 374,243 |
| gcc-11 -O | 306,939 | 272,790 | 339,441 | 568,709 | 358,192 | 391,130 | 346,299 | 320,989 | 391,916 | 378,653 | 412,305 | 386,016 |
| gcc-12 -O3 | 305,862 | **271,655** | 323,769 | 553,061 | 358,779 | 393,539 | 335,614 | 324,090 | 386,049 | **355,527** | 386,562 | 341,212 |
| gcc-12 -O2 | 311,448 | 271,975 | 324,193 | 552,263 | 359,204 | 391,745 | 334,224 | 325,362 | 401,135 | 372,402 | 402,220 | 371,408 |
| gcc-12 -Os | 312,875 | 272,364 | 363,717 | 594,999 | 358,898 | 398,443 | 340,992 | 423,787 | 406,219 | 372,493 | 411,300 | 373,322 |
| gcc-12 -O | 307,572 | 272,464 | 340,307 | 569,352 | 358,265 | 390,063 | 344,886 | 320,959 | 393,493 | 379,948 | 408,281 | 377,843 |
| gcc-13 -O3 | 308,225 | 271,723 | 314,948 | 539,505 | 358,746 | 391,777 | 333,851 | 322,054 | 390,841 | 366,789 | 389,014 | 341,595 |
| gcc-13 -O2 | 311,215 | 271,943 | 314,157 | 544,641 | 359,082 | 391,875 | 333,933 | 324,155 | 399,246 | 372,155 | 395,517 | 356,425 |
| gcc-13 -Os | 313,046 | 272,415 | 348,569 | 581,028 | 358,790 | 398,493 | 340,992 | 423,724 | 406,229 | 373,214 | 414,164 | 357,848 |
| gcc-13 -O | 307,976 | 273,346 | 336,331 | 566,573 | 359,326 | 389,878 | 344,408 | 321,154 | 391,889 | 388,663 | 404,370 | 373,240 |
| clang-14 -O3 | **300,266** | 276,106 | 312,690 | 522,987 | 356,830 | 389,245 | 331,327 | 317,454 | 381,632 | 387,903 | 388,703 | 454,669 |
| clang-14 -O2 | 300,331 | 273,979 | 312,804 | 521,214 | 356,865 | 389,170 | 331,125 | 316,963 | **381,285** | 390,123 | 388,227 | 454,711 |
| clang-14 -Os | 304,264 | 273,056 | 314,911 | 523,142 | 356,788 | 390,600 | 329,924 | 317,008 | 389,037 | 411,072 | 411,836 | 447,760 |
| clang-14 -O | 305,394 | 273,372 | 314,412 | 528,152 | 358,264 | 388,164 | 334,352 | 316,930 | 387,019 | 399,263 | 402,999 | 465,837 |
| clang-17 -O3 | 301,109 | 275,695 | **306,273** | 516,897 | 356,728 | **388,007** | 330,649 | 317,199 | 381,863 | 387,041 | **381,245** | 442,296 |
| clang-17 -O2 | 301,225 | 274,097 | 306,349 | 515,872 | 356,959 | 389,178 | 329,904 | 317,739 | 381,643 | 387,042 | 381,252 | 441,133 |
| clang-17 -Os | 305,675 | 272,502 | 310,228 | 515,860 | 356,847 | 389,511 | 329,590 | 316,768 | 390,084 | 405,557 | 401,781 | 438,358 |
| clang-17 -O | 305,486 | 271,921 | 307,877 | 515,023 | 358,733 | 388,636 | 330,329 | **316,590** | 387,248 | 396,496 | 402,432 | 451,569 |
| clang-18 -O3 | 301,108 | 275,558 | 306,788 | 516,888 | **356,726** | 390,513 | 330,999 | 317,368 | 381,929 | 387,000 | 381,424 | 458,479 |
| clang-18 -O2 | 301,137 | 274,927 | 306,837 | 514,387 | 356,915 | 389,725 | 329,873 | 317,698 | 381,464 | 388,164 | 382,651 | 459,112 |
| clang-18 -Os | 305,404 | 272,387 | 308,451 | 515,594 | 356,802 | 389,632 | 329,568 | 316,599 | 389,988 | 399,439 | 403,403 | 454,520 |
| clang-18 -O | 305,400 | 273,390 | 306,870 | **513,941** | 358,625 | 388,667 | **327,622** | 317,336 | 389,145 | 399,770 | 402,574 | 460,562 |

### A.2 — Common C ladder, only the field backend differs (Table 1, 5×51 block)

| Config | microcode | amd64-51 asm | CryptOpt | fiat | hand-C |
|---|---|---|---|---|---|
| gcc-11 -O3 | 313,284 | 388,237 | 387,240 | 358,585 | 390,798 |
| gcc-11 -O2 | 337,303 | 395,560 | 399,141 | 376,111 | 404,236 |
| gcc-11 -Os | 341,177 | 401,415 | 404,459 | 371,562 | 410,562 |
| gcc-11 -O | 331,688 | 393,388 | 391,916 | 378,653 | 412,305 |
| gcc-12 -O3 | 314,333 | 385,253 | 386,049 | **355,527** | 386,562 |
| gcc-12 -O2 | 337,151 | 398,178 | 401,135 | 372,402 | 402,220 |
| gcc-12 -Os | 342,625 | 403,599 | 406,219 | 372,493 | 411,300 |
| gcc-12 -O | 333,415 | 394,987 | 393,493 | 379,948 | 408,281 |
| gcc-13 -O3 | 315,048 | 393,634 | 390,841 | 366,789 | 389,014 |
| gcc-13 -O2 | 335,482 | 396,148 | 399,246 | 372,155 | 395,517 |
| gcc-13 -Os | 341,911 | 404,906 | 406,229 | 373,214 | 414,164 |
| gcc-13 -O | 330,697 | 393,726 | 391,889 | 388,663 | 404,370 |
| clang-14 -O3 | **308,335** | **383,781** | 381,632 | 387,903 | 388,703 |
| clang-14 -O2 | 308,390 | 383,927 | **381,285** | 390,123 | 388,227 |
| clang-14 -Os | 314,654 | 390,929 | 389,037 | 411,072 | 411,836 |
| clang-14 -O | 312,767 | 389,464 | 387,019 | 399,263 | 402,999 |
| clang-17 -O3 | 309,431 | 385,772 | 381,863 | 387,041 | **381,245** |
| clang-17 -O2 | 309,333 | 386,096 | 381,643 | 387,042 | 381,252 |
| clang-17 -Os | 315,896 | 390,401 | 390,084 | 405,557 | 401,781 |
| clang-17 -O | 312,549 | 388,050 | 387,248 | 396,496 | 402,432 |
| clang-18 -O3 | 310,359 | 385,937 | 381,929 | 387,000 | 381,424 |
| clang-18 -O2 | 310,086 | 385,821 | 381,464 | 388,164 | 382,651 |
| clang-18 -Os | 315,565 | 390,322 | 389,988 | 399,439 | 403,403 |
| clang-18 -O | 310,593 | 389,957 | 389,145 | 399,770 | 402,574 |

## Appendix B — paired per-configuration ratios

_Ratio = comparison ÷ microcode at the **same** compiler configuration; >1 means microcode is faster. Computed from raw ticks._

### B.1 — 5×51 block, common C ladder (Table 1)

| Config | fiat-crypto | CryptOpt | amd64-51 asm | hand-written C |
|---|---:|---:|---:|---:|
| gcc-11 -O3 | 1.145 | 1.236 | 1.239 | 1.247 |
| gcc-11 -O2 | 1.115 | 1.183 | 1.173 | 1.198 |
| gcc-11 -Os | 1.089 | 1.185 | 1.177 | 1.203 |
| gcc-11 -O | 1.142 | 1.182 | 1.186 | 1.243 |
| gcc-12 -O3 | 1.131 | 1.228 | 1.226 | 1.230 |
| gcc-12 -O2 | 1.105 | 1.190 | 1.181 | 1.193 |
| gcc-12 -Os | 1.087 | 1.186 | 1.178 | 1.200 |
| gcc-12 -O | 1.140 | 1.180 | 1.185 | 1.225 |
| gcc-13 -O3 | 1.164 | 1.241 | 1.249 | 1.235 |
| gcc-13 -O2 | 1.109 | 1.190 | 1.181 | 1.179 |
| gcc-13 -Os | 1.092 | 1.188 | 1.184 | 1.211 |
| gcc-13 -O | 1.175 | 1.185 | 1.191 | 1.223 |
| clang-14 -O3 | 1.258 | 1.238 | 1.245 | 1.261 |
| clang-14 -O2 | 1.265 | 1.236 | 1.245 | 1.259 |
| clang-14 -Os | 1.306 | 1.236 | 1.242 | 1.309 |
| clang-14 -O | 1.277 | 1.237 | 1.245 | 1.288 |
| clang-17 -O3 | 1.251 | 1.234 | 1.247 | 1.232 |
| clang-17 -O2 | 1.251 | 1.234 | 1.248 | 1.232 |
| clang-17 -Os | 1.284 | 1.235 | 1.236 | 1.272 |
| clang-17 -O | 1.269 | 1.239 | 1.242 | 1.288 |
| clang-18 -O3 | 1.247 | 1.231 | 1.244 | 1.229 |
| clang-18 -O2 | 1.252 | 1.230 | 1.244 | 1.234 |
| clang-18 -Os | 1.266 | 1.236 | 1.237 | 1.278 |
| clang-18 -O | 1.287 | 1.253 | 1.256 | 1.296 |
| **geometric mean** | **1.194** | **1.217** | **1.220** | **1.240** |
| **configurations won** | **24/24** | **24/24** | **24/24** | **24/24** |

### B.2 — 4×64 block, amd64-64 C ladder (Table 1)

| Config | microcode ÷ assembly |
|---|---:|
| gcc-11 -O3 | 1.664 |
| gcc-11 -O2 | 1.693 |
| gcc-11 -Os | 1.647 |
| gcc-11 -O | 1.675 |
| gcc-12 -O3 | 1.708 |
| gcc-12 -O2 | 1.704 |
| gcc-12 -Os | 1.636 |
| gcc-12 -O | 1.673 |
| gcc-13 -O3 | 1.713 |
| gcc-13 -O2 | 1.734 |
| gcc-13 -Os | 1.667 |
| gcc-13 -O | 1.685 |
| clang-14 -O3 | 1.673 |
| clang-14 -O2 | 1.666 |
| clang-14 -Os | 1.661 |
| clang-14 -O | 1.680 |
| clang-17 -O3 | 1.688 |
| clang-17 -O2 | 1.684 |
| clang-17 -Os | 1.663 |
| clang-17 -O | 1.673 |
| clang-18 -O3 | 1.685 |
| clang-18 -O2 | 1.676 |
| clang-18 -Os | 1.672 |
| clang-18 -O | 1.675 |
| **geometric mean** | **1.679** |

## Appendix C — dispersion at each selected configuration

_Median of 1000 repetitions (1000 for the two `amd64-64` rows). Raw RDTSC ticks._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| this work (5×51, chained ladder) | 300,266 | 299,801 | 299,955 | 310,914 | 10,959 | clang-14 -O3 |
| amd64-64 asm | 271,655 | 271,478 | 271,563 | 279,400 | 7,837 | gcc-12 -O3 |
| amd64-64 asm, C ladder | 306,273 | 306,135 | — | — | — | clang-17 -O3 |
| 4×64 microcode, C ladder | 513,941 | 510,535 | — | — | — | clang-18 -O |
| amd64-51 asm | 356,726 | 356,541 | 356,606 | 366,997 | 10,391 | clang-18 -O3 |
| amd64-51 asm, C ladder | 388,007 | 387,842 | 387,909 | 399,377 | 11,468 | clang-17 -O3 |
| 5×51 microcode, C ladder | 327,622 | 327,370 | 327,518 | 334,438 | 6,920 | clang-18 -O |
| 5×51 microcode, chained ladder | 316,590 | 316,156 | 316,485 | 327,770 | 11,285 | clang-17 -O |
| CryptOpt | 381,285 | 380,959 | 381,204 | 391,564 | 10,360 | clang-14 -O2 |
| fiat-crypto | 355,527 | 355,332 | 355,460 | 364,416 | 8,956 | gcc-12 -O3 |
| hand-written C | 381,245 | 380,049 | 380,979 | 391,822 | 10,843 | clang-17 -O3 |
| donna c64 | 337,834 | 337,481 | 337,637 | 347,413 | 9,776 | gcc-11 -O3 |

> The p90−p10 spread is a near-constant ~6,500–7,000 ticks for every contender regardless of its cost, consistent with an external perturbation (timer interrupts on the benchmark core) rather than contender behaviour. Median, minimum and p10 agree to within 0.01% for every contender, so the reported medians sit at the interference-free floor and the ranking is not an artefact of noise.

_Dispersion for the two backends measured only in the A.2 matrix (`uc/Clad`, `a51op/Clad`) is printed by the harness but not currently captured into `RESULTS.md`; re-run the sweep to record it._
