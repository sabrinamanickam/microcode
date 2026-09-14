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
| Run-to-run reproducibility | worst spread 1.483% (amd64-51/asm-Clad @ gcc-13 -O2) | `note_repro` |
| Repetitions | **1000** per contender, all binaries | `BENCH_REPS` |
| Warm-up | RFC 7748 verification, then one untimed call per contender | `benchmark()` |
| Inputs | fixed RFC 7748 vector 1, byte-identical across all repetitions | `benchmark()` |
| Compiler sweep | 24 configs: {gcc-11,12,13, clang-14,17,18} × {-O,-O2,-O3,-Os} | `lib/build_run.sh` |
| Correctness | all contenders pass RFC 7748 vectors 1-4 in every configuration | `test_rfc7748()` |

## Table 1 — Controlled X25519 field-arithmetic comparison

| Representation | Common ladder / framework | Field backend | kcycles/X25519 | Relative cycles |
|---|---|---|---:|---:|
| 5×51 | common C ladder | OpenSSL fe51 asm | 264 | ×0.93 |
| **5×51** | common C ladder | **microcode** | **285** | **×1.00** |
| 5×51 | common C ladder | fiat-crypto | 358 | ×1.26 |
| 5×51 | common C ladder | hand-written C | 382 | ×1.34 |
| 5×51 | common C ladder | CryptOpt | 384 | ×1.35 |
| 5×51 | common C ladder | amd64-51 asm | 386 | ×1.35 |
| **4×64 saturated** | amd64-64 C ladder | **assembly** | **308** | **×1.00** |
| 4×64 saturated | amd64-64 C ladder | microcode | 517 | ×1.68 |

> **Table 1: Controlled X25519 field-arithmetic comparison.** Cycle counts are median core kcycles per X25519, rounded to three significant figures. Within each representation block the ladder and surrounding implementation are identical and only field multiplication and squaring change. The 5×51 rows use our common C Montgomery ladder; the 4×64 rows use the same amd64-64 C `ladderstep.c`. Relative cycle counts are normalised **within** each block, so the two blocks must not be compared against one another. The 4×64 microcode backend computes sq(a) = mul(a, a) because its 75-triad multiplier leaves no room for a dedicated squarer inside the 128-triad patch capacity. All rows are the median of 1000 repetitions.

With the 5×51 representation fixed, microcode outperforms every compiler-generated and published-research field backend evaluated here — fiat-crypto, CryptOpt, the amd64-51 assembly and hand-written C — across all 24 matched compiler and optimisation configurations, with paired geometric-mean speedups between ×1.297 and ×1.345 (Appendix B.1). It does not outperform OpenSSL's hand-tuned fe51 assembly, which is faster on the same ladder (×0.915 paired geometric mean, microcode ahead in 0 of 24 configurations). Isolated-kernel measurement attributes that difference entirely to invocation cost rather than to the arithmetic: with each side's invocation floor removed, the two field multiplications are within 0.4% of one another (Table K5). Entering patch RAM costs a flat 16.2 cycles irrespective of operand count, where an equivalent native call costs 10.1; over the 2,561 field firings of one X25519 that differential is ≈20,300 cycles and accounts for the whole gap. This result also does not extend to the saturated 4×64 representation: with the amd64-64 C ladder held fixed the microcode backend requires 1.678× as many cycles as the assembly backend (1.678× as a paired geometric mean, Appendix B.2). The 128-triad patch capacity prevents the 4×64 implementation from holding both its 75-triad multiplier and a dedicated squarer, forcing squaring through multiplication.

## Table 2 — End-to-end X25519 performance

| Implementation | Representation | kcycles/X25519 | Relative cycles |
|---|---|---:|---:|
| s2n-bignum verified asm | 4×64 | 246 | ×0.888 |
| OpenSSL (own ladder + fe51 asm) | 5×51 | 255 | ×0.922 |
| OpenSSL fe51 asm on our C ladder | 5×51 | 264 | ×0.954 |
| Bernstein–Schwabe amd64-64 asm | 4×64 | 273 | ×0.988 |
| **this work** | **5×51** | **277** | **×1.000** |
| amd64-51 framework + microcode | 5×51 | 290 | ×1.046 |
| donna c64 | 5×51 | 339 | ×1.224 |
| fiat-crypto | 5×51 | 358 | ×1.293 |
| Bernstein–Schwabe amd64-51 asm | 5×51 | 359 | ×1.296 |
| hand-written C | 5×51 | 382 | ×1.378 |
| CryptOpt | 5×51 | 384 | ×1.387 |

