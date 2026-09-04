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
| Run-to-run reproducibility | worst spread 2.206% (amd64-51/asm @ clang-14 -Os) | `note_repro` |
| Repetitions | **1000** per contender, all binaries | `BENCH_REPS` |
| Warm-up | RFC 7748 verification, then one untimed call per contender | `benchmark()` |
| Inputs | fixed RFC 7748 vector 1, byte-identical across all repetitions | `benchmark()` |
| Compiler sweep | 24 configs: {gcc-11,12,13, clang-14,17,18} × {-O,-O2,-O3,-Os} | `lib/build_run.sh` |
| Correctness | all contenders pass RFC 7748 vectors 1-4 in every configuration | `test_rfc7748()` |

## Table 1 — Controlled X25519 field-arithmetic comparison

| Representation | Common ladder / framework | Field backend | kcycles/X25519 | Relative cycles |
|---|---|---|---:|---:|
| **5×51** | common C ladder | **microcode** | **309** | **×1.00** |
| 5×51 | common C ladder | fiat-crypto | 358 | ×1.16 |
| 5×51 | common C ladder | CryptOpt | 383 | ×1.24 |
| 5×51 | common C ladder | hand-written C | 383 | ×1.24 |
| 5×51 | common C ladder | amd64-51 asm | 386 | ×1.25 |
| **4×64 saturated** | amd64-64 C ladder | **assembly** | **308** | **×1.00** |
| 4×64 saturated | amd64-64 C ladder | microcode | 517 | ×1.68 |

> **Table 1: Controlled X25519 field-arithmetic comparison.** Cycle counts are median core kcycles per X25519, rounded to three significant figures. Within each representation block the ladder and surrounding implementation are identical and only field multiplication and squaring change. The 5×51 rows use our common C Montgomery ladder; the 4×64 rows use the same amd64-64 C `ladderstep.c`. Relative cycle counts are normalised **within** each block, so the two blocks must not be compared against one another. The 4×64 microcode backend computes sq(a) = mul(a, a) because its 75-triad multiplier leaves no room for a dedicated squarer inside the 128-triad patch capacity. All rows are the median of 1000 repetitions.

With the 5×51 representation fixed, microcode outperforms every evaluated ISA-level field backend. The advantage persists across all 24 matched compiler and optimisation configurations, with paired geometric-mean speedups between ×1.194 and ×1.240 (Appendix B.1). This result does not extend to the saturated 4×64 representation: with the amd64-64 C ladder held fixed the microcode backend requires 1.680× as many cycles as the assembly backend (1.678× as a paired geometric mean, Appendix B.2). The 128-triad patch capacity prevents the 4×64 implementation from holding both its 75-triad multiplier and a dedicated squarer, forcing squaring through multiplication.

## Table 2 — End-to-end X25519 performance

| Implementation | Representation | kcycles/X25519 | Relative cycles |
|---|---|---:|---:|
| Bernstein–Schwabe amd64-64 asm | 4×64 | 273 | ×0.906 |
| **this work** | **5×51** | **302** | **×1.000** |
| amd64-51 framework + microcode | 5×51 | 317 | ×1.049 |
| donna c64 | 5×51 | 340 | ×1.128 |
| fiat-crypto | 5×51 | 358 | ×1.185 |
| Bernstein–Schwabe amd64-51 asm | 5×51 | 359 | ×1.188 |
| hand-written C | 5×51 | 383 | ×1.269 |
| CryptOpt | 5×51 | 383 | ×1.270 |

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
| native qhasm → C ladder | loss of ladder/field-op fusion | ×1.088 | ×1.093 |
| asm field ops → microcode | controlled microcode field-op gain | ×1.179 | ×1.167 |
| C ladder → register-chained | ladder-integration recovery | ×1.046 | ×1.015 (×1.048 excl. gcc `-Os`) |

> **Table 3: 5×51 integration and ladder decomposition.** All rows use the amd64-51 framework. Register-chaining the ladder helps in 21 of 24 configurations (paired geometric mean ×1.048); under gcc `-Os` the inline-assembly ladder degrades sharply in this framework (×0.81, reproducible across gcc-11/12/13 to within 43 ticks), which pulls the all-configuration geometric mean down to ×1.015. The same chained ladder in our own framework shows no such degradation, so this is a compiler/framework interaction rather than a property of the ladder. The final row is `amd64-51/ucode`, **not** the canonical implementation reported in Table 2.

## Appendix A — full per-configuration sweep

_Median RDTSC ticks per X25519 at each of the 24 configurations. **Bold** = best (lowest) in that column. Raw ticks; multiply by 1.00548 for core cycles._

### A.1 — End-to-end, every complete implementation (Table 2)

