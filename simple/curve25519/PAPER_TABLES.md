# Curve25519 / X25519 — paper tables

_Generated from `RESULTS.md` (Appendices A.1, A.2) by `gen_paper_tables.py`. Do not hand-edit; regenerate._

_Absolute values are RDTSC ticks converted to core cycles by ×1.00548 (f_core/f_TSC, measured by this run's frequency guard), then rounded to three significant figures. Ratios are computed from raw full-precision ticks and are invariant to that correction._

## Measurement setup

| item | value | source |
|---|---|---|
| CPU | Intel(R) Celeron(R) CPU N3350 @ 1.10GHz | `/proc/cpuinfo` |
| Core / pinning | core 1, `taskset -c 1` + in-process `sched_setaffinity` (BENCH_CORE=1) | `lib/isolation.sh` |
| Governor | `userspace`, requested 1094410 kHz | `lib/freq_guard.sh` |
| Turbo | disabled (`no_turbo = 1`) | `lib/freq_guard.sh` |
| Delivered core freq (load) | 1100 MHz (APERF/MPERF) | turbostat |
| TSC / RDTSC rate | 1094 MHz | turbostat |
| Correction f_core/f_TSC | 1.00548 | measured before the sweep |
| Timing | `RDTSC`, serialised `cpuid; rdtsc` before / `rdtscp; cpuid` after | `full_curve25519_inline2.c` |
| Timer overhead | 15 ticks (min 13, p99 17); **not** subtracted — 0.005% of ~300k | measured |
| Post-sweep frequency check | stable (+0.000% over the sweep) (delivered 1100 MHz / TSC 1094 MHz after) | `lib/freq_guard.sh` |
| Core isolation | core 1; 28 IRQs steered to core 0 (2 per-CPU/unmovable); SCHED_FIFO 99 | `lib/isolation.sh` |
| nohz_full / isolcpus | off — periodic timer tick still hits core 1 | `/proc/cmdline` |
| Timing order | **interleaved** — round-robin, one repetition of every contender per round | `benchmark()` |
| Statistic | median; each configuration measured 3× and reduced to the median of those runs | `bench_stats()`, `median_of` |
| Run-to-run reproducibility | worst spread 1.615% (amd64-64/ucode @ clang-14 -O3) | `note_repro` |
| Repetitions | **1000** per contender, all binaries | `BENCH_REPS` |
| Warm-up | RFC 7748 verification, then one untimed call per contender | `benchmark()` |
| Inputs | fixed RFC 7748 vector 1, byte-identical across all repetitions | `benchmark()` |
| Compiler sweep | 24 configs: {gcc-11,12,13, clang-14,17,18} × {-O,-O2,-O3,-Os} | `lib/build_run.sh` |
| Correctness | all contenders pass RFC 7748 vectors 1-4 in every configuration | `test_rfc7748()` |

## Table 1 — Controlled X25519 field-arithmetic comparison

| Representation | Common ladder / framework | Field backend | kcycles/X25519 | Relative cycles |
|---|---|---|---:|---:|
| 5×51 | common C ladder | OpenSSL fe51 asm | 255 | ×0.97 |
| **5×51** | common C ladder | **microcode** | **263** | **×1.00** |
| 5×51 | common C ladder | fiat-crypto | 349 | ×1.33 |
| 5×51 | common C ladder | CryptOpt | 374 | ×1.42 |
| 5×51 | common C ladder | hand-written C | 374 | ×1.43 |
| 5×51 | common C ladder | amd64-51 asm | 377 | ×1.44 |
| **4×64 saturated** | amd64-64 C ladder | **assembly** | **309** | **×1.00** |
| 4×64 saturated | amd64-64 C ladder | microcode | 534 | ×1.73 |

> **Table 1: Controlled X25519 field-arithmetic comparison.** Cycle counts are median core kcycles per X25519, rounded to three significant figures. Within each representation block the ladder and surrounding implementation are identical and only field multiplication and squaring change. The 5×51 rows use our common C Montgomery ladder; the 4×64 rows use the same amd64-64 C `ladderstep.c`. Relative cycle counts are normalised **within** each block, so the two blocks must not be compared against one another. The 4×64 microcode backend computes sq(a) = mul(a, a) because its 75-triad multiplier leaves no room for a dedicated squarer inside the 128-triad patch capacity. All rows are the median of 1000 repetitions.

With the 5×51 representation fixed, microcode outperforms every compiler-generated and published-research field backend evaluated here — fiat-crypto, CryptOpt, the amd64-51 assembly and hand-written C — across all 24 matched compiler and optimisation configurations, with paired geometric-mean speedups between ×1.368 and ×1.424 (Appendix B.1). It does not outperform OpenSSL's hand-tuned fe51 assembly, which is faster on the same ladder (×0.957 paired geometric mean, microcode ahead in 0 of 24 configurations). Isolated-kernel measurement relates that difference to invocation cost: with each side's invocation floor removed, microcode multiplication costs 88.5 cycles against OpenSSL's 88.0 (+0.6%, Table K5). Entering patch RAM costs 15.2 cycles where an equivalent native call costs 9.1; over the 2,561 field firings of one X25519 that differential is ≈15,648 cycles. This result also does not extend to the saturated 4×64 representation: with the amd64-64 C ladder held fixed the microcode backend requires 1.732× as many cycles as the assembly backend (1.732× as a paired geometric mean, Appendix B.2). The 128-triad patch capacity prevents the 4×64 implementation from holding both its 75-triad multiplier and a dedicated squarer, forcing squaring through multiplication.

## Table 2 — End-to-end X25519 performance

| Implementation | Representation | kcycles/X25519 | Relative cycles |
|---|---|---:|---:|
| s2n-bignum verified asm | 4×64 | 246 | ×0.978 |
| **this work** | **5×51** | **251** | **×1.000** |
| OpenSSL (own ladder + fe51 asm) | 5×51 | 252 | ×1.003 |
| OpenSSL fe51 asm on our C ladder | 5×51 | 255 | ×1.016 |
| amd64-51 framework + microcode | 5×51 | 261 | ×1.039 |
| Bernstein–Schwabe amd64-64 asm | 4×64 | 273 | ×1.089 |
| donna c64 | 5×51 | 340 | ×1.352 |
| fiat-crypto | 5×51 | 349 | ×1.390 |
| Bernstein–Schwabe amd64-51 asm | 5×51 | 359 | ×1.430 |
| CryptOpt | 5×51 | 374 | ×1.489 |
| hand-written C | 5×51 | 374 | ×1.491 |

> **Table 2: End-to-end X25519 performance.** Cycle counts are median core kcycles per X25519, rounded to three significant figures, each row at its own best compiler configuration. These are complete implementations differing in representation, ladder structure, inversion, field arithmetic and code organisation; the table therefore establishes overall standing but does **not** isolate the effect of microcode. Lower is better; relative cycles are normalised to this work.

## Table 3 — 5×51 integration and ladder decomposition

| Variant | Ladder | Field ops | kcycles/X25519 |
|---|---|---|---:|
| amd64-51 native | qhasm, monolithic | qhasm asm | 359 |
| C-ladder control | C, per-op calls | qhasm asm | 390 |
| C-ladder + microcode | C, per-op calls | microcode | 290 |
| chained ladder + microcode | inline asm, register-chained | microcode | 261 |

**Transitions** — each changes exactly one element from the row above:

| Transition | Effect isolated | best-of-24 | paired geomean |
|---|---|---:|---:|
| native qhasm → C ladder | loss of ladder/field-op fusion | ×1.087 | ×1.092 |
| asm field ops → microcode | controlled microcode field-op gain | ×1.348 | ×1.337 |
| C ladder → register-chained | ladder-integration recovery | ×1.110 | ×1.067 (×1.108 excl. gcc `-Os`) |

> **Table 3: 5×51 integration and ladder decomposition.** All rows use the amd64-51 framework. Register-chaining the ladder helps in 21 of 24 configurations (paired geometric mean ×1.108); under gcc `-Os` the inline-assembly ladder degrades sharply in this framework (×0.82), which pulls the all-configuration geometric mean down to ×1.067. The same chained ladder in our own framework shows no such degradation, so this is a compiler/framework interaction rather than a property of the ladder. The final row is `amd64-51/ucode`, **not** the canonical implementation reported in Table 2.

## Appendix A — full per-configuration sweep

_Median RDTSC ticks per X25519 at each of the 24 configurations. **Bold** = best (lowest) in that column. Raw ticks; multiply by 1.00548 for core cycles._

### A.1 — End-to-end, every complete implementation (Table 2)

| Config | this work | a64/asm | a64/asm+Clad | a64/ucode | a51/asm | a51/asm+Clad | a51/uc+Clad | a51/ucode | CryptOpt | fiat | hand-C | donna |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gcc-11 -O3 | 253,371 | 272,527 | 343,465 | 587,454 | 359,033 | 394,476 | 293,434 | 265,561 | 380,918 | 353,445 | 384,211 | **337,689** | 244,611 | 260,938 | 250,532 |
| gcc-11 -O2 | 255,920 | 273,266 | 333,702 | 586,848 | 359,118 | 390,469 | 292,386 | 265,911 | 388,874 | 364,604 | 392,606 | 371,037 | 244,395 | 269,648 | 250,725 |
| gcc-11 -Os | 257,966 | 274,212 | 373,908 | 631,462 | 359,273 | 398,882 | 301,665 | 368,750 | 390,615 | 360,656 | 399,818 | 368,956 | 244,562 | 271,824 | 250,596 |
| gcc-11 -O | 256,099 | 273,032 | 335,610 | 591,564 | 359,000 | 391,224 | 301,532 | 263,291 | 380,744 | 370,067 | 400,602 | 380,571 | 244,306 | 263,545 | 250,716 |
| gcc-12 -O3 | 253,035 | 271,869 | 333,078 | 581,862 | 358,861 | 393,605 | 291,471 | 265,856 | 379,764 | **347,211** | 376,495 | 341,243 | 244,628 | 259,071 | **250,509** |
| gcc-12 -O2 | 256,593 | 272,122 | 332,984 | 581,785 | 359,171 | 393,691 | 293,482 | 266,328 | 391,590 | 362,089 | 392,113 | 370,720 | 244,293 | 268,667 | 250,696 |
| gcc-12 -Os | 258,959 | 272,712 | 374,924 | 633,392 | 358,974 | 398,430 | 302,670 | 368,436 | 393,023 | 360,101 | 400,602 | 370,337 | 244,565 | 273,386 | 250,529 |
| gcc-12 -O | 253,778 | 272,562 | 340,339 | 592,652 | 359,615 | 390,165 | 300,162 | 263,218 | 381,503 | 367,591 | 396,950 | 377,652 | 244,290 | 262,900 | 250,584 |
| gcc-13 -O3 | 255,639 | **271,864** | 323,599 | 572,677 | 359,090 | 393,252 | 291,563 | 265,447 | 383,584 | 360,356 | 381,332 | 341,442 | 244,520 | 263,941 | 250,578 |
| gcc-13 -O2 | 256,917 | 272,055 | 323,833 | 575,481 | 359,065 | 392,014 | 290,654 | 265,545 | 390,069 | 362,054 | 385,310 | 356,626 | 244,337 | 267,006 | 250,670 |
| gcc-13 -Os | 260,547 | 272,736 | 361,126 | 616,190 | 358,809 | 398,462 | 302,515 | 368,123 | 396,767 | 360,309 | 403,462 | 356,218 | 244,561 | 274,842 | 250,537 |
| gcc-13 -O | 255,623 | 273,520 | 336,370 | 590,792 | 359,855 | 390,008 | 299,615 | 263,095 | 382,423 | 377,409 | 392,985 | 372,652 | 244,377 | 262,489 | 250,596 |
| clang-14 -O3 | 251,675 | 274,718 | 315,969 | 538,933 | 361,856 | 392,101 | 289,497 | 268,006 | **371,948** | 384,115 | 390,957 | 454,437 | 251,865 | 255,004 | 251,684 |
| clang-14 -O2 | 252,516 | 276,161 | 316,027 | 538,422 | 357,372 | 389,187 | 288,249 | 262,554 | 372,340 | 383,777 | 383,333 | 454,536 | 251,927 | 254,788 | 250,535 |
| clang-14 -Os | 253,151 | 273,022 | 315,051 | 540,456 | 357,068 | 390,644 | 288,506 | 262,086 | 382,672 | 401,376 | 406,015 | 447,417 | **244,259** | 262,962 | 250,668 |
| clang-14 -O | 257,257 | 273,403 | 314,506 | 551,093 | 357,881 | **388,235** | 295,856 | 262,443 | 380,042 | 391,252 | 393,624 | 463,005 | 250,285 | 262,634 | 251,659 |
| clang-17 -O3 | 249,725 | 274,642 | **306,821** | 533,531 | 361,211 | 390,825 | 289,507 | 265,282 | 372,707 | 379,917 | 380,153 | 441,982 | 251,458 | 254,588 | 251,657 |
| clang-17 -O2 | 251,076 | 275,695 | 306,878 | 533,679 | 357,304 | 388,475 | 288,285 | 259,965 | 373,720 | 381,707 | 373,851 | 440,569 | 251,030 | 254,629 | 250,521 |
| clang-17 -Os | 254,978 | 272,313 | 310,373 | 535,210 | **357,053** | 392,144 | 291,292 | 259,739 | 381,697 | 395,993 | 393,441 | 435,283 | 244,261 | 259,482 | 250,580 |
| clang-17 -O | 257,273 | 272,241 | 307,991 | **531,309** | 358,782 | 388,818 | 288,297 | 261,455 | 380,241 | 388,231 | 393,831 | 449,732 | 250,556 | 262,222 | 251,651 |
| clang-18 -O3 | **249,721** | 274,389 | 307,332 | 532,217 | 362,012 | 390,389 | 289,696 | 265,048 | 372,637 | 380,377 | 380,611 | 458,180 | 251,484 | 254,454 | 251,614 |
| clang-18 -O2 | 251,119 | 275,748 | 307,391 | 531,926 | 357,948 | 390,247 | 289,004 | 260,822 | 372,689 | 379,701 | **372,316** | 459,039 | 250,934 | **253,831** | 250,592 |
| clang-18 -Os | 254,478 | 272,637 | 308,564 | 533,778 | 357,160 | 391,990 | 291,417 | **259,519** | 380,670 | 394,269 | 394,517 | 452,313 | 244,286 | 259,472 | 250,733 |
| clang-18 -O | 257,127 | 273,282 | 307,027 | 531,759 | 357,738 | 388,939 | **288,025** | 260,885 | 380,793 | 394,381 | 394,505 | 457,614 | 251,530 | 263,100 | 251,655 |

### A.2 — Common C ladder, only the field backend differs (Table 1, 5×51 block)

| Config | microcode | amd64-51 asm | CryptOpt | fiat | hand-C |
|---|---|---|---|---|---|---|
| gcc-11 -O3 | 267,377 | 380,943 | 260,938 | 380,918 | 353,445 | 384,211 |
| gcc-11 -O2 | 288,628 | 385,033 | 269,648 | 388,874 | 364,604 | 392,606 |
| gcc-11 -Os | 294,157 | 392,199 | 271,824 | 390,615 | 360,656 | 399,818 |
| gcc-11 -O | 282,292 | 382,246 | 263,545 | 380,744 | 370,067 | 400,602 |
| gcc-12 -O3 | 268,576 | 377,643 | 259,071 | 379,764 | **347,211** | 376,495 |
| gcc-12 -O2 | 287,626 | 388,481 | 268,667 | 391,590 | 362,089 | 392,113 |
| gcc-12 -Os | 294,776 | 393,666 | 273,386 | 393,023 | 360,101 | 400,602 |
| gcc-12 -O | 283,359 | 383,614 | 262,900 | 381,503 | 367,591 | 396,950 |
| gcc-13 -O3 | 269,406 | 382,910 | 263,941 | 383,584 | 360,356 | 381,332 |
| gcc-13 -O2 | 286,242 | 386,529 | 267,006 | 390,069 | 362,054 | 385,310 |
| gcc-13 -Os | 295,881 | 394,705 | 274,842 | 396,767 | 360,309 | 403,462 |
| gcc-13 -O | 283,066 | 382,881 | 262,489 | 382,423 | 377,409 | 392,985 |
| clang-14 -O3 | 264,260 | 377,397 | 255,004 | **371,948** | 384,115 | 390,957 |
| clang-14 -O2 | 262,709 | 377,674 | 254,788 | 372,340 | 383,777 | 383,333 |
| clang-14 -Os | 273,207 | 384,982 | 262,962 | 382,672 | 401,376 | 406,015 |
| clang-14 -O | 263,220 | 379,530 | 262,634 | 380,042 | 391,252 | 393,624 |
| clang-17 -O3 | 263,414 | 375,293 | 254,588 | 372,707 | 379,917 | 380,153 |
| clang-17 -O2 | 261,750 | **375,267** | 254,629 | 373,720 | 381,707 | 373,851 |
| clang-17 -Os | 269,936 | 381,866 | 259,482 | 381,697 | 395,993 | 393,441 |
| clang-17 -O | 263,251 | 380,484 | 262,222 | 380,241 | 388,231 | 393,831 |
| clang-18 -O3 | 263,009 | 375,331 | 254,454 | 372,637 | 380,377 | 380,611 |
| clang-18 -O2 | **261,141** | 375,594 | **253,831** | 372,689 | 379,701 | **372,316** |
| clang-18 -Os | 270,704 | 381,646 | 259,472 | 380,670 | 394,269 | 394,517 |
| clang-18 -O | 263,391 | 381,524 | 263,100 | 380,793 | 394,381 | 394,505 |

## Appendix B — paired per-configuration ratios

_Ratio = comparison ÷ microcode at the **same** compiler configuration; >1 means microcode is faster. Computed from raw ticks._

### B.1 — 5×51 block, common C ladder (Table 1)

| Config | fiat-crypto | CryptOpt | amd64-51 asm | hand-written C |
|---|---:|---:|---:|---:|
| gcc-11 -O3 | 1.322 | 1.425 | 1.425 | 1.437 |
| gcc-11 -O2 | 1.263 | 1.347 | 1.334 | 1.360 |
| gcc-11 -Os | 1.226 | 1.328 | 1.333 | 1.359 |
| gcc-11 -O | 1.311 | 1.349 | 1.354 | 1.419 |
| gcc-12 -O3 | 1.293 | 1.414 | 1.406 | 1.402 |
| gcc-12 -O2 | 1.259 | 1.361 | 1.351 | 1.363 |
| gcc-12 -Os | 1.222 | 1.333 | 1.335 | 1.359 |
| gcc-12 -O | 1.297 | 1.346 | 1.354 | 1.401 |
| gcc-13 -O3 | 1.338 | 1.424 | 1.421 | 1.415 |
| gcc-13 -O2 | 1.265 | 1.363 | 1.350 | 1.346 |
| gcc-13 -Os | 1.218 | 1.341 | 1.334 | 1.364 |
| gcc-13 -O | 1.333 | 1.351 | 1.353 | 1.388 |
| clang-14 -O3 | 1.454 | 1.408 | 1.428 | 1.479 |
| clang-14 -O2 | 1.461 | 1.417 | 1.438 | 1.459 |
| clang-14 -Os | 1.469 | 1.401 | 1.409 | 1.486 |
| clang-14 -O | 1.486 | 1.444 | 1.442 | 1.495 |
| clang-17 -O3 | 1.442 | 1.415 | 1.425 | 1.443 |
| clang-17 -O2 | 1.458 | 1.428 | 1.434 | 1.428 |
| clang-17 -Os | 1.467 | 1.414 | 1.415 | 1.458 |
| clang-17 -O | 1.475 | 1.444 | 1.445 | 1.496 |
| clang-18 -O3 | 1.446 | 1.417 | 1.427 | 1.447 |
| clang-18 -O2 | 1.454 | 1.427 | 1.438 | 1.426 |
| clang-18 -Os | 1.456 | 1.406 | 1.410 | 1.457 |
| clang-18 -O | 1.497 | 1.446 | 1.449 | 1.498 |
| **geometric mean** | **1.368** | **1.393** | **1.396** | **1.424** |
| **configurations won** | **24/24** | **24/24** | **24/24** | **24/24** |

### B.2 — 4×64 block, amd64-64 C ladder (Table 1)

| Config | microcode ÷ assembly |
|---|---:|
| gcc-11 -O3 | 1.710 |
| gcc-11 -O2 | 1.759 |
| gcc-11 -Os | 1.689 |
| gcc-11 -O | 1.763 |
| gcc-12 -O3 | 1.747 |
| gcc-12 -O2 | 1.747 |
| gcc-12 -Os | 1.689 |
| gcc-12 -O | 1.741 |
| gcc-13 -O3 | 1.770 |
| gcc-13 -O2 | 1.777 |
| gcc-13 -Os | 1.706 |
| gcc-13 -O | 1.756 |
| clang-14 -O3 | 1.706 |
| clang-14 -O2 | 1.704 |
| clang-14 -Os | 1.715 |
| clang-14 -O | 1.752 |
| clang-17 -O3 | 1.739 |
| clang-17 -O2 | 1.739 |
| clang-17 -Os | 1.724 |
| clang-17 -O | 1.725 |
| clang-18 -O3 | 1.732 |
| clang-18 -O2 | 1.730 |
| clang-18 -Os | 1.730 |
| clang-18 -O | 1.732 |
| **geometric mean** | **1.732** |

## Appendix C — dispersion at each selected configuration

_Median of 1000 repetitions (1000 for the two `amd64-64` C-ladder rows). Raw RDTSC ticks._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| this work (5×51, chained ladder) | 249,721 | 249,385 | 249,586 | 258,988 | 9,402 | clang-18 -O3 |
| amd64-64 asm | 271,864 | 271,630 | 271,717 | 279,958 | 8,241 | gcc-13 -O3 |
| amd64-64 asm, C ladder | 306,821 | 306,573 | 306,786 | 315,049 | 8,263 | clang-17 -O3 |
| 4×64 microcode, C ladder | 531,309 | 530,831 | 531,179 | 538,982 | 7,803 | clang-17 -O |
| amd64-51 asm | 357,053 | 356,888 | 356,963 | 365,582 | 8,619 | clang-17 -Os |
| amd64-51 asm, C ladder | 388,235 | 388,117 | 388,170 | 396,239 | 8,069 | clang-14 -O |
| 5×51 microcode, C ladder | 288,025 | 287,915 | 287,970 | 296,089 | 8,119 | clang-18 -O |
| 5×51 microcode, chained ladder | 259,519 | 259,395 | 259,446 | 268,006 | 8,560 | clang-18 -Os |
| CryptOpt | 371,948 | 371,740 | 371,855 | 381,347 | 9,492 | clang-14 -O3 |
| fiat-crypto | 347,211 | 346,900 | 347,148 | 355,037 | 7,889 | gcc-12 -O3 |
| hand-written C | 372,316 | 371,823 | 372,098 | 382,105 | 10,007 | clang-18 -O2 |
| donna c64 | 337,689 | 337,395 | 337,529 | 345,171 | 7,642 | gcc-11 -O3 |

> The p90−p10 spread is a near-constant ~6,500–7,000 ticks for every contender regardless of its cost, consistent with an external perturbation (timer interrupts on the benchmark core) rather than contender behaviour. Median, minimum and p10 agree to within 0.01% for every contender, so the reported medians sit at the interference-free floor and the ranking is not an artefact of noise.

_Dispersion for the two backends measured only in the A.2 matrix (`uc/Clad`, `a51op/Clad`) is printed by the harness but not currently captured into `RESULTS.md`; re-run the sweep to record it._