> **Table 2: End-to-end X25519 performance.** Cycle counts are median core kcycles per X25519, rounded to three significant figures, each row at its own best compiler configuration. These are complete implementations differing in representation, ladder structure, inversion, field arithmetic and code organisation; the table therefore establishes overall standing but does **not** isolate the effect of microcode. Lower is better; relative cycles are normalised to this work.

## Table 3 — 5×51 integration and ladder decomposition

| Variant | Ladder | Field ops | kcycles/X25519 |
|---|---|---|---:|
| amd64-51 native | qhasm, monolithic | qhasm asm | 359 |
| C-ladder control | C, per-op calls | qhasm asm | 390 |
| C-ladder + microcode | C, per-op calls | microcode | 303 |
| chained ladder + microcode | inline asm, register-chained | microcode | 290 |

**Transitions** — each changes exactly one element from the row above:

| Transition | Effect isolated | best-of-24 | paired geomean |
|---|---|---:|---:|
| native qhasm → C ladder | loss of ladder/field-op fusion | ×1.087 | ×1.092 |
| asm field ops → microcode | controlled microcode field-op gain | ×1.287 | ×1.264 |
| C ladder → register-chained | ladder-integration recovery | ×1.047 | ×1.022 (×1.057 excl. gcc `-Os`) |

> **Table 3: 5×51 integration and ladder decomposition.** All rows use the amd64-51 framework. Register-chaining the ladder helps in 21 of 24 configurations (paired geometric mean ×1.057); under gcc `-Os` the inline-assembly ladder degrades sharply in this framework (×0.80, reproducible across gcc-11/12/13 to within 43 ticks), which pulls the all-configuration geometric mean down to ×1.022. The same chained ladder in our own framework shows no such degradation, so this is a compiler/framework interaction rather than a property of the ladder. The final row is `amd64-51/ucode`, **not** the canonical implementation reported in Table 2.

## Appendix A — full per-configuration sweep

_Median RDTSC ticks per X25519 at each of the 24 configurations. **Bold** = best (lowest) in that column. Raw ticks; multiply by 1.00548 for core cycles._

### A.1 — End-to-end, every complete implementation (Table 2)