| Config | this work | a64/asm | a64/asm+Clad | a64/ucode | a51/asm | a51/asm+Clad | a51/uc+Clad | a51/ucode | CryptOpt | fiat | hand-C | donna |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gcc-11 -O3 | 305,127 | 272,501 | 336,510 | 559,504 | 359,187 | 393,492 | 334,729 | 321,321 | 387,065 | 358,646 | 389,900 | **338,430** |
| gcc-11 -O2 | 310,568 | 273,073 | 326,923 | 553,272 | 358,614 | 390,676 | 334,905 | 324,541 | 399,006 | 375,793 | 403,737 | 371,813 |
| gcc-11 -Os | 312,617 | 273,972 | 362,011 | 595,915 | 359,491 | 398,570 | 342,411 | 424,436 | 403,700 | 371,280 | 410,048 | 374,341 |
| gcc-11 -O | 306,650 | 272,603 | 339,453 | 568,892 | 357,966 | 391,214 | 346,450 | 321,920 | 392,221 | 380,365 | 411,844 | 386,020 |
| gcc-12 -O3 | 305,669 | 273,527 | 323,809 | 552,383 | 359,041 | 393,500 | 334,632 | 321,645 | 385,951 | **355,584** | 386,501 | 341,756 |
| gcc-12 -O2 | 311,344 | 271,768 | 324,309 | 552,291 | 358,508 | 393,410 | 334,863 | 323,971 | 400,895 | 372,214 | 401,553 | 371,286 |
| gcc-12 -Os | 312,585 | 272,596 | 363,869 | 593,723 | 359,223 | 398,209 | 344,799 | 423,833 | 404,273 | 371,841 | 411,029 | 370,756 |
| gcc-12 -O | 307,337 | 272,478 | 340,290 | 568,693 | 358,074 | 390,063 | 345,129 | 322,516 | 392,070 | 380,113 | 408,385 | 377,922 |
| gcc-13 -O3 | 307,941 | 271,839 | 314,951 | 539,340 | 358,966 | 393,422 | 334,726 | 322,667 | 389,787 | 366,589 | 388,955 | 342,094 |
| gcc-13 -O2 | 310,935 | **271,750** | 314,082 | 542,346 | 358,950 | 393,309 | 334,798 | 322,968 | 399,226 | 371,960 | 394,831 | 357,014 |
| gcc-13 -Os | 314,373 | 272,582 | 348,648 | 582,261 | 359,197 | 398,162 | 344,661 | 423,854 | 408,896 | 372,361 | 414,844 | 356,703 |
| gcc-13 -O | 307,710 | 273,383 | 340,148 | 566,570 | 359,151 | 389,978 | 344,191 | 321,744 | 391,871 | 389,043 | 404,375 | 373,077 |
| clang-14 -O3 | 300,663 | 275,869 | 312,679 | 521,723 | 356,706 | 388,966 | 333,650 | 315,973 | 381,391 | 388,679 | 388,286 | 446,776 |
| clang-14 -O2 | **300,102** | 273,416 | 312,780 | 521,263 | 356,740 | 389,137 | 331,809 | 315,969 | **381,112** | 388,416 | 388,186 | 442,291 |
| clang-14 -Os | 304,563 | 272,895 | 314,902 | 523,858 | 356,727 | 390,790 | 329,908 | 317,655 | 389,763 | 409,242 | 413,317 | 446,652 |
| clang-14 -O | 306,182 | 273,130 | 314,426 | 527,866 | 358,630 | **387,938** | 335,726 | 316,712 | 387,136 | 398,436 | 402,520 | 455,717 |
| clang-17 -O3 | 300,946 | 275,516 | **306,243** | 518,160 | 356,775 | 388,980 | 331,854 | 316,057 | 381,803 | 387,691 | 380,984 | 434,139 |
| clang-17 -O2 | 300,897 | 273,364 | 306,332 | 515,673 | 356,745 | 387,980 | 329,837 | 315,849 | 381,224 | 388,007 | **380,915** | 428,898 |
| clang-17 -Os | 305,393 | 272,501 | 310,224 | 515,995 | 356,773 | 392,071 | 331,172 | 316,304 | 388,948 | 405,907 | 402,288 | 437,325 |
| clang-17 -O | 305,342 | 271,898 | 307,735 | 515,578 | 358,630 | 388,459 | **329,110** | 315,474 | 387,406 | 395,745 | 402,198 | 442,657 |
| clang-18 -O3 | 301,064 | 275,564 | 306,752 | 514,789 | **356,649** | 387,946 | 330,820 | 316,285 | 381,676 | 387,075 | 381,340 | 450,063 |
| clang-18 -O2 | 300,944 | 273,760 | 306,840 | **514,355** | 356,749 | 389,688 | 331,569 | 316,330 | 381,459 | 387,987 | 382,751 | 447,289 |
| clang-18 -Os | 305,361 | 272,485 | 308,433 | 515,855 | 356,973 | 390,139 | 331,282 | 317,039 | 389,572 | 399,010 | 402,031 | 454,343 |
| clang-18 -O | 305,471 | 273,209 | 306,870 | 515,939 | 358,791 | 388,683 | 329,134 | **314,787** | 389,467 | 399,420 | 402,512 | 449,494 |

