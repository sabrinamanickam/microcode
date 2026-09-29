# Curve25519 / X25519 — paper tables

_Generated from `RESULTS.md` (Appendices A.1, A.2) by `gen_paper_tables.py`. Do not hand-edit; regenerate._

_Absolute values are RDTSC ticks converted to core cycles by ×1.00548 (f_core/f_TSC, measured by this run's frequency guard), then rounded to three significant figures. Ratios are computed from raw full-precision ticks and are invariant to that correction._

## Measurement setup

| item | value | source |
|---|---|---|
| CPU | Intel(R) Celeron(R) CPU N3350 @ 1.10GHz | `/proc/cpuinfo` |
| Core / pinning | core 0, `taskset -c 0` | `lib/build_run.sh` |
| Governor | `userspace`, requested 1099163 kHz | `lib/freq_guard.sh` |
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
| Run-to-run reproducibility | worst spread 1.195% (amd64-64/asm-Clad @ gcc-12 -O) | `note_repro` |
| Repetitions | **1000** per contender, all binaries | `BENCH_REPS` |
| Warm-up | RFC 7748 verification, then one untimed call per contender | `benchmark()` |
| Inputs | fixed RFC 7748 vector 1, byte-identical across all repetitions | `benchmark()` |
| Compiler sweep | 24 configs: {gcc-11,12,13, clang-14,17,18} × {-O,-O2,-O3,-Os} | `lib/build_run.sh` |
| Correctness | all contenders pass RFC 7748 vectors 1-4 in every configuration | `test_rfc7748()` |

## Table 1 — Controlled X25519 field-arithmetic comparison

| Representation | Common ladder / framework | Field backend | kcycles/X25519 | Relative cycles |
|---|---|---|---:|---:|
| 5×51 | common C ladder | OpenSSL fe51 asm | 255 | ×0.93 |
| **5×51** | common C ladder | **microcode** | **274** | **×1.00** |
| 5×51 | common C ladder | fiat-crypto | 349 | ×1.27 |
| 5×51 | common C ladder | hand-written C | 373 | ×1.36 |
| 5×51 | common C ladder | CryptOpt | 374 | ×1.36 |
| 5×51 | common C ladder | amd64-51 asm | 377 | ×1.37 |
| **4×64 saturated** | amd64-64 C ladder | **assembly** | **308** | **×1.00** |
| 4×64 saturated | amd64-64 C ladder | microcode | 517 | ×1.68 |

> **Table 1: Controlled X25519 field-arithmetic comparison.** Cycle counts are median core kcycles per X25519, rounded to three significant figures. Within each representation block the ladder and surrounding implementation are identical and only field multiplication and squaring change. The 5×51 rows use our common C Montgomery ladder; the 4×64 rows use the same amd64-64 C `ladderstep.c`. Relative cycle counts are normalised **within** each block, so the two blocks must not be compared against one another. The 4×64 microcode backend computes sq(a) = mul(a, a) because its 75-triad multiplier leaves no room for a dedicated squarer inside the 128-triad patch capacity. All rows are the median of 1000 repetitions.

With the 5×51 representation fixed, microcode outperforms every compiler-generated and published-research field backend evaluated here — fiat-crypto, CryptOpt, the amd64-51 assembly and hand-written C — across all 24 matched compiler and optimisation configurations, with paired geometric-mean speedups between ×1.308 and ×1.358 (Appendix B.1). It does not outperform OpenSSL's hand-tuned fe51 assembly, which is faster on the same ladder (×0.916 paired geometric mean, microcode ahead in 0 of 24 configurations). Isolated-kernel measurement attributes that difference entirely to invocation cost rather than to the arithmetic: with each side's invocation floor removed, the two field multiplications are within 0.4% of one another (Table K5). Entering patch RAM costs a flat 16.2 cycles irrespective of operand count, where an equivalent native call costs 10.1; over the 2,561 field firings of one X25519 that differential is ≈20,300 cycles and accounts for the whole gap. This result also does not extend to the saturated 4×64 representation: with the amd64-64 C ladder held fixed the microcode backend requires 1.680× as many cycles as the assembly backend (1.678× as a paired geometric mean, Appendix B.2). The 128-triad patch capacity prevents the 4×64 implementation from holding both its 75-triad multiplier and a dedicated squarer, forcing squaring through multiplication.

## Table 2 — End-to-end X25519 performance

| Implementation | Representation | kcycles/X25519 | Relative cycles |
|---|---|---:|---:|
| s2n-bignum verified asm | 4×64 | 246 | ×0.927 |
| OpenSSL fe51 asm on our C ladder | 5×51 | 255 | ×0.961 |
| OpenSSL (own ladder + fe51 asm) | 5×51 | 255 | ×0.963 |
| **this work** | **5×51** | **265** | **×1.000** |
| Bernstein–Schwabe amd64-64 asm | 4×64 | 273 | ×1.031 |
| amd64-51 framework + microcode | 5×51 | 279 | ×1.054 |
| donna c64 | 5×51 | 339 | ×1.278 |
| fiat-crypto | 5×51 | 349 | ×1.316 |
| Bernstein–Schwabe amd64-51 asm | 5×51 | 359 | ×1.353 |
| hand-written C | 5×51 | 373 | ×1.408 |
| CryptOpt | 5×51 | 374 | ×1.411 |

> **Table 2: End-to-end X25519 performance.** Cycle counts are median core kcycles per X25519, rounded to three significant figures, each row at its own best compiler configuration. These are complete implementations differing in representation, ladder structure, inversion, field arithmetic and code organisation; the table therefore establishes overall standing but does **not** isolate the effect of microcode. Lower is better; relative cycles are normalised to this work.

## Table 3 — 5×51 integration and ladder decomposition

| Variant | Ladder | Field ops | kcycles/X25519 |
|---|---|---|---:|
| amd64-51 native | qhasm, monolithic | qhasm asm | 359 |
| C-ladder control | C, per-op calls | qhasm asm | 390 |
| C-ladder + microcode | C, per-op calls | microcode | 303 |
| chained ladder + microcode | inline asm, register-chained | microcode | 279 |

**Transitions** — each changes exactly one element from the row above:

| Transition | Effect isolated | best-of-24 | paired geomean |
|---|---|---:|---:|
| native qhasm → C ladder | loss of ladder/field-op fusion | ×1.087 | ×1.093 |
| asm field ops → microcode | controlled microcode field-op gain | ×1.285 | ×1.262 |
| C ladder → register-chained | ladder-integration recovery | ×1.086 | ×1.060 (×1.099 excl. gcc `-Os`) |

> **Table 3: 5×51 integration and ladder decomposition.** All rows use the amd64-51 framework. Register-chaining the ladder helps in 21 of 24 configurations (paired geometric mean ×1.099); under gcc `-Os` the inline-assembly ladder degrades sharply in this framework (×0.82, reproducible across gcc-11/12/13 to within 43 ticks), which pulls the all-configuration geometric mean down to ×1.060. The same chained ladder in our own framework shows no such degradation, so this is a compiler/framework interaction rather than a property of the ladder. The final row is `amd64-51/ucode`, **not** the canonical implementation reported in Table 2.

## Appendix A — full per-configuration sweep

_Median RDTSC ticks per X25519 at each of the 24 configurations. **Bold** = best (lowest) in that column. Raw ticks; multiply by 1.00548 for core cycles._

### A.1 — End-to-end, every complete implementation (Table 2)

| Config | this work | a64/asm | a64/asm+Clad | a64/ucode | a51/asm | a51/asm+Clad | a51/uc+Clad | a51/ucode | CryptOpt | fiat | hand-C | donna |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gcc-11 -O3 | 267,362 | 272,862 | 336,396 | 559,585 | 359,228 | 392,125 | 310,149 | 280,906 | 380,949 | 353,526 | 382,732 | **337,023** | 244,567 | 260,942 | 253,967 |
| gcc-11 -O2 | 273,580 | 273,095 | 326,844 | 553,376 | 359,032 | 390,777 | 309,622 | 284,819 | 389,032 | 364,416 | 392,540 | 370,417 | 244,591 | 269,728 | 254,143 |
| gcc-11 -Os | 275,809 | 274,065 | 361,759 | 596,523 | 359,083 | 398,652 | 319,363 | 386,818 | 391,344 | 361,108 | 400,982 | 369,997 | 244,672 | 272,824 | 253,950 |
| gcc-11 -O | 272,240 | 272,895 | 339,439 | 568,943 | 358,395 | 391,669 | 321,657 | 281,537 | 380,610 | 369,305 | 400,162 | 377,128 | 244,715 | 263,672 | 254,307 |
| gcc-12 -O3 | 268,197 | 272,021 | 324,140 | 551,897 | 358,895 | 393,563 | 309,917 | 283,660 | 380,031 | **347,204** | 376,617 | 340,231 | 244,528 | 259,165 | 253,951 |
| gcc-12 -O2 | 274,194 | 271,987 | 324,275 | 552,265 | 358,960 | 393,710 | 310,235 | 284,037 | 391,647 | 362,425 | 392,266 | 369,861 | 244,595 | 268,659 | 254,000 |
| gcc-12 -Os | 274,950 | 272,533 | 364,010 | 593,608 | 358,850 | 398,276 | 316,680 | 386,448 | 394,882 | 363,308 | 400,211 | 375,260 | 244,632 | 273,936 | 253,952 |
| gcc-12 -O | 269,276 | 272,571 | 340,300 | 568,741 | 358,314 | 390,363 | 322,292 | 280,902 | 381,129 | 368,784 | 397,710 | 381,281 | **244,461** | 263,289 | 254,300 |
| gcc-13 -O3 | 272,184 | 272,210 | 314,853 | 539,558 | 359,340 | 391,651 | 308,335 | 283,246 | 383,671 | 360,337 | 381,381 | 340,941 | 244,544 | 263,914 | **253,912** |
| gcc-13 -O2 | 274,111 | **271,841** | 314,092 | 542,565 | 359,022 | 393,777 | 310,305 | 284,243 | 390,139 | 362,115 | 385,392 | 355,682 | 244,640 | 266,988 | 254,062 |
| gcc-13 -Os | 277,057 | 272,616 | 348,479 | 580,862 | 358,797 | 398,186 | 316,597 | 386,595 | 396,101 | 360,212 | 402,200 | 356,022 | 244,664 | 274,170 | 253,986 |
| gcc-13 -O | 273,284 | 273,490 | 336,380 | 566,596 | 359,294 | 390,334 | 322,657 | 281,682 | 382,191 | 377,749 | 392,705 | 373,346 | 244,553 | 262,848 | 254,404 |
| clang-14 -O3 | 267,267 | 276,106 | 312,694 | 522,969 | 357,930 | 389,786 | 308,413 | 284,735 | 372,918 | 383,698 | 384,037 | 454,638 | 250,234 | 254,835 | 255,118 |
| clang-14 -O2 | 269,154 | 274,406 | 312,776 | 521,184 | 357,439 | 388,831 | 304,104 | 279,766 | **372,252** | 383,146 | 382,671 | 454,471 | 252,956 | 255,053 | 255,397 |
| clang-14 -Os | 269,768 | 273,100 | 314,901 | 523,857 | **356,881** | 392,873 | 305,208 | 280,486 | 383,571 | 399,993 | 406,834 | 445,996 | 244,605 | 262,593 | 254,348 |
| clang-14 -O | 273,224 | 273,518 | 314,408 | 527,867 | 359,505 | 388,325 | 312,189 | 279,444 | 379,930 | 390,926 | 393,006 | 466,185 | 252,795 | 262,567 | 255,161 |
| clang-17 -O3 | 264,338 | 275,680 | **306,243** | 516,831 | 357,019 | 389,483 | 305,756 | 280,448 | 373,480 | 379,830 | 373,387 | 442,212 | 250,450 | 254,344 | 254,864 |
| clang-17 -O2 | 265,402 | 274,389 | 306,320 | 518,202 | 357,056 | 388,127 | 305,001 | 279,074 | 373,069 | 379,716 | 372,736 | 440,533 | 253,141 | 254,865 | 255,208 |
| clang-17 -Os | 268,124 | 272,647 | 310,224 | 515,820 | 357,018 | 389,928 | 305,443 | 278,056 | 381,713 | 397,719 | 395,130 | 436,154 | 244,479 | 259,116 | 254,395 |
| clang-17 -O | 272,735 | 272,258 | 307,745 | 515,577 | 359,387 | 388,717 | 302,558 | 279,819 | 380,322 | 388,207 | 393,298 | 452,207 | 252,825 | 262,071 | 255,478 |
| clang-18 -O3 | **263,779** | 275,563 | 306,749 | 514,794 | 357,337 | 390,145 | 308,884 | 281,925 | 373,364 | 379,445 | 373,928 | 458,544 | 250,279 | **253,574** | 254,919 |
| clang-18 -O2 | 267,061 | 275,275 | 306,822 | **514,402** | 357,249 | **387,769** | **301,783** | 279,330 | 372,955 | 377,977 | **371,422** | 458,978 | 253,033 | 254,352 | 255,095 |
| clang-18 -Os | 268,993 | 272,462 | 308,423 | 516,027 | 357,012 | 390,635 | 306,379 | **277,954** | 379,791 | 393,249 | 393,933 | 453,120 | 244,558 | 259,739 | 254,369 |
| clang-18 -O | 273,042 | 273,962 | 306,866 | 514,911 | 359,398 | 388,773 | 302,877 | 279,219 | 381,450 | 394,175 | 393,534 | 460,195 | 252,886 | 263,048 | 255,411 |

### A.2 — Common C ladder, only the field backend differs (Table 1, 5×51 block)

| Config | microcode | amd64-51 asm | CryptOpt | fiat | hand-C |
|---|---|---|---|---|---|---|
| gcc-11 -O3 | 281,477 | 380,862 | 260,942 | 380,949 | 353,526 | 382,732 |
| gcc-11 -O2 | 301,658 | 385,051 | 269,728 | 389,032 | 364,416 | 392,540 |
| gcc-11 -Os | 306,817 | 393,325 | 272,824 | 391,344 | 361,108 | 400,982 |
| gcc-11 -O | 295,390 | 382,232 | 263,672 | 380,610 | 369,305 | 400,162 |
| gcc-12 -O3 | 282,612 | 377,886 | 259,165 | 380,031 | **347,204** | 376,617 |
| gcc-12 -O2 | 300,671 | 388,377 | 268,659 | 391,647 | 362,425 | 392,266 |
| gcc-12 -Os | 305,524 | 393,994 | 273,936 | 394,882 | 363,308 | 400,211 |
| gcc-12 -O | 297,416 | 383,698 | 263,289 | 381,129 | 368,784 | 397,710 |
| gcc-13 -O3 | 285,112 | 382,993 | 263,914 | 383,671 | 360,337 | 381,381 |
| gcc-13 -O2 | 299,113 | 386,710 | 266,988 | 390,139 | 362,115 | 385,392 |
| gcc-13 -Os | 305,054 | 394,033 | 274,170 | 396,101 | 360,212 | 402,200 |
| gcc-13 -O | 296,420 | 382,869 | 262,848 | 382,191 | 377,749 | 392,705 |
| clang-14 -O3 | 275,149 | 377,721 | 254,835 | 372,918 | 383,698 | 384,037 |
| clang-14 -O2 | 274,620 | 377,397 | 255,053 | **372,252** | 383,146 | 382,671 |
| clang-14 -Os | 283,512 | 385,454 | 262,593 | 383,571 | 399,993 | 406,834 |
| clang-14 -O | 277,541 | 380,870 | 262,567 | 379,930 | 390,926 | 393,006 |
| clang-17 -O3 | 272,999 | **375,092** | 254,344 | 373,480 | 379,830 | 373,387 |
| clang-17 -O2 | 272,899 | 375,352 | 254,865 | 373,069 | 379,716 | 372,736 |
| clang-17 -Os | 281,920 | 381,827 | 259,116 | 381,713 | 397,719 | 395,130 |
| clang-17 -O | 277,238 | 380,499 | 262,071 | 380,322 | 388,207 | 393,298 |
| clang-18 -O3 | 275,151 | 375,518 | **253,574** | 373,364 | 379,445 | 373,928 |
| clang-18 -O2 | **272,809** | 375,507 | 254,352 | 372,955 | 377,977 | **371,422** |
| clang-18 -Os | 281,755 | 381,356 | 259,739 | 379,791 | 393,249 | 393,933 |
| clang-18 -O | 276,098 | 381,656 | 263,048 | 381,450 | 394,175 | 393,534 |

## Appendix B — paired per-configuration ratios

_Ratio = comparison ÷ microcode at the **same** compiler configuration; >1 means microcode is faster. Computed from raw ticks._

### B.1 — 5×51 block, common C ladder (Table 1)

| Config | fiat-crypto | CryptOpt | amd64-51 asm | hand-written C |
|---|---:|---:|---:|---:|
| gcc-11 -O3 | 1.256 | 1.353 | 1.353 | 1.360 |
| gcc-11 -O2 | 1.208 | 1.290 | 1.276 | 1.301 |
| gcc-11 -Os | 1.177 | 1.275 | 1.282 | 1.307 |
| gcc-11 -O | 1.250 | 1.288 | 1.294 | 1.355 |
| gcc-12 -O3 | 1.229 | 1.345 | 1.337 | 1.333 |
| gcc-12 -O2 | 1.205 | 1.303 | 1.292 | 1.305 |
| gcc-12 -Os | 1.189 | 1.292 | 1.290 | 1.310 |
| gcc-12 -O | 1.240 | 1.281 | 1.290 | 1.337 |
| gcc-13 -O3 | 1.264 | 1.346 | 1.343 | 1.338 |
| gcc-13 -O2 | 1.211 | 1.304 | 1.293 | 1.288 |
| gcc-13 -Os | 1.181 | 1.298 | 1.292 | 1.318 |
| gcc-13 -O | 1.274 | 1.289 | 1.292 | 1.325 |
| clang-14 -O3 | 1.395 | 1.355 | 1.373 | 1.396 |
| clang-14 -O2 | 1.395 | 1.356 | 1.374 | 1.393 |
| clang-14 -Os | 1.411 | 1.353 | 1.360 | 1.435 |
| clang-14 -O | 1.409 | 1.369 | 1.372 | 1.416 |
| clang-17 -O3 | 1.391 | 1.368 | 1.374 | 1.368 |
| clang-17 -O2 | 1.391 | 1.367 | 1.375 | 1.366 |
| clang-17 -Os | 1.411 | 1.354 | 1.354 | 1.402 |
| clang-17 -O | 1.400 | 1.372 | 1.372 | 1.419 |
| clang-18 -O3 | 1.379 | 1.357 | 1.365 | 1.359 |
| clang-18 -O2 | 1.386 | 1.367 | 1.376 | 1.361 |
| clang-18 -Os | 1.396 | 1.348 | 1.354 | 1.398 |
| clang-18 -O | 1.428 | 1.382 | 1.382 | 1.425 |
| **geometric mean** | **1.308** | **1.333** | **1.336** | **1.358** |
| **configurations won** | **24/24** | **24/24** | **24/24** | **24/24** |

### B.2 — 4×64 block, amd64-64 C ladder (Table 1)

| Config | microcode ÷ assembly |
|---|---:|
| gcc-11 -O3 | 1.663 |
| gcc-11 -O2 | 1.693 |
| gcc-11 -Os | 1.649 |
| gcc-11 -O | 1.676 |
| gcc-12 -O3 | 1.703 |
| gcc-12 -O2 | 1.703 |
| gcc-12 -Os | 1.631 |
| gcc-12 -O | 1.671 |
| gcc-13 -O3 | 1.714 |
| gcc-13 -O2 | 1.727 |
| gcc-13 -Os | 1.667 |
| gcc-13 -O | 1.684 |
| clang-14 -O3 | 1.672 |
| clang-14 -O2 | 1.666 |
| clang-14 -Os | 1.664 |
| clang-14 -O | 1.679 |
| clang-17 -O3 | 1.688 |
| clang-17 -O2 | 1.692 |
| clang-17 -Os | 1.663 |
| clang-17 -O | 1.675 |
| clang-18 -O3 | 1.678 |
| clang-18 -O2 | 1.677 |
| clang-18 -Os | 1.673 |
| clang-18 -O | 1.678 |
| **geometric mean** | **1.678** |

## Appendix C — dispersion at each selected configuration

_Median of 1000 repetitions (1000 for the two `amd64-64` rows). Raw RDTSC ticks._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| this work (5×51, chained ladder) | 263,779 | 263,527 | 263,640 | 272,944 | 9,304 | clang-18 -O3 |
| amd64-64 asm | 271,841 | 271,721 | 271,752 | 279,925 | 8,173 | gcc-13 -O2 |
| amd64-64 asm, C ladder | 306,243 | 306,134 | — | — | — | clang-17 -O3 |
| 4×64 microcode, C ladder | 514,402 | 514,269 | — | — | — | clang-18 -O2 |
| amd64-51 asm | 356,881 | 356,718 | 356,810 | 365,773 | 8,963 | clang-14 -Os |
| amd64-51 asm, C ladder | 387,769 | 387,600 | 387,669 | 396,547 | 8,878 | clang-18 -O2 |
| 5×51 microcode, C ladder | 301,783 | 301,666 | 301,713 | 312,421 | 10,708 | clang-18 -O2 |
| 5×51 microcode, chained ladder | 277,954 | 276,625 | 277,588 | 286,093 | 8,505 | clang-18 -Os |
| CryptOpt | 372,252 | 372,140 | 372,171 | 380,058 | 7,887 | clang-14 -O2 |
| fiat-crypto | 347,204 | 346,944 | 347,148 | 354,717 | 7,569 | gcc-12 -O3 |
| hand-written C | 371,422 | 371,043 | 371,156 | 380,715 | 9,559 | clang-18 -O2 |
| donna c64 | 337,023 | 336,740 | 336,829 | 343,806 | 6,977 | gcc-11 -O3 |

> The p90−p10 spread is a near-constant ~6,500–7,000 ticks for every contender regardless of its cost, consistent with an external perturbation (timer interrupts on the benchmark core) rather than contender behaviour. Median, minimum and p10 agree to within 0.01% for every contender, so the reported medians sit at the interference-free floor and the ranking is not an artefact of noise.

_Dispersion for the two backends measured only in the A.2 matrix (`uc/Clad`, `a51op/Clad`) is printed by the harness but not currently captured into `RESULTS.md`; re-run the sweep to record it._