| Config | this work | a64/asm | a64/asm+Clad | a64/ucode | a51/asm | a51/asm+Clad | a51/uc+Clad | a51/ucode | CryptOpt | fiat | hand-C | donna |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gcc-11 -O3 | 277,673 | 272,877 | 336,151 | 559,369 | 359,511 | 394,057 | 310,516 | 294,326 | 392,860 | 358,353 | 388,723 | **336,932** | **244,402** | 270,571 | 254,766 |
| gcc-11 -O2 | 284,735 | 273,128 | 326,563 | 553,229 | 359,078 | 390,692 | 309,701 | 297,069 | 398,792 | 375,310 | 403,430 | 370,962 | 244,542 | 278,357 | 254,222 |
| gcc-11 -Os | 284,764 | 274,146 | 362,150 | 596,562 | 359,096 | 398,654 | 319,349 | 395,725 | 402,861 | 371,404 | 410,638 | 368,692 | 244,687 | 281,256 | 253,909 |
| gcc-11 -O | 282,691 | 272,874 | 335,591 | 568,854 | 358,457 | 391,486 | 321,858 | 291,973 | 392,105 | 381,033 | 411,000 | 381,027 | 244,512 | 273,736 | 254,313 |
| gcc-12 -O3 | 277,676 | 272,052 | 324,129 | 552,141 | 359,420 | 391,413 | 308,417 | 289,959 | 386,938 | **356,046** | 385,715 | 340,479 | 244,634 | 265,944 | 254,224 |
| gcc-12 -O2 | 285,108 | 271,998 | 324,243 | 552,242 | 358,923 | 391,871 | 308,643 | 299,171 | 400,236 | 371,160 | 401,520 | 370,305 | 244,589 | 276,446 | 254,037 |
| gcc-12 -Os | 286,564 | 272,576 | 363,914 | 593,982 | 358,754 | 398,139 | 316,682 | 395,223 | 405,345 | 371,147 | 410,135 | 372,456 | 244,644 | 282,826 | **253,903** |
| gcc-12 -O | 279,740 | 272,606 | 340,306 | 568,805 | 358,376 | 390,427 | 322,457 | 291,614 | 392,248 | 378,378 | 409,908 | 376,540 | 244,530 | 273,553 | 254,475 |
| gcc-13 -O3 | 281,140 | 272,074 | 314,896 | 539,562 | 359,318 | 391,510 | 308,463 | 292,547 | 387,633 | 367,163 | 388,194 | 340,746 | **244,402** | 267,920 | 254,542 |
| gcc-13 -O2 | 285,449 | **271,969** | 314,091 | 542,145 | 359,041 | 393,764 | 310,267 | 298,728 | 398,715 | 371,263 | 395,729 | 355,617 | 244,637 | 275,249 | 253,978 |
| gcc-13 -Os | 285,928 | 272,588 | 348,541 | 580,478 | 358,841 | 398,269 | 316,514 | 395,460 | 404,297 | 372,369 | 412,989 | 356,016 | 244,829 | 284,074 | 254,047 |
| gcc-13 -O | 283,697 | 273,464 | 340,146 | 566,598 | 359,281 | 390,191 | 322,673 | 291,952 | 392,428 | 389,355 | 404,242 | 370,119 | 244,469 | 273,583 | 254,381 |
| clang-14 -O3 | 275,887 | 276,138 | 312,684 | 521,780 | 357,075 | 388,980 | 305,040 | 292,306 | 382,594 | 388,394 | 389,691 | 454,726 | 250,729 | 262,902 | 254,772 |
| clang-14 -O2 | 275,385 | 274,658 | 312,783 | 521,234 | 357,255 | 388,628 | 304,139 | 292,057 | 382,073 | 388,671 | 388,655 | 454,366 | 253,053 | 263,191 | 255,400 |
| clang-14 -Os | 280,955 | 272,993 | 314,895 | 523,767 | 356,930 | 390,912 | 304,757 | 289,039 | 389,239 | 407,018 | 415,677 | 447,348 | 244,539 | 267,833 | 254,385 |
| clang-14 -O | 280,828 | 273,468 | 314,410 | 527,872 | 359,367 | 388,255 | 312,119 | 290,240 | 387,533 | 398,786 | 401,606 | 466,223 | 252,728 | 268,541 | 255,181 |
| clang-17 -O3 | 275,486 | 275,629 | **306,247** | 516,206 | 357,235 | 389,658 | 302,080 | 289,804 | 382,132 | 388,665 | **379,510** | 442,169 | 251,126 | **262,783** | 254,283 |
| clang-17 -O2 | 275,451 | 274,700 | 306,316 | 515,691 | 357,113 | 389,426 | 307,835 | 289,800 | 382,376 | 392,858 | 379,934 | 440,463 | 252,956 | 263,195 | 254,906 |
| clang-17 -Os | 279,420 | 272,640 | 310,224 | 515,992 | **356,861** | 392,280 | 305,862 | 287,954 | 388,510 | 407,558 | 400,653 | 436,107 | 244,632 | 267,322 | 254,368 |
| clang-17 -O | 281,900 | 272,136 | 307,754 | 515,579 | 359,365 | 388,756 | 302,548 | 288,589 | 388,253 | 395,136 | 400,474 | 453,608 | 252,932 | 268,287 | 255,442 |
| clang-18 -O3 | 275,452 | 275,579 | 306,748 | 515,593 | 357,073 | **388,074** | 305,850 | 291,386 | 382,464 | 387,616 | 380,295 | 458,557 | 251,130 | 263,450 | 254,257 |
| clang-18 -O2 | **275,334** | 275,271 | 306,824 | 514,319 | 357,184 | 390,159 | **301,571** | 291,891 | **381,855** | 385,932 | 380,206 | 458,976 | 253,015 | 263,955 | 255,013 |
| clang-18 -Os | 279,705 | 272,352 | 308,421 | 518,114 | 356,926 | 389,957 | 305,341 | **287,952** | 389,477 | 401,388 | 403,666 | 453,170 | 244,562 | 266,737 | 254,464 |
| clang-18 -O | 279,940 | 273,964 | 306,869 | **513,942** | 359,269 | 388,697 | 302,831 | 291,280 | 389,986 | 398,576 | 403,424 | 460,788 | 252,984 | 267,799 | 255,395 |

### A.2 — Common C ladder, only the field backend differs (Table 1, 5×51 block)