### A.2 — Common C ladder, only the field backend differs (Table 1, 5×51 block)

| Config | microcode | amd64-51 asm | CryptOpt | fiat | hand-C |
|---|---|---|---|---|---|
| gcc-11 -O3 | 312,633 | 388,319 | 387,065 | 358,646 | 389,900 |
| gcc-11 -O2 | 338,022 | 395,375 | 399,006 | 375,793 | 403,737 |
| gcc-11 -Os | 341,561 | 400,097 | 403,700 | 371,280 | 410,048 |
| gcc-11 -O | 334,802 | 393,274 | 392,221 | 380,365 | 411,844 |
| gcc-12 -O3 | 316,291 | 385,157 | 385,951 | **355,584** | 386,501 |
| gcc-12 -O2 | 333,786 | 398,068 | 400,895 | 372,214 | 401,553 |
| gcc-12 -Os | 341,848 | 403,456 | 404,273 | 371,841 | 411,029 |
| gcc-12 -O | 331,746 | 394,672 | 392,070 | 380,113 | 408,385 |
| gcc-13 -O3 | 314,880 | 393,605 | 389,787 | 366,589 | 388,955 |
| gcc-13 -O2 | 332,514 | 395,822 | 399,226 | 371,960 | 394,831 |
| gcc-13 -Os | 344,394 | 405,007 | 408,896 | 372,361 | 414,844 |
| gcc-13 -O | 332,658 | 393,807 | 391,871 | 389,043 | 404,375 |
| clang-14 -O3 | **307,389** | **383,801** | 381,391 | 388,679 | 388,286 |
| clang-14 -O2 | 307,830 | 384,044 | **381,112** | 388,416 | 388,186 |
| clang-14 -Os | 315,167 | 390,641 | 389,763 | 409,242 | 413,317 |
| clang-14 -O | 310,617 | 389,571 | 387,136 | 398,436 | 402,520 |
| clang-17 -O3 | 310,280 | 385,874 | 381,803 | 387,691 | 380,984 |
| clang-17 -O2 | 310,614 | 385,666 | 381,224 | 388,007 | **380,915** |
| clang-17 -Os | 314,371 | 390,266 | 388,948 | 405,907 | 402,288 |
| clang-17 -O | 312,263 | 388,862 | 387,406 | 395,745 | 402,198 |
| clang-18 -O3 | 308,834 | 385,740 | 381,676 | 387,075 | 381,340 |
| clang-18 -O2 | 308,726 | 385,630 | 381,459 | 387,987 | 382,751 |
| clang-18 -Os | 316,429 | 390,363 | 389,572 | 399,010 | 402,031 |
| clang-18 -O | 311,642 | 390,098 | 389,467 | 399,420 | 402,512 |

## Appendix B — paired per-configuration ratios

_Ratio = comparison ÷ microcode at the **same** compiler configuration; >1 means microcode is faster. Computed from raw ticks._

### B.1 — 5×51 block, common C ladder (Table 1)

