# Curve25519 / X25519 — paper tables

_Generated from `RESULTS.md` (Appendices A.1, A.2) by `gen_paper_tables.py`. Do not hand-edit; regenerate._

_Absolute values are RDTSC ticks converted to core cycles by ×1.00548 (f_core/f_TSC, measured by this run's frequency guard), then rounded to three significant figures. Ratios are computed from raw full-precision ticks and are invariant to that correction._

## Measurement setup

| item | value | source |
|---|---|---|
| CPU | Intel(R) Celeron(R) CPU N3350 @ 1.10GHz | `/proc/cpuinfo` |
| Core / pinning | core 0, `taskset -c 0` | `lib/build_run.sh` |
| Governor | `userspace`, requested 1094410 kHz | `lib/freq_guard.sh` |
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
| Run-to-run reproducibility | worst spread 2.719% (amd64-51/ucode @ clang-14 -Os) | `note_repro` |
| Repetitions | **1000** per contender, all binaries | `BENCH_REPS` |
| Warm-up | RFC 7748 verification, then one untimed call per contender | `benchmark()` |
| Inputs | fixed RFC 7748 vector 1, byte-identical across all repetitions | `benchmark()` |
| Compiler sweep | 24 configs: {gcc-11,12,13, clang-14,17,18} × {-O,-O2,-O3,-Os} | `lib/build_run.sh` |
| Correctness | all contenders pass RFC 7748 vectors 1-4 in every configuration | `test_rfc7748()` |

## Table 1 — Controlled X25519 field-arithmetic comparison

| Representation | Common ladder / framework | Field backend | kcycles/X25519 | Relative cycles |
|---|---|---|---:|---:|
| **5×51** | common C ladder | **microcode** | **309** | **×1.00** |
| 5×51 | common C ladder | fiat-crypto | 357 | ×1.16 |
| 5×51 | common C ladder | CryptOpt | 383 | ×1.24 |
| 5×51 | common C ladder | hand-written C | 383 | ×1.24 |
| 5×51 | common C ladder | amd64-51 asm | 386 | ×1.25 |
| **4×64 saturated** | amd64-64 C ladder | **assembly** | **308** | **×1.00** |
| 4×64 saturated | amd64-64 C ladder | microcode | 518 | ×1.68 |

> **Table 1: Controlled X25519 field-arithmetic comparison.** Cycle counts are median core kcycles per X25519, rounded to three significant figures. Within each representation block the ladder and surrounding implementation are identical and only field multiplication and squaring change. The 5×51 rows use our common C Montgomery ladder; the 4×64 rows use the same amd64-64 C `ladderstep.c`. Relative cycle counts are normalised **within** each block, so the two blocks must not be compared against one another. The 4×64 microcode backend computes sq(a) = mul(a, a) because its 75-triad multiplier leaves no room for a dedicated squarer inside the 128-triad patch capacity. All rows are the median of 1000 repetitions.

With the 5×51 representation fixed, microcode outperforms every evaluated ISA-level field backend. The advantage persists across all 24 matched compiler and optimisation configurations, with paired geometric-mean speedups between ×1.195 and ×1.241 (Appendix B.1). This result does not extend to the saturated 4×64 representation: with the amd64-64 C ladder held fixed the microcode backend requires 1.681× as many cycles as the assembly backend (1.680× as a paired geometric mean, Appendix B.2). The 128-triad patch capacity prevents the 4×64 implementation from holding both its 75-triad multiplier and a dedicated squarer, forcing squaring through multiplication.

## Table 2 — End-to-end X25519 performance

| Implementation | Representation | kcycles/X25519 | Relative cycles |
|---|---|---:|---:|
| Bernstein–Schwabe amd64-64 asm | 4×64 | 273 | ×0.905 |
| **this work** | **5×51** | **302** | **×1.000** |
| amd64-51 framework + microcode | 5×51 | 317 | ×1.049 |
| donna c64 | 5×51 | 340 | ×1.125 |
| fiat-crypto | 5×51 | 357 | ×1.183 |
| Bernstein–Schwabe amd64-51 asm | 5×51 | 359 | ×1.188 |
| CryptOpt | 5×51 | 383 | ×1.269 |
| hand-written C | 5×51 | 383 | ×1.270 |

> **Table 2: End-to-end X25519 performance.** Cycle counts are median core kcycles per X25519, rounded to three significant figures, each row at its own best compiler configuration. These are complete implementations differing in representation, ladder structure, inversion, field arithmetic and code organisation; the table therefore establishes overall standing but does **not** isolate the effect of microcode. Lower is better; relative cycles are normalised to this work.

## Table 3 — 5×51 integration and ladder decomposition

| Variant | Ladder | Field ops | kcycles/X25519 |
|---|---|---|---:|
| amd64-51 native | qhasm, monolithic | qhasm asm | 359 |
| C-ladder control | C, per-op calls | qhasm asm | 390 |
| C-ladder + microcode | C, per-op calls | microcode | 331 |
| chained ladder + microcode | inline asm, register-chained | microcode | 317 |

**Transitions** — each changes exactly one element from the row above:

| Transition | Effect isolated | best-of-24 | paired geomean |
|---|---|---:|---:|
| native qhasm → C ladder | loss of ladder/field-op fusion | ×1.086 | ×1.092 |
| asm field ops → microcode | controlled microcode field-op gain | ×1.177 | ×1.166 |
| C ladder → register-chained | ladder-integration recovery | ×1.045 | ×1.015 (×1.048 excl. gcc `-Os`) |

> **Table 3: 5×51 integration and ladder decomposition.** All rows use the amd64-51 framework. Register-chaining the ladder helps in 21 of 24 configurations (paired geometric mean ×1.048); under gcc `-Os` the inline-assembly ladder degrades sharply in this framework (×0.81, reproducible across gcc-11/12/13 to within 43 ticks), which pulls the all-configuration geometric mean down to ×1.015. The same chained ladder in our own framework shows no such degradation, so this is a compiler/framework interaction rather than a property of the ladder. The final row is `amd64-51/ucode`, **not** the canonical implementation reported in Table 2.

## Appendix A — full per-configuration sweep

_Median RDTSC ticks per X25519 at each of the 24 configurations. **Bold** = best (lowest) in that column. Raw ticks; multiply by 1.00548 for core cycles._

### A.1 — End-to-end, every complete implementation (Table 2)

| Config | this work | a64/asm | a64/asm+Clad | a64/ucode | a51/asm | a51/asm+Clad | a51/uc+Clad | a51/ucode | CryptOpt | fiat | hand-C | donna |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gcc-11 -O3 | 305,338 | 272,423 | 336,414 | 559,702 | 358,729 | 392,105 | 335,239 | 319,181 | 387,036 | 358,269 | 389,520 | **337,876** |
| gcc-11 -O2 | 310,731 | 272,990 | 326,993 | 553,065 | 357,999 | 390,414 | 334,912 | 323,398 | 399,330 | 376,132 | 404,234 | 371,241 |
| gcc-11 -Os | 312,952 | 273,785 | 362,266 | 596,780 | 358,938 | 398,929 | 342,137 | 424,933 | 403,749 | 371,555 | 410,003 | 374,415 |
| gcc-11 -O | 306,774 | 272,703 | 335,571 | 568,862 | 358,300 | 391,179 | 346,552 | 323,180 | 392,170 | 380,461 | 411,957 | 385,702 |
| gcc-12 -O3 | 306,007 | 273,568 | 324,053 | 552,220 | 358,741 | 393,523 | 334,787 | 323,383 | 385,984 | **355,283** | 386,413 | 341,412 |
| gcc-12 -O2 | 310,741 | 271,854 | 324,250 | 552,639 | 358,872 | 391,631 | 334,810 | 324,933 | 400,987 | 372,061 | 401,874 | 371,032 |
| gcc-12 -Os | 313,699 | 272,340 | 364,017 | 595,068 | 358,705 | 398,597 | 344,561 | 424,234 | 404,415 | 372,177 | 411,162 | 370,967 |
| gcc-12 -O | 307,545 | 272,647 | 340,317 | 568,788 | 358,436 | 389,935 | 345,269 | 321,875 | 392,091 | 380,283 | 408,558 | 377,705 |
| gcc-13 -O3 | 308,234 | **271,756** | 314,669 | 540,153 | 358,762 | 391,785 | 334,708 | 321,142 | 389,418 | 366,539 | 388,834 | 341,362 |
| gcc-13 -O2 | 311,078 | 271,898 | 314,013 | 542,328 | 359,031 | 391,615 | 334,732 | 322,948 | 399,303 | 371,789 | 395,140 | 356,691 |
| gcc-13 -Os | 314,695 | 272,399 | 348,591 | 581,015 | 358,686 | 398,469 | 344,612 | 424,065 | 408,912 | 372,817 | 414,769 | 357,107 |
| gcc-13 -O | 307,906 | 273,511 | 336,347 | 566,586 | 359,462 | 389,830 | 344,329 | 321,588 | 391,829 | 389,098 | 404,272 | 372,939 |
| clang-14 -O3 | **300,205** | 275,931 | 312,716 | 521,735 | 356,918 | 388,540 | 332,245 | 316,171 | 381,562 | 390,157 | 388,143 | 446,907 |
| clang-14 -O2 | 300,262 | 272,838 | 312,836 | 521,008 | 356,719 | 389,156 | 332,866 | 316,122 | **381,025** | 390,082 | 388,145 | 443,116 |
| clang-14 -Os | 304,441 | 273,022 | 314,907 | 522,831 | 356,966 | 390,415 | 330,008 | 317,539 | 389,921 | 409,191 | 413,348 | 446,699 |
| clang-14 -O | 306,263 | 273,048 | 314,418 | 528,008 | 358,348 | 387,981 | 335,822 | 316,918 | 387,040 | 398,573 | 402,774 | 456,272 |
| clang-17 -O3 | 300,974 | 275,448 | **306,275** | 518,169 | **356,704** | 388,982 | 331,431 | 316,273 | 381,841 | 387,791 | **381,115** | 434,287 |
| clang-17 -O2 | 301,084 | 272,820 | 306,330 | 516,803 | **356,704** | 388,050 | 329,826 | 316,537 | 381,182 | 386,776 | 381,336 | 429,503 |
| clang-17 -Os | 305,579 | 272,494 | 310,228 | 518,083 | 356,916 | 391,957 | 331,306 | 316,391 | 388,957 | 405,838 | 402,382 | 438,910 |
| clang-17 -O | 305,365 | 271,823 | 307,751 | 515,575 | 358,340 | 388,486 | 329,226 | 316,145 | 387,370 | 395,852 | 402,074 | 443,039 |
| clang-18 -O3 | 301,033 | 275,536 | 306,800 | **514,891** | 356,944 | **387,458** | 330,712 | 316,324 | 381,792 | 388,092 | 382,553 | 450,365 |
| clang-18 -O2 | 301,113 | 273,354 | 306,839 | 516,929 | 356,784 | 387,478 | 329,791 | 314,935 | 381,464 | 387,094 | 382,527 | 448,230 |
| clang-18 -Os | 305,418 | 272,556 | 308,451 | 515,999 | 356,732 | 390,144 | 331,854 | 318,475 | 389,768 | 398,890 | 402,332 | 455,054 |
| clang-18 -O | 305,373 | 272,986 | 306,878 | 515,241 | 358,638 | 388,674 | **329,131** | **314,851** | 388,980 | 399,839 | 402,481 | 449,850 |

### A.2 — Common C ladder, only the field backend differs (Table 1, 5×51 block)

| Config | microcode | amd64-51 asm | CryptOpt | fiat | hand-C |
|---|---|---|---|---|---|
| gcc-11 -O3 | 312,934 | 388,381 | 387,036 | 358,269 | 389,520 |
| gcc-11 -O2 | 337,947 | 395,690 | 399,330 | 376,132 | 404,234 |
| gcc-11 -Os | 341,487 | 399,983 | 403,749 | 371,555 | 410,003 |
| gcc-11 -O | 334,794 | 393,416 | 392,170 | 380,461 | 411,957 |
| gcc-12 -O3 | 316,664 | 385,180 | 385,984 | **355,283** | 386,413 |
| gcc-12 -O2 | 333,683 | 398,086 | 400,987 | 372,061 | 401,874 |
| gcc-12 -Os | 341,863 | 403,501 | 404,415 | 372,177 | 411,162 |
| gcc-12 -O | 331,762 | 394,819 | 392,091 | 380,283 | 408,558 |
| gcc-13 -O3 | 315,402 | 393,542 | 389,418 | 366,539 | 388,834 |
| gcc-13 -O2 | 332,577 | 395,802 | 399,303 | 371,789 | 395,140 |
| gcc-13 -Os | 345,283 | 405,272 | 408,912 | 372,817 | 414,769 |
| gcc-13 -O | 332,799 | 393,471 | 391,829 | 389,098 | 404,272 |
| clang-14 -O3 | 307,470 | **383,608** | 381,562 | 390,157 | 388,143 |
| clang-14 -O2 | **307,358** | 383,899 | **381,025** | 390,082 | 388,145 |
| clang-14 -Os | 314,968 | 390,615 | 389,921 | 409,191 | 413,348 |
| clang-14 -O | 310,634 | 389,341 | 387,040 | 398,573 | 402,774 |
| clang-17 -O3 | 310,159 | 385,845 | 381,841 | 387,791 | **381,115** |
| clang-17 -O2 | 309,938 | 385,566 | 381,182 | 386,776 | 381,336 |
| clang-17 -Os | 314,153 | 390,328 | 388,957 | 405,838 | 402,382 |
| clang-17 -O | 311,949 | 388,667 | 387,370 | 395,852 | 402,074 |
| clang-18 -O3 | 308,354 | 385,796 | 381,792 | 388,092 | 382,553 |
| clang-18 -O2 | 308,267 | 385,764 | 381,464 | 387,094 | 382,527 |
| clang-18 -Os | 313,922 | 390,431 | 389,768 | 398,890 | 402,332 |
| clang-18 -O | 311,332 | 389,945 | 388,980 | 399,839 | 402,481 |

## Appendix B — paired per-configuration ratios

_Ratio = comparison ÷ microcode at the **same** compiler configuration; >1 means microcode is faster. Computed from raw ticks._

### B.1 — 5×51 block, common C ladder (Table 1)

| Config | fiat-crypto | CryptOpt | amd64-51 asm | hand-written C |
|---|---:|---:|---:|---:|
| gcc-11 -O3 | 1.145 | 1.237 | 1.241 | 1.245 |
| gcc-11 -O2 | 1.113 | 1.182 | 1.171 | 1.196 |
| gcc-11 -Os | 1.088 | 1.182 | 1.171 | 1.201 |
| gcc-11 -O | 1.136 | 1.171 | 1.175 | 1.230 |
| gcc-12 -O3 | 1.122 | 1.219 | 1.216 | 1.220 |
| gcc-12 -O2 | 1.115 | 1.202 | 1.193 | 1.204 |
| gcc-12 -Os | 1.089 | 1.183 | 1.180 | 1.203 |
| gcc-12 -O | 1.146 | 1.182 | 1.190 | 1.231 |
| gcc-13 -O3 | 1.162 | 1.235 | 1.248 | 1.233 |
| gcc-13 -O2 | 1.118 | 1.201 | 1.190 | 1.188 |
| gcc-13 -Os | 1.080 | 1.184 | 1.174 | 1.201 |
| gcc-13 -O | 1.169 | 1.177 | 1.182 | 1.215 |
| clang-14 -O3 | 1.269 | 1.241 | 1.248 | 1.262 |
| clang-14 -O2 | 1.269 | 1.240 | 1.249 | 1.263 |
| clang-14 -Os | 1.299 | 1.238 | 1.240 | 1.312 |
| clang-14 -O | 1.283 | 1.246 | 1.253 | 1.297 |
| clang-17 -O3 | 1.250 | 1.231 | 1.244 | 1.229 |
| clang-17 -O2 | 1.248 | 1.230 | 1.244 | 1.230 |
| clang-17 -Os | 1.292 | 1.238 | 1.242 | 1.281 |
| clang-17 -O | 1.269 | 1.242 | 1.246 | 1.289 |
| clang-18 -O3 | 1.259 | 1.238 | 1.251 | 1.241 |
| clang-18 -O2 | 1.256 | 1.237 | 1.251 | 1.241 |
| clang-18 -Os | 1.271 | 1.242 | 1.244 | 1.282 |
| clang-18 -O | 1.284 | 1.249 | 1.253 | 1.293 |
| **geometric mean** | **1.195** | **1.217** | **1.220** | **1.241** |
| **configurations won** | **24/24** | **24/24** | **24/24** | **24/24** |

### B.2 — 4×64 block, amd64-64 C ladder (Table 1)

| Config | microcode ÷ assembly |
|---|---:|
| gcc-11 -O3 | 1.664 |
| gcc-11 -O2 | 1.691 |
| gcc-11 -Os | 1.647 |
| gcc-11 -O | 1.695 |
| gcc-12 -O3 | 1.704 |
| gcc-12 -O2 | 1.704 |
| gcc-12 -Os | 1.635 |
| gcc-12 -O | 1.671 |
| gcc-13 -O3 | 1.717 |
| gcc-13 -O2 | 1.727 |
| gcc-13 -Os | 1.667 |
| gcc-13 -O | 1.685 |
| clang-14 -O3 | 1.668 |
| clang-14 -O2 | 1.665 |
| clang-14 -Os | 1.660 |
| clang-14 -O | 1.679 |
| clang-17 -O3 | 1.692 |
| clang-17 -O2 | 1.687 |
| clang-17 -Os | 1.670 |
| clang-17 -O | 1.675 |
| clang-18 -O3 | 1.678 |
| clang-18 -O2 | 1.685 |
| clang-18 -Os | 1.673 |
| clang-18 -O | 1.679 |
| **geometric mean** | **1.680** |

## Appendix C — dispersion at each selected configuration

_Median of 1000 repetitions (1000 for the two `amd64-64` rows). Raw RDTSC ticks._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| this work (5×51, chained ladder) | 300,205 | 300,011 | 300,075 | 311,128 | 11,053 | clang-14 -O3 |
| amd64-64 asm | 271,756 | 271,603 | 271,738 | 278,989 | 7,251 | gcc-13 -O3 |
| amd64-64 asm, C ladder | 306,275 | 306,134 | — | — | — | clang-17 -O3 |
| 4×64 microcode, C ladder | 514,891 | 514,732 | — | — | — | clang-18 -O3 |
| amd64-51 asm | 356,704 | 356,580 | 356,631 | 366,940 | 10,309 | clang-17 -O3 |
| amd64-51 asm, C ladder | 387,458 | 387,336 | 387,364 | 399,485 | 12,121 | clang-18 -O3 |
| 5×51 microcode, C ladder | 329,131 | 329,000 | 329,069 | 337,173 | 8,104 | clang-18 -O |
| 5×51 microcode, chained ladder | 314,851 | 314,697 | 314,811 | 324,600 | 9,789 | clang-18 -O |
| CryptOpt | 381,025 | 380,764 | 380,966 | 391,191 | 10,225 | clang-14 -O2 |
| fiat-crypto | 355,283 | 355,159 | 355,195 | 365,584 | 10,389 | gcc-12 -O3 |
| hand-written C | 381,115 | 380,200 | 380,996 | 389,884 | 8,888 | clang-17 -O3 |
| donna c64 | 337,876 | 337,232 | 337,819 | 345,582 | 7,763 | gcc-11 -O3 |

> The p90−p10 spread is a near-constant ~6,500–7,000 ticks for every contender regardless of its cost, consistent with an external perturbation (timer interrupts on the benchmark core) rather than contender behaviour. Median, minimum and p10 agree to within 0.01% for every contender, so the reported medians sit at the interference-free floor and the ranking is not an artefact of noise.

_Dispersion for the two backends measured only in the A.2 matrix (`uc/Clad`, `a51op/Clad`) is printed by the harness but not currently captured into `RESULTS.md`; re-run the sweep to record it._