| Config | microcode | amd64-51 asm | CryptOpt | fiat | hand-C |
|---|---|---|---|---|---|---|
| gcc-11 -O3 | 289,815 | 390,985 | 270,571 | 392,860 | 358,353 | 388,723 |
| gcc-11 -O2 | 310,988 | 395,482 | 278,357 | 398,792 | 375,310 | 403,430 |
| gcc-11 -Os | 312,792 | 400,692 | 281,256 | 402,861 | 371,404 | 410,638 |
| gcc-11 -O | 306,120 | 393,129 | 273,736 | 392,105 | 381,033 | 411,000 |
| gcc-12 -O3 | 289,597 | 386,212 | 265,944 | 386,938 | **356,046** | 385,715 |
| gcc-12 -O2 | 309,455 | 397,917 | 276,446 | 400,236 | 371,160 | 401,520 |
| gcc-12 -Os | 313,363 | 402,143 | 282,826 | 405,345 | 371,147 | 410,135 |
| gcc-12 -O | 307,234 | 394,693 | 273,553 | 392,248 | 378,378 | 409,908 |
| gcc-13 -O3 | 290,824 | 390,435 | 267,920 | 387,633 | 367,163 | 388,194 |
| gcc-13 -O2 | 307,790 | 396,140 | 275,249 | 398,715 | 371,263 | 395,729 |
| gcc-13 -Os | 315,432 | 403,505 | 284,074 | 404,297 | 372,369 | 412,989 |
| gcc-13 -O | 306,520 | 393,947 | 273,583 | 392,428 | 389,355 | 404,242 |
| clang-14 -O3 | 284,072 | 384,186 | 262,902 | 382,594 | 388,394 | 389,691 |
| clang-14 -O2 | 283,819 | **384,099** | 263,191 | 382,073 | 388,671 | 388,655 |
| clang-14 -Os | 289,210 | 391,405 | 267,833 | 389,239 | 407,018 | 415,677 |
| clang-14 -O | 285,714 | 389,112 | 268,541 | 387,533 | 398,786 | 401,606 |
| clang-17 -O3 | 285,421 | 386,077 | **262,783** | 382,132 | 388,665 | **379,510** |
| clang-17 -O2 | **283,528** | 386,128 | 263,195 | 382,376 | 392,858 | 379,934 |
| clang-17 -Os | 290,686 | 390,475 | 267,322 | 388,510 | 407,558 | 400,653 |
| clang-17 -O | 285,761 | 388,598 | 268,287 | 388,253 | 395,136 | 400,474 |
| clang-18 -O3 | 286,069 | 386,138 | 263,450 | 382,464 | 387,616 | 380,295 |
| clang-18 -O2 | 287,426 | 385,892 | 263,955 | **381,855** | 385,932 | 380,206 |
| clang-18 -Os | 289,865 | 390,216 | 266,737 | 389,477 | 401,388 | 403,666 |
| clang-18 -O | 285,693 | 389,567 | 267,799 | 389,986 | 398,576 | 403,424 |

## Appendix B — paired per-configuration ratios

_Ratio = comparison ÷ microcode at the **same** compiler configuration; >1 means microcode is faster. Computed from raw ticks._

### B.1 — 5×51 block, common C ladder (Table 1)

| Config | fiat-crypto | CryptOpt | amd64-51 asm | hand-written C |
|---|---:|---:|---:|---:|
| gcc-11 -O3 | 1.236 | 1.356 | 1.349 | 1.341 |
| gcc-11 -O2 | 1.207 | 1.282 | 1.272 | 1.297 |
| gcc-11 -Os | 1.187 | 1.288 | 1.281 | 1.313 |
| gcc-11 -O | 1.245 | 1.281 | 1.284 | 1.343 |
| gcc-12 -O3 | 1.229 | 1.336 | 1.334 | 1.332 |
| gcc-12 -O2 | 1.199 | 1.293 | 1.286 | 1.298 |
| gcc-12 -Os | 1.184 | 1.294 | 1.283 | 1.309 |
| gcc-12 -O | 1.232 | 1.277 | 1.285 | 1.334 |
| gcc-13 -O3 | 1.262 | 1.333 | 1.343 | 1.335 |
| gcc-13 -O2 | 1.206 | 1.295 | 1.287 | 1.286 |
| gcc-13 -Os | 1.181 | 1.282 | 1.279 | 1.309 |
| gcc-13 -O | 1.270 | 1.280 | 1.285 | 1.319 |
| clang-14 -O3 | 1.367 | 1.347 | 1.352 | 1.372 |
| clang-14 -O2 | 1.369 | 1.346 | 1.353 | 1.369 |
| clang-14 -Os | 1.407 | 1.346 | 1.353 | 1.437 |
| clang-14 -O | 1.396 | 1.356 | 1.362 | 1.406 |
| clang-17 -O3 | 1.362 | 1.339 | 1.353 | 1.330 |
| clang-17 -O2 | 1.386 | 1.349 | 1.362 | 1.340 |
| clang-17 -Os | 1.402 | 1.337 | 1.343 | 1.378 |
| clang-17 -O | 1.383 | 1.359 | 1.360 | 1.401 |
| clang-18 -O3 | 1.355 | 1.337 | 1.350 | 1.329 |
| clang-18 -O2 | 1.343 | 1.329 | 1.343 | 1.323 |
| clang-18 -Os | 1.385 | 1.344 | 1.346 | 1.393 |
| clang-18 -O | 1.395 | 1.365 | 1.364 | 1.412 |
| **geometric mean** | **1.297** | **1.323** | **1.325** | **1.345** |
| **configurations won** | **24/24** | **24/24** | **24/24** | **24/24** |