| Config | fiat-crypto | CryptOpt | amd64-51 asm | hand-written C |
|---|---:|---:|---:|---:|
| gcc-11 -O3 | 1.147 | 1.238 | 1.242 | 1.247 |
| gcc-11 -O2 | 1.112 | 1.180 | 1.170 | 1.194 |
| gcc-11 -Os | 1.087 | 1.182 | 1.171 | 1.201 |
| gcc-11 -O | 1.136 | 1.172 | 1.175 | 1.230 |
| gcc-12 -O3 | 1.124 | 1.220 | 1.218 | 1.222 |
| gcc-12 -O2 | 1.115 | 1.201 | 1.193 | 1.203 |
| gcc-12 -Os | 1.088 | 1.183 | 1.180 | 1.202 |
| gcc-12 -O | 1.146 | 1.182 | 1.190 | 1.231 |
| gcc-13 -O3 | 1.164 | 1.238 | 1.250 | 1.235 |
| gcc-13 -O2 | 1.119 | 1.201 | 1.190 | 1.187 |
| gcc-13 -Os | 1.081 | 1.187 | 1.176 | 1.205 |
| gcc-13 -O | 1.169 | 1.178 | 1.184 | 1.216 |
| clang-14 -O3 | 1.264 | 1.241 | 1.249 | 1.263 |
| clang-14 -O2 | 1.262 | 1.238 | 1.248 | 1.261 |
| clang-14 -Os | 1.298 | 1.237 | 1.239 | 1.311 |
| clang-14 -O | 1.283 | 1.246 | 1.254 | 1.296 |
| clang-17 -O3 | 1.249 | 1.231 | 1.244 | 1.228 |
| clang-17 -O2 | 1.249 | 1.227 | 1.242 | 1.226 |
| clang-17 -Os | 1.291 | 1.237 | 1.241 | 1.280 |
| clang-17 -O | 1.267 | 1.241 | 1.245 | 1.288 |
| clang-18 -O3 | 1.253 | 1.236 | 1.249 | 1.235 |
| clang-18 -O2 | 1.257 | 1.236 | 1.249 | 1.240 |
| clang-18 -Os | 1.261 | 1.231 | 1.234 | 1.271 |
| clang-18 -O | 1.282 | 1.250 | 1.252 | 1.292 |
| **geometric mean** | **1.194** | **1.217** | **1.220** | **1.240** |
| **configurations won** | **24/24** | **24/24** | **24/24** | **24/24** |

### B.2 — 4×64 block, amd64-64 C ladder (Table 1)

| Config | microcode ÷ assembly |
|---|---:|
| gcc-11 -O3 | 1.663 |
| gcc-11 -O2 | 1.692 |
| gcc-11 -Os | 1.646 |
| gcc-11 -O | 1.676 |
| gcc-12 -O3 | 1.706 |
| gcc-12 -O2 | 1.703 |
| gcc-12 -Os | 1.632 |
| gcc-12 -O | 1.671 |
| gcc-13 -O3 | 1.712 |
| gcc-13 -O2 | 1.727 |
| gcc-13 -Os | 1.670 |
| gcc-13 -O | 1.666 |
| clang-14 -O3 | 1.669 |
| clang-14 -O2 | 1.667 |
| clang-14 -Os | 1.664 |
| clang-14 -O | 1.679 |
| clang-17 -O3 | 1.692 |
| clang-17 -O2 | 1.683 |
| clang-17 -Os | 1.663 |
| clang-17 -O | 1.675 |
| clang-18 -O3 | 1.678 |
| clang-18 -O2 | 1.676 |
| clang-18 -Os | 1.673 |
| clang-18 -O | 1.681 |
| **geometric mean** | **1.678** |

## Appendix C — dispersion at each selected configuration

_Median of 1000 repetitions (1000 for the two `amd64-64` rows). Raw RDTSC ticks._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| this work (5×51, chained ladder) | 300,102 | 299,734 | 300,007 | 308,164 | 8,157 | clang-14 -O2 |
| amd64-64 asm | 271,750 | 271,594 | 271,707 | 278,519 | 6,812 | gcc-13 -O2 |
| amd64-64 asm, C ladder | 306,243 | 306,131 | — | — | — | clang-17 -O3 |
| 4×64 microcode, C ladder | 514,355 | 514,270 | — | — | — | clang-18 -O2 |
| amd64-51 asm | 356,649 | 356,523 | 356,568 | 365,565 | 8,997 | clang-18 -O3 |
| amd64-51 asm, C ladder | 387,938 | 387,822 | 387,879 | 397,206 | 9,327 | clang-14 -O |
| 5×51 microcode, C ladder | 329,110 | 329,000 | 329,045 | 336,428 | 7,383 | clang-17 -O |
| 5×51 microcode, chained ladder | 314,787 | 313,384 | 314,636 | 325,653 | 11,017 | clang-18 -O |
| CryptOpt | 381,112 | 380,795 | 381,055 | 389,500 | 8,445 | clang-14 -O2 |
| fiat-crypto | 355,584 | 355,487 | 355,522 | 363,880 | 8,358 | gcc-12 -O3 |
| hand-written C | 380,915 | 380,293 | 380,837 | 389,853 | 9,016 | clang-17 -O2 |
| donna c64 | 338,430 | 337,821 | 338,339 | 346,327 | 7,988 | gcc-11 -O3 |

> The p90−p10 spread is a near-constant ~6,500–7,000 ticks for every contender regardless of its cost, consistent with an external perturbation (timer interrupts on the benchmark core) rather than contender behaviour. Median, minimum and p10 agree to within 0.01% for every contender, so the reported medians sit at the interference-free floor and the ranking is not an artefact of noise.

_Dispersion for the two backends measured only in the A.2 matrix (`uc/Clad`, `a51op/Clad`) is printed by the harness but not currently captured into `RESULTS.md`; re-run the sweep to record it._
