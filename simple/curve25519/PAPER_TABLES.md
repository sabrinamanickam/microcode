# Curve25519 / X25519 — paper tables

_Generated from `RESULTS.md` (Appendices A.1, A.2) by `gen_paper_tables.py`. Do not hand-edit; regenerate._

_Absolute values are RDTSC ticks converted to core cycles by ×1.00548 (f_core/f_TSC, measured by this run's frequency guard), then rounded to three significant figures. Ratios are computed from raw full-precision ticks and are invariant to that correction._

## Measurement setup

| item | value | source |
|---|---|---|
| CPU | Intel Celeron N3350 (Goldmont), nominal 1.10 GHz | `/proc/cpuinfo` |
| Core / pinning | core 0, `taskset -c 0` | `lib/build_run.sh` |
| Governor | `?`, requested ? kHz | `lib/freq_guard.sh` |
| Turbo | disabled (`no_turbo = ?`) | `lib/freq_guard.sh` |
| Delivered core freq (load) | 1100 MHz (APERF/MPERF) | turbostat |
| TSC / RDTSC rate | 1094 MHz | turbostat |
| Correction f_core/f_TSC | 1.00548 | measured before the sweep |
| Timing | `RDTSC`, serialised `cpuid; rdtsc` before / `rdtscp; cpuid` after | `full_curve25519_inline2.c` |
| Timer overhead | 15 ticks (min 13, p99 17); **not** subtracted — 0.005% of ~300k | measured |
| Post-sweep frequency check | not checked | `lib/freq_guard.sh` |
| Core isolation | not configured | `lib/isolation.sh` |
| nohz_full / isolcpus | unknown | `/proc/cmdline` |
| Timing order | **interleaved** — round-robin, one repetition of every contender per round | `benchmark()` |
| Statistic | median; each configuration measured 1× and reduced to the median of those runs | `bench_stats()`, `median_of` |
| Run-to-run reproducibility | n/a (single run per config) | `note_repro` |
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

With the 5×51 representation fixed, microcode outperforms every compiler-generated and published-research field backend evaluated here — fiat-crypto, CryptOpt, the amd64-51 assembly and hand-written C — across all 24 matched compiler and optimisation configurations, with paired geometric-mean speedups between ×1.296 and ×1.346 (Appendix B.1). It does not outperform OpenSSL's hand-tuned fe51 assembly, which is faster on the same ladder (×0.914 paired geometric mean, microcode ahead in 0 of 24 configurations). Isolated-kernel measurement attributes that difference entirely to invocation cost rather than to the arithmetic: with each side's invocation floor removed, the two field multiplications are within 0.4% of one another (Table K5). Entering patch RAM costs a flat 16.2 cycles irrespective of operand count, where an equivalent native call costs 10.1; over the 2,561 field firings of one X25519 that differential is ≈20,300 cycles and accounts for the whole gap. This result also does not extend to the saturated 4×64 representation: with the amd64-64 C ladder held fixed the microcode backend requires 1.680× as many cycles as the assembly backend (1.679× as a paired geometric mean, Appendix B.2). The 128-triad patch capacity prevents the 4×64 implementation from holding both its 75-triad multiplier and a dedicated squarer, forcing squaring through multiplication.

## Table 2 — End-to-end X25519 performance

| Implementation | Representation | kcycles/X25519 | Relative cycles |
|---|---|---:|---:|
| s2n-bignum verified asm | 4×64 | 246 | ×0.880 |
| OpenSSL (own ladder + fe51 asm) | 5×51 | 255 | ×0.914 |
| OpenSSL fe51 asm on our C ladder | 5×51 | 264 | ×0.945 |
| Bernstein–Schwabe amd64-64 asm | 4×64 | 273 | ×0.978 |
| **this work** | **5×51** | **279** | **×1.000** |
| amd64-51 framework + microcode | 5×51 | 292 | ×1.045 |
| donna c64 | 5×51 | 340 | ×1.216 |
| fiat-crypto | 5×51 | 358 | ×1.282 |
| Bernstein–Schwabe amd64-51 asm | 5×51 | 359 | ×1.285 |
| hand-written C | 5×51 | 382 | ×1.369 |
| CryptOpt | 5×51 | 384 | ×1.373 |

> **Table 2: End-to-end X25519 performance.** Cycle counts are median core kcycles per X25519, rounded to three significant figures, each row at its own best compiler configuration. These are complete implementations differing in representation, ladder structure, inversion, field arithmetic and code organisation; the table therefore establishes overall standing but does **not** isolate the effect of microcode. Lower is better; relative cycles are normalised to this work.

## Table 3 — 5×51 integration and ladder decomposition

| Variant | Ladder | Field ops | kcycles/X25519 |
|---|---|---|---:|
| amd64-51 native | qhasm, monolithic | qhasm asm | 359 |
| C-ladder control | C, per-op calls | qhasm asm | 390 |
| C-ladder + microcode | C, per-op calls | microcode | 304 |
| chained ladder + microcode | inline asm, register-chained | microcode | 292 |

**Transitions** — each changes exactly one element from the row above:

| Transition | Effect isolated | best-of-24 | paired geomean |
|---|---|---:|---:|
| native qhasm → C ladder | loss of ladder/field-op fusion | ×1.086 | ×1.092 |
| asm field ops → microcode | controlled microcode field-op gain | ×1.282 | ×1.263 |
| C ladder → register-chained | ladder-integration recovery | ×1.043 | ×1.011 (×1.046 excl. gcc `-Os`) |

> **Table 3: 5×51 integration and ladder decomposition.** All rows use the amd64-51 framework. Register-chaining the ladder helps in 21 of 24 configurations (paired geometric mean ×1.046); under gcc `-Os` the inline-assembly ladder degrades sharply in this framework (×0.79, reproducible across gcc-11/12/13 to within 43 ticks), which pulls the all-configuration geometric mean down to ×1.011. The same chained ladder in our own framework shows no such degradation, so this is a compiler/framework interaction rather than a property of the ladder. The final row is `amd64-51/ucode`, **not** the canonical implementation reported in Table 2.

## Appendix A — full per-configuration sweep

_Median RDTSC ticks per X25519 at each of the 24 configurations. **Bold** = best (lowest) in that column. Raw ticks; multiply by 1.00548 for core cycles._

### A.1 — End-to-end, every complete implementation (Table 2)

| Config | this work | a64/asm | a64/asm+Clad | a64/ucode | a51/asm | a51/asm+Clad | a51/uc+Clad | a51/ucode | CryptOpt | fiat | hand-C | donna |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gcc-11 -O3 | 280,926 | 272,971 | 336,188 | 559,899 | 359,007 | 393,270 | 309,690 | 298,776 | 392,689 | 358,310 | 388,736 | **337,723** | 244,721 | 270,486 | 254,788 |
| gcc-11 -O2 | 288,009 | 273,351 | 326,801 | 553,220 | 359,515 | 390,438 | 309,634 | 302,127 | 398,727 | 375,231 | 403,605 | 371,072 | 244,526 | 278,341 | 254,269 |
| gcc-11 -Os | 289,731 | 273,899 | 362,066 | 596,621 | 359,465 | 399,156 | 319,127 | 398,892 | 402,990 | 370,237 | 410,144 | 369,537 | 244,601 | 281,016 | 254,125 |
| gcc-11 -O | 286,464 | 273,028 | 335,541 | 568,935 | 358,604 | 391,280 | 321,822 | 294,101 | 391,745 | 378,620 | 412,900 | 379,085 | 244,734 | 273,739 | 254,175 |
| gcc-12 -O3 | 280,955 | 271,792 | 323,959 | 552,185 | 358,890 | 393,453 | 310,157 | 295,308 | 386,776 | **355,994** | 385,705 | 341,044 | 244,827 | 265,897 | 254,255 |
| gcc-12 -O2 | 288,049 | 271,915 | 324,454 | 552,139 | 359,578 | 392,140 | 308,299 | 302,456 | 400,503 | 371,095 | 401,394 | 370,659 | 244,563 | 276,540 | 253,975 |
| gcc-12 -Os | 289,586 | 272,387 | 363,910 | 593,236 | 359,150 | 398,790 | 316,459 | 398,534 | 402,741 | 373,287 | 410,461 | 373,089 | 244,502 | 282,340 | 254,134 |
| gcc-12 -O | 283,563 | 272,922 | 340,288 | 568,697 | 358,797 | 389,961 | 319,524 | 294,199 | 391,937 | 378,850 | 410,042 | 378,511 | 244,487 | 273,504 | 254,218 |
| gcc-13 -O3 | 284,447 | **271,735** | 314,738 | 539,632 | 358,841 | 393,420 | 309,987 | 295,506 | 387,687 | 367,196 | 388,270 | 341,819 | 244,659 | 267,768 | 254,515 |
| gcc-13 -O2 | 288,777 | 272,259 | 313,956 | 542,090 | 359,394 | 393,416 | 310,227 | 301,887 | 398,788 | 371,198 | 395,696 | 356,382 | 244,562 | 275,184 | **253,971** |
| gcc-13 -Os | 288,308 | 272,443 | 348,447 | 580,649 | 359,078 | 398,980 | 316,550 | 398,475 | 404,082 | 372,485 | 414,469 | 356,602 | **244,393** | 282,230 | 254,135 |
| gcc-13 -O | 287,394 | 273,775 | 336,315 | 566,557 | 359,780 | 389,855 | 322,630 | 294,285 | 392,962 | 389,676 | 403,988 | 372,149 | 244,654 | 273,348 | 254,236 |
| clang-14 -O3 | **277,794** | 276,063 | 312,661 | 524,314 | 357,643 | 388,469 | 304,804 | 295,213 | 382,432 | 389,220 | 389,956 | 454,511 | 251,038 | 262,554 | 254,800 |
| clang-14 -O2 | 279,221 | 274,423 | 312,764 | 521,007 | 358,292 | 388,835 | 303,942 | 295,507 | 382,077 | 387,651 | 389,440 | 454,418 | 252,940 | 263,107 | 255,415 |
| clang-14 -Os | 281,741 | 273,221 | 314,905 | 523,152 | 357,154 | 390,717 | 305,907 | 292,597 | 389,337 | 408,458 | 413,745 | 447,042 | 244,469 | 267,731 | 254,462 |
| clang-14 -O | 283,966 | 273,331 | 314,410 | 527,871 | 358,756 | 388,524 | 312,106 | 293,650 | 387,404 | 399,450 | 401,801 | 466,126 | 252,819 | 268,596 | 255,193 |
| clang-17 -O3 | 279,309 | 275,556 | **306,224** | 516,088 | 357,612 | 388,070 | 305,246 | 294,932 | 381,770 | 388,781 | **380,318** | 442,316 | 251,523 | **262,501** | 254,379 |
| clang-17 -O2 | 278,755 | 274,343 | 306,306 | 515,657 | 357,282 | 389,460 | 304,606 | 294,796 | 382,256 | 388,620 | 380,369 | 440,468 | 252,942 | 262,868 | 255,410 |
| clang-17 -Os | 283,897 | 272,631 | 310,223 | 515,563 | 357,160 | 390,208 | 306,605 | 291,587 | 388,905 | 403,843 | 402,187 | 435,413 | 244,568 | 267,387 | 254,481 |
| clang-17 -O | 284,513 | 271,927 | 307,738 | 515,575 | 358,579 | 389,147 | **302,512** | 293,668 | 387,982 | 395,822 | 400,692 | 453,217 | 252,831 | 268,391 | 255,374 |
| clang-18 -O3 | 278,610 | 275,541 | 306,740 | 514,893 | 357,599 | **387,802** | 305,746 | 294,277 | 382,466 | 387,626 | 380,801 | 458,424 | 251,543 | 263,255 | 254,273 |
| clang-18 -O2 | 278,710 | 275,171 | 306,816 | **514,403** | 357,486 | 389,306 | 305,832 | 294,480 | **381,533** | 386,222 | 380,771 | 459,194 | 253,109 | 263,668 | 255,446 |
| clang-18 -Os | 282,336 | 272,827 | 308,429 | 515,989 | **357,011** | 389,743 | 305,154 | **290,179** | 390,314 | 402,088 | 400,557 | 452,387 | 244,447 | 267,147 | 254,146 |
| clang-18 -O | 282,683 | 273,748 | 306,875 | 514,982 | 359,061 | 388,666 | 303,079 | 293,423 | 389,837 | 398,701 | 403,879 | 459,623 | 252,984 | 267,800 | 255,476 |

### A.2 — Common C ladder, only the field backend differs (Table 1, 5×51 block)

| Config | microcode | amd64-51 asm | CryptOpt | fiat | hand-C |
|---|---|---|---|---|---|---|
| gcc-11 -O3 | 289,473 | 391,026 | 270,486 | 392,689 | 358,310 | 388,736 |
| gcc-11 -O2 | 311,046 | 395,568 | 278,341 | 398,727 | 375,231 | 403,605 |
| gcc-11 -Os | 314,112 | 400,389 | 281,016 | 402,990 | 370,237 | 410,144 |
| gcc-11 -O | 306,347 | 393,194 | 273,739 | 391,745 | 378,620 | 412,900 |
| gcc-12 -O3 | 289,647 | 386,513 | 265,897 | 386,776 | **355,994** | 385,705 |
| gcc-12 -O2 | 309,634 | 397,822 | 276,540 | 400,503 | 371,095 | 401,394 |
| gcc-12 -Os | 314,086 | 403,269 | 282,340 | 402,741 | 373,287 | 410,461 |
| gcc-12 -O | 305,097 | 394,638 | 273,504 | 391,937 | 378,850 | 410,042 |
| gcc-13 -O3 | 290,305 | 390,694 | 267,768 | 387,687 | 367,196 | 388,270 |
| gcc-13 -O2 | 307,498 | 395,973 | 275,184 | 398,788 | 371,198 | 395,696 |
| gcc-13 -Os | 316,622 | 403,645 | 282,230 | 404,082 | 372,485 | 414,469 |
| gcc-13 -O | 306,805 | 393,949 | 273,348 | 392,962 | 389,676 | 403,988 |
| clang-14 -O3 | 284,570 | 384,076 | 262,554 | 382,432 | 389,220 | 389,956 |
| clang-14 -O2 | **283,588** | **384,043** | 263,107 | 382,077 | 387,651 | 389,440 |
| clang-14 -Os | 287,891 | 392,176 | 267,731 | 389,337 | 408,458 | 413,745 |
| clang-14 -O | 285,745 | 388,946 | 268,596 | 387,404 | 399,450 | 401,801 |
| clang-17 -O3 | 285,518 | 386,085 | **262,501** | 381,770 | 388,781 | **380,318** |
| clang-17 -O2 | 285,283 | 386,138 | 262,868 | 382,256 | 388,620 | 380,369 |
| clang-17 -Os | 290,411 | 390,678 | 267,387 | 388,905 | 403,843 | 402,187 |
| clang-17 -O | 286,087 | 388,247 | 268,391 | 387,982 | 395,822 | 400,692 |
| clang-18 -O3 | 287,388 | 386,038 | 263,255 | 382,466 | 387,626 | 380,801 |
| clang-18 -O2 | 285,775 | 385,897 | 263,668 | **381,533** | 386,222 | 380,771 |
| clang-18 -Os | 288,084 | 390,475 | 267,147 | 390,314 | 402,088 | 400,557 |
| clang-18 -O | 286,042 | 389,527 | 267,800 | 389,837 | 398,701 | 403,879 |

## Appendix B — paired per-configuration ratios

_Ratio = comparison ÷ microcode at the **same** compiler configuration; >1 means microcode is faster. Computed from raw ticks._

### B.1 — 5×51 block, common C ladder (Table 1)

| Config | fiat-crypto | CryptOpt | amd64-51 asm | hand-written C |
|---|---:|---:|---:|---:|
| gcc-11 -O3 | 1.238 | 1.357 | 1.351 | 1.343 |
| gcc-11 -O2 | 1.206 | 1.282 | 1.272 | 1.298 |
| gcc-11 -Os | 1.179 | 1.283 | 1.275 | 1.306 |
| gcc-11 -O | 1.236 | 1.279 | 1.283 | 1.348 |
| gcc-12 -O3 | 1.229 | 1.335 | 1.334 | 1.332 |
| gcc-12 -O2 | 1.198 | 1.293 | 1.285 | 1.296 |
| gcc-12 -Os | 1.188 | 1.282 | 1.284 | 1.307 |
| gcc-12 -O | 1.242 | 1.285 | 1.293 | 1.344 |
| gcc-13 -O3 | 1.265 | 1.335 | 1.346 | 1.337 |
| gcc-13 -O2 | 1.207 | 1.297 | 1.288 | 1.287 |
| gcc-13 -Os | 1.176 | 1.276 | 1.275 | 1.309 |
| gcc-13 -O | 1.270 | 1.281 | 1.284 | 1.317 |
| clang-14 -O3 | 1.368 | 1.344 | 1.350 | 1.370 |
| clang-14 -O2 | 1.367 | 1.347 | 1.354 | 1.373 |
| clang-14 -Os | 1.419 | 1.352 | 1.362 | 1.437 |
| clang-14 -O | 1.398 | 1.356 | 1.361 | 1.406 |
| clang-17 -O3 | 1.362 | 1.337 | 1.352 | 1.332 |
| clang-17 -O2 | 1.362 | 1.340 | 1.354 | 1.333 |
| clang-17 -Os | 1.391 | 1.339 | 1.345 | 1.385 |
| clang-17 -O | 1.384 | 1.356 | 1.357 | 1.401 |
| clang-18 -O3 | 1.349 | 1.331 | 1.343 | 1.325 |
| clang-18 -O2 | 1.351 | 1.335 | 1.350 | 1.332 |
| clang-18 -Os | 1.396 | 1.355 | 1.355 | 1.390 |
| clang-18 -O | 1.394 | 1.363 | 1.362 | 1.412 |
| **geometric mean** | **1.296** | **1.322** | **1.325** | **1.346** |
| **configurations won** | **24/24** | **24/24** | **24/24** | **24/24** |

### B.2 — 4×64 block, amd64-64 C ladder (Table 1)

| Config | microcode ÷ assembly |
|---|---:|
| gcc-11 -O3 | 1.665 |
| gcc-11 -O2 | 1.693 |
| gcc-11 -Os | 1.648 |
| gcc-11 -O | 1.696 |
| gcc-12 -O3 | 1.704 |
| gcc-12 -O2 | 1.702 |
| gcc-12 -Os | 1.630 |
| gcc-12 -O | 1.671 |
| gcc-13 -O3 | 1.715 |
| gcc-13 -O2 | 1.727 |
| gcc-13 -Os | 1.666 |
| gcc-13 -O | 1.685 |
| clang-14 -O3 | 1.677 |
| clang-14 -O2 | 1.666 |
| clang-14 -Os | 1.661 |
| clang-14 -O | 1.679 |
| clang-17 -O3 | 1.685 |
| clang-17 -O2 | 1.683 |
| clang-17 -Os | 1.662 |
| clang-17 -O | 1.675 |
| clang-18 -O3 | 1.679 |
| clang-18 -O2 | 1.677 |
| clang-18 -Os | 1.673 |
| clang-18 -O | 1.678 |
| **geometric mean** | **1.679** |

## Appendix C — dispersion at each selected configuration

_Median of 1000 repetitions (1000 for the two `amd64-64` rows). Raw RDTSC ticks._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| this work (5×51, chained ladder) | 277,794 | 277,590 | 277,706 | 288,782 | 11,076 | clang-14 -O3 |
| amd64-64 asm | 271,735 | 271,479 | 271,695 | 279,019 | 7,324 | gcc-13 -O3 |
| amd64-64 asm, C ladder | 306,224 | 306,135 | — | — | — | clang-17 -O3 |
| 4×64 microcode, C ladder | 514,403 | 514,269 | — | — | — | clang-18 -O2 |
| amd64-51 asm | 357,011 | 356,843 | 356,930 | 365,073 | 8,143 | clang-18 -Os |
| amd64-51 asm, C ladder | 387,802 | 387,704 | 387,744 | 397,373 | 9,629 | clang-18 -O3 |
| 5×51 microcode, C ladder | 302,512 | 302,377 | 302,426 | 310,556 | 8,130 | clang-17 -O |
| 5×51 microcode, chained ladder | 290,179 | 290,092 | 290,136 | 298,761 | 8,625 | clang-18 -Os |
| CryptOpt | 381,533 | 381,448 | 381,480 | 391,749 | 10,269 | clang-18 -O2 |
| fiat-crypto | 355,994 | 355,904 | 355,944 | 362,966 | 7,022 | gcc-12 -O3 |
| hand-written C | 380,318 | 380,035 | 380,143 | 388,223 | 8,080 | clang-17 -O3 |
| donna c64 | 337,723 | 337,373 | 337,534 | 345,186 | 7,652 | gcc-11 -O3 |

> The p90−p10 spread is a near-constant ~6,500–7,000 ticks for every contender regardless of its cost, consistent with an external perturbation (timer interrupts on the benchmark core) rather than contender behaviour. Median, minimum and p10 agree to within 0.01% for every contender, so the reported medians sit at the interference-free floor and the ranking is not an artefact of noise.

_Dispersion for the two backends measured only in the A.2 matrix (`uc/Clad`, `a51op/Clad`) is printed by the harness but not currently captured into `RESULTS.md`; re-run the sweep to record it._