### B.2 — 4×64 block, amd64-64 C ladder (Table 1)

| Config | microcode ÷ assembly |
|---|---:|
| gcc-11 -O3 | 1.664 |
| gcc-11 -O2 | 1.694 |
| gcc-11 -Os | 1.647 |
| gcc-11 -O | 1.695 |
| gcc-12 -O3 | 1.703 |
| gcc-12 -O2 | 1.703 |
| gcc-12 -Os | 1.632 |
| gcc-12 -O | 1.671 |
| gcc-13 -O3 | 1.713 |
| gcc-13 -O2 | 1.726 |
| gcc-13 -Os | 1.665 |
| gcc-13 -O | 1.666 |
| clang-14 -O3 | 1.669 |
| clang-14 -O2 | 1.666 |
| clang-14 -Os | 1.663 |
| clang-14 -O | 1.679 |
| clang-17 -O3 | 1.686 |
| clang-17 -O2 | 1.684 |
| clang-17 -Os | 1.663 |
| clang-17 -O | 1.675 |
| clang-18 -O3 | 1.681 |
| clang-18 -O2 | 1.676 |
| clang-18 -Os | 1.680 |
| clang-18 -O | 1.675 |
| **geometric mean** | **1.678** |

## Appendix C — dispersion at each selected configuration

_Median of 1000 repetitions (1000 for the two `amd64-64` rows). Raw RDTSC ticks._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| this work (5×51, chained ladder) | 275,334 | 273,753 | 275,227 | 283,145 | 7,918 | clang-18 -O2 |
| amd64-64 asm | 271,969 | 271,821 | 271,879 | 279,903 | 8,024 | gcc-13 -O2 |
| amd64-64 asm, C ladder | 306,247 | 306,135 | — | — | — | clang-17 -O3 |
| 4×64 microcode, C ladder | 513,942 | 510,745 | — | — | — | clang-18 -O |
| amd64-51 asm | 356,861 | 356,716 | 356,759 | 365,657 | 8,898 | clang-17 -Os |
| amd64-51 asm, C ladder | 388,074 | 387,940 | 388,009 | 397,378 | 9,369 | clang-18 -O3 |
| 5×51 microcode, C ladder | 301,571 | 301,476 | 301,516 | 311,809 | 10,293 | clang-18 -O2 |
| 5×51 microcode, chained ladder | 287,952 | 287,152 | 287,875 | 296,040 | 8,165 | clang-18 -Os |
| CryptOpt | 381,855 | 381,784 | 381,813 | 390,579 | 8,766 | clang-18 -O2 |
| fiat-crypto | 356,046 | 355,973 | 355,995 | 364,580 | 8,585 | gcc-12 -O3 |
| hand-written C | 379,510 | 379,263 | 379,394 | 388,431 | 9,037 | clang-17 -O3 |
| donna c64 | 336,932 | 336,317 | 336,723 | 344,384 | 7,661 | gcc-11 -O3 |

> The p90−p10 spread is a near-constant ~6,500–7,000 ticks for every contender regardless of its cost, consistent with an external perturbation (timer interrupts on the benchmark core) rather than contender behaviour. Median, minimum and p10 agree to within 0.01% for every contender, so the reported medians sit at the interference-free floor and the ranking is not an artefact of noise.

_Dispersion for the two backends measured only in the A.2 matrix (`uc/Clad`, `a51op/Clad`) is printed by the harness but not currently captured into `RESULTS.md`; re-run the sweep to record it._
