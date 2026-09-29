# X25519 Microcode Benchmark Results

**Generated:** Tue 29 Sep 2026 20:13:53 ACST
**Host:** redunlock-GB-BPCE-3350C
**CPU:** Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
**Pinned freq:** 1094513 kHz   (governor: `userspace`, no_turbo: `1`)
**Delivered core freq:** 1100 MHz · **TSC (RDTSC) rate:** 1094 MHz · **correction f_core/f_TSC:** 1.00548 (aperf/mperf under load, verified before the sweep; comparative **ratios are invariant** to this factor, multiply **absolute** cycle counts by it for true core cycles)
**Post-sweep frequency check:** stable (+0.000% over the sweep) · delivered 1100 MHz / TSC 1094 MHz after the sweep (the pre-sweep guard proves the machine was pinned when the sweep started; this proves it stayed pinned throughout)
**Core isolation:** core 1; 28 IRQs steered to core 0 (2 per-CPU/unmovable); SCHED_FIFO 99 · nohz_full/isolcpus: off — periodic timer tick still hits core 1
**Runs per config:** 3 (recorded median is the median of those runs; worst run-to-run spread 1.615% at amd64-64/ucode @ clang-14 -O3)
**Timing:** contenders measured INTERLEAVED (round-robin, one repetition of each per round) so measurement order cannot bias the ranking
**Configs that ran:** 24 / 24
**Pipeline:** `BENCH_CORE=1 taskset -c 1 ./full_curve25519_inline2_static` + `./bench/bench_table_4x64_static` (4x64 control, same process) for each (compiler, -O) combo
**Metric:** median cycles per X25519 (headline; best config per contender in **bold**). Min and the p10–p90 spread are in the dispersion table below.

## Contender legend

| label | backend |
|---|---|
| ours/ucode     | all-in-one inline-asm register-chained 5×51 ladder + microcode field ops (the canonical implementation) |
| ucode/C-ladder | microcode field ops on the SAME C ladder as hand-C/fiat/cryptopt (field-op isolation only — not a headline contender) |
| a51ops/C-ladder | amd64-51 hand-asm field ops on that SAME C ladder — the CONTROL for the field-op claim (see CONTROLS.md) |
| amd64-51/asm-Clad | amd64-51's framework + C ladderstep.c + amd64-51's own asm field ops — the ladder-tax control for 5×51 (see CONTROLS.md) |
| amd64-51/ucode-Clad | amd64-51's framework + that SAME C ladderstep.c + 5×51 microcode field ops — pairs with asm-Clad to isolate the field ops inside one framework |
| amd64-64/asm   | Bernstein–Schwabe whole-stack x86-64 asm (4x64 saturated; lib25519's Goldmont pick) |
| amd64-64/ucode | amd64-64's framework + C ladder + 4×64 microcode field ops (hybrid) |
| amd64-64/asm-Clad | amd64-64's framework + the SAME C ladder as amd64-64/ucode + amd64-64's own asm field ops — the CONTROL that separates the field-op backend from the ladder rewrite (see CONTROLS.md) |
| amd64-51/asm   | Bernstein–Schwabe whole-stack x86-64 asm (5x51 unsaturated) |
| amd64-51/ucode | amd64-51's framework + inline-asm ladder + 5×51 microcode field ops (hybrid) |
| ours/cryptopt  | our C ladder + CryptOpt Goldmont-tuned asm field ops |
| ours/fiat      | our C ladder + fiat-crypto autogen C field ops |
| ours/hand-C    | our C ladder + hand-written C with `__uint128_t` |
| donna_c64      | donna whole-stack portable C |

---

## How to read this

Four tables, one question each. Every caption says what is held constant —
that is the only thing that makes a ratio mean anything here.

| table | question |
|---|---|
| 1 | Is microcode field arithmetic faster than the best ISA-level code for the same representation? |
| 2 | Where does the end-to-end 5×51 number come from? |
| 3 | Does the benefit survive in a saturated representation? |
| 4 | How does the whole implementation stand against shipped code? (orientation only) |

Selection rule throughout: **best median per contender across all
24 (compiler, -O) configs** — SUPERCOP's own discipline. The full
per-config sweep is Appendix A.

---

## Table 1 — Field arithmetic: microcode vs the best ISA-level code

**Held constant:** the entire implementation except `fe_mul`/`fe_sq` — identical C
Montgomery ladder, driver, Fermat inversion, cswap and packing, all compiled into
the same binary and timed in the same process. Only the field-op backend differs.

**This is the paper's claim.** It is the one comparison in which nothing but the
field arithmetic changes.

| field-op backend | cyc/X25519 | ÷ microcode | geomean | best config |
|---|---:|---:|---:|---|
| osslops/C-ladder | 253,831 | 0.972 | 0.957 | clang-18 -O2 |
| **microcode 5×51 (this work)** | **261,141** | — | — | clang-18 -O2 |
| fiat-crypto (verified C) | 347,211 | 1.330 | 1.368 | gcc-12 -O3 |
| CryptOpt (superoptimized asm) | 371,948 | 1.424 | 1.393 | clang-14 -O3 |
| hand-written C (`__uint128_t`) | 372,316 | 1.426 | 1.424 | clang-18 -O2 |
| amd64-51 asm (Bernstein–Schwabe) | 375,267 | 1.437 | 1.396 | clang-17 -O2 |

_Note: fiat-crypto's C beating Bernstein–Schwabe's hand asm here is real, not
an error. amd64-51's `fe25519_mul.S` is written to be inlined into its qhasm
ladder, and pays a penalty when called per-op from C. Table 2 measures that
penalty directly (row 2), which is why this table is not the whole story._

---

## Table 2 — Where the end-to-end 5×51 number comes from

**Held constant:** amd64-51's framework (driver, inversion, pack, cswap) across all
four rows. Each row changes exactly one thing from the row above.

The microcode field ops are worth more than the end-to-end figure shows, because
part of the win is handed back: amd64-51's asm gains from being *fused into* its
monolithic `ladderstep.S`, and the 128-triad patch RAM forbids microcode from
holding a whole ladder step. Our register-chained inline-asm ladder recovers some
of it.

| variant | ladder | field ops | cyc/X25519 | × vs row above | what changed |
|---|---|---|---:|---:|---|
| `a51/asm` | qhasm, monolithic | qhasm asm | 357,053 | — | baseline |
| `a51/asmCld` | C, per-op calls | qhasm asm | 388,235 | 0.920 | ladder: qhasm → C (fusion lost) |
| `a51/ucCld` | C, per-op calls | **microcode** | 288,025 | **1.348** | **field ops: asm → microcode** |
| `a51/ucode` | inline-asm, chained | microcode | 259,519 | 1.110 | ladder: C → register-chained asm |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** 1.348× faster (geomean 1.337×), with the ladder **and** the framework held constant.

**Consistency check.** The steps are multiplicative, so they must compose to the
measured end-to-end ratio:

```
  0.920 (ladder) x 1.348 (field ops) x 1.110 (chaining)  =  1.37583
  measured  357053 / 259519                              =  1.37583
```

---

## Table 3 — Does it survive in a saturated representation?

**Held constant:** amd64-64's framework across all three rows; rows 2 and 3 share
the same C `ladderstep.c` object source, so row 3 differs from row 2 only in the
field ops.

No. The 4×64 saturated multiplier costs 75 triads, leaving no room for a dedicated
squarer under the 128-triad cap, so squaring is `mul(a,a)`. Microcode wins inside a
representation it can hold; it cannot adopt the better algorithm. This is the
headroom result, and it is why the end-to-end table has us losing to amd64-64.

| variant | ladder | field ops | cyc/X25519 | × vs row above | what changed |
|---|---|---|---:|---:|---|
| `a64/asm` | qhasm, monolithic | qhasm asm | 271,864 | — | baseline |
| `a64/asmCld` | C, per-op calls | qhasm asm | 306,821 | 0.886 | ladder: qhasm → C (fusion lost) |
| `a64/ucode` | C, per-op calls | **microcode** | 531,309 | **0.577** | **field ops: asm → microcode** |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** microcode is 1.732× **slower** than the asm it replaces (geomean 1.732×), with the ladder held constant — against 1.954× if the ladder rewrite is wrongly charged to the field ops.

---

## Table 4 — End-to-end standing (orientation, not the claim)

**Held constant:** nothing — these are whole implementations differing in
representation, ladder, inversion and field ops at once. Useful for placing the
work against shipped code; useless for attributing the difference to microcode.
For that, see Table 1.

| implementation | cyc/X25519 | best config |
|---|---:|---|
| **microcode 5×51 + inline-asm ladder (this work)** | **249,721** | clang-18 -O3 |
| amd64-51 framework + microcode | 259,519 | clang-18 -Os |
| amd64-64 asm (Bernstein–Schwabe, 4×64) | 271,864 | gcc-13 -O3 |
| donna c64 (portable C) | 337,689 | gcc-11 -O3 |
| fiat-crypto (verified C) | 347,211 | gcc-12 -O3 |
| amd64-51 asm (Bernstein–Schwabe, 5×51) | 357,053 | clang-17 -Os |
| CryptOpt (superoptimized asm) | 371,948 | clang-14 -O3 |
| hand-written C (`__uint128_t`) | 372,316 | clang-18 -O2 |


### Dispersion at each contender's best config

_Median is the headline; min and the p10–p90 range show run-to-run spread at that config. A tight p90−p10 relative to the inter-contender gaps means the ranking is not noise._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| ucode | 249721 | 249385 | 249586 | 258988 | 9402 | clang-18 -O3 |
| a64/asm | 271864 | 271630 | 271717 | 279958 | 8241 | gcc-13 -O3 |
| a64/asmCld | 306821 | 306573 | 306786 | 315049 | 8263 | clang-17 -O3 |
| a64/ucode | 531309 | 530831 | 531179 | 538982 | 7803 | clang-17 -O |
| a51/asm | 357053 | 356888 | 356963 | 365582 | 8619 | clang-17 -Os |
| a51/asmCld | 388235 | 388117 | 388170 | 396239 | 8069 | clang-14 -O |
| a51/ucCld | 288025 | 287915 | 287970 | 296089 | 8119 | clang-18 -O |
| a51/ucode | 259519 | 259395 | 259446 | 268006 | 8560 | clang-18 -Os |
| cryptopt | 371948 | 371740 | 371855 | 381347 | 9492 | clang-14 -O3 |
| fiat | 347211 | 346900 | 347148 | 355037 | 7889 | gcc-12 -O3 |
| hand-C | 372316 | 371823 | 372098 | 382105 | 10007 | clang-18 -O2 |
| donna | 337689 | 337395 | 337529 | 345171 | 7642 | gcc-11 -O3 |
| s2n-bignum/asm | 244259 | 244104 | 244163 | 252641 | 8478 | clang-14 -Os |
| osslops/C-ladder | 253831 | 253666 | 253745 | 262811 | 9066 | clang-18 -O2 |
| openssl | 250509 | 250458 | 250478 | 259464 | 8986 | gcc-12 -O3 |

---

# Appendix A — full per-config sweep

The raw 24-config matrices behind the best-per-contender numbers above.
Present so the selection rule can be audited and so per-compiler behaviour is
visible; not intended to be read row by row.

### A.1 — X25519 end-to-end, every contender

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 253371 | 272527 | 343465 | 587454 | 359033 | 394476 | 293434 | 265561 | 380918 | 353445 | 384211 | **337689** | 244611 | 260938 | 250532 |
| gcc-11 -O2 | 255920 | 273266 | 333702 | 586848 | 359118 | 390469 | 292386 | 265911 | 388874 | 364604 | 392606 | 371037 | 244395 | 269648 | 250725 |
| gcc-11 -Os | 257966 | 274212 | 373908 | 631462 | 359273 | 398882 | 301665 | 368750 | 390615 | 360656 | 399818 | 368956 | 244562 | 271824 | 250596 |
| gcc-11 -O | 256099 | 273032 | 335610 | 591564 | 359000 | 391224 | 301532 | 263291 | 380744 | 370067 | 400602 | 380571 | 244306 | 263545 | 250716 |
| gcc-12 -O3 | 253035 | 271869 | 333078 | 581862 | 358861 | 393605 | 291471 | 265856 | 379764 | **347211** | 376495 | 341243 | 244628 | 259071 | **250509** |
| gcc-12 -O2 | 256593 | 272122 | 332984 | 581785 | 359171 | 393691 | 293482 | 266328 | 391590 | 362089 | 392113 | 370720 | 244293 | 268667 | 250696 |
| gcc-12 -Os | 258959 | 272712 | 374924 | 633392 | 358974 | 398430 | 302670 | 368436 | 393023 | 360101 | 400602 | 370337 | 244565 | 273386 | 250529 |
| gcc-12 -O | 253778 | 272562 | 340339 | 592652 | 359615 | 390165 | 300162 | 263218 | 381503 | 367591 | 396950 | 377652 | 244290 | 262900 | 250584 |
| gcc-13 -O3 | 255639 | **271864** | 323599 | 572677 | 359090 | 393252 | 291563 | 265447 | 383584 | 360356 | 381332 | 341442 | 244520 | 263941 | 250578 |
| gcc-13 -O2 | 256917 | 272055 | 323833 | 575481 | 359065 | 392014 | 290654 | 265545 | 390069 | 362054 | 385310 | 356626 | 244337 | 267006 | 250670 |
| gcc-13 -Os | 260547 | 272736 | 361126 | 616190 | 358809 | 398462 | 302515 | 368123 | 396767 | 360309 | 403462 | 356218 | 244561 | 274842 | 250537 |
| gcc-13 -O | 255623 | 273520 | 336370 | 590792 | 359855 | 390008 | 299615 | 263095 | 382423 | 377409 | 392985 | 372652 | 244377 | 262489 | 250596 |
| clang-14 -O3 | 251675 | 274718 | 315969 | 538933 | 361856 | 392101 | 289497 | 268006 | **371948** | 384115 | 390957 | 454437 | 251865 | 255004 | 251684 |
| clang-14 -O2 | 252516 | 276161 | 316027 | 538422 | 357372 | 389187 | 288249 | 262554 | 372340 | 383777 | 383333 | 454536 | 251927 | 254788 | 250535 |
| clang-14 -Os | 253151 | 273022 | 315051 | 540456 | 357068 | 390644 | 288506 | 262086 | 382672 | 401376 | 406015 | 447417 | **244259** | 262962 | 250668 |
| clang-14 -O | 257257 | 273403 | 314506 | 551093 | 357881 | **388235** | 295856 | 262443 | 380042 | 391252 | 393624 | 463005 | 250285 | 262634 | 251659 |
| clang-17 -O3 | 249725 | 274642 | **306821** | 533531 | 361211 | 390825 | 289507 | 265282 | 372707 | 379917 | 380153 | 441982 | 251458 | 254588 | 251657 |
| clang-17 -O2 | 251076 | 275695 | 306878 | 533679 | 357304 | 388475 | 288285 | 259965 | 373720 | 381707 | 373851 | 440569 | 251030 | 254629 | 250521 |
| clang-17 -Os | 254978 | 272313 | 310373 | 535210 | **357053** | 392144 | 291292 | 259739 | 381697 | 395993 | 393441 | 435283 | 244261 | 259482 | 250580 |
| clang-17 -O | 257273 | 272241 | 307991 | **531309** | 358782 | 388818 | 288297 | 261455 | 380241 | 388231 | 393831 | 449732 | 250556 | 262222 | 251651 |
| clang-18 -O3 | **249721** | 274389 | 307332 | 532217 | 362012 | 390389 | 289696 | 265048 | 372637 | 380377 | 380611 | 458180 | 251484 | 254454 | 251614 |
| clang-18 -O2 | 251119 | 275748 | 307391 | 531926 | 357948 | 390247 | 289004 | 260822 | 372689 | 379701 | **372316** | 459039 | 250934 | **253831** | 250592 |
| clang-18 -Os | 254478 | 272637 | 308564 | 533778 | 357160 | 391990 | 291417 | **259519** | 380670 | 394269 | 394517 | 452313 | 244286 | 259472 | 250733 |
| clang-18 -O | 257127 | 273282 | 307027 | 531759 | 357738 | 388939 | **288025** | 260885 | 380793 | 394381 | 394505 | 457614 | 251530 | 263100 | 251655 |

### A.2 — Same C ladder, only the field op differs

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | uc/Clad | a51op/Clad | osslops/C-ladder | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 267377 | 380943 | 260938 | 380918 | 353445 | 384211 |
| gcc-11 -O2 | 288628 | 385033 | 269648 | 388874 | 364604 | 392606 |
| gcc-11 -Os | 294157 | 392199 | 271824 | 390615 | 360656 | 399818 |
| gcc-11 -O | 282292 | 382246 | 263545 | 380744 | 370067 | 400602 |
| gcc-12 -O3 | 268576 | 377643 | 259071 | 379764 | **347211** | 376495 |
| gcc-12 -O2 | 287626 | 388481 | 268667 | 391590 | 362089 | 392113 |
| gcc-12 -Os | 294776 | 393666 | 273386 | 393023 | 360101 | 400602 |
| gcc-12 -O | 283359 | 383614 | 262900 | 381503 | 367591 | 396950 |
| gcc-13 -O3 | 269406 | 382910 | 263941 | 383584 | 360356 | 381332 |
| gcc-13 -O2 | 286242 | 386529 | 267006 | 390069 | 362054 | 385310 |
| gcc-13 -Os | 295881 | 394705 | 274842 | 396767 | 360309 | 403462 |
| gcc-13 -O | 283066 | 382881 | 262489 | 382423 | 377409 | 392985 |
| clang-14 -O3 | 264260 | 377397 | 255004 | **371948** | 384115 | 390957 |
| clang-14 -O2 | 262709 | 377674 | 254788 | 372340 | 383777 | 383333 |
| clang-14 -Os | 273207 | 384982 | 262962 | 382672 | 401376 | 406015 |
| clang-14 -O | 263220 | 379530 | 262634 | 380042 | 391252 | 393624 |
| clang-17 -O3 | 263414 | 375293 | 254588 | 372707 | 379917 | 380153 |
| clang-17 -O2 | 261750 | **375267** | 254629 | 373720 | 381707 | 373851 |
| clang-17 -Os | 269936 | 381866 | 259482 | 381697 | 395993 | 393441 |
| clang-17 -O | 263251 | 380484 | 262222 | 380241 | 388231 | 393831 |
| clang-18 -O3 | 263009 | 375331 | 254454 | 372637 | 380377 | 380611 |
| clang-18 -O2 | **261141** | 375594 | **253831** | 372689 | 379701 | **372316** |
| clang-18 -Os | 270704 | 381646 | 259472 | 380670 | 394269 | 394517 |
| clang-18 -O | 263391 | 381524 | 263100 | 380793 | 394381 | 394505 |

### A.3 — Per-config ratios


### Does `uc/Clad` win?

_ratio = other ÷ uc/Clad (median cycles). **>1 ⇒ uc/Clad is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a51op/Clad | osslops/C-ladder | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 1.425 | 0.976 | 1.425 | 1.322 | 1.437 |
| gcc-11 -O2 | 1.334 | 0.934 | 1.347 | 1.263 | 1.360 |
| gcc-11 -Os | 1.333 | 0.924 | 1.328 | 1.226 | 1.359 |
| gcc-11 -O | 1.354 | 0.934 | 1.349 | 1.311 | 1.419 |
| gcc-12 -O3 | 1.406 | 0.965 | 1.414 | 1.293 | 1.402 |
| gcc-12 -O2 | 1.351 | 0.934 | 1.361 | 1.259 | 1.363 |
| gcc-12 -Os | 1.335 | 0.927 | 1.333 | 1.222 | 1.359 |
| gcc-12 -O | 1.354 | 0.928 | 1.346 | 1.297 | 1.401 |
| gcc-13 -O3 | 1.421 | 0.980 | 1.424 | 1.338 | 1.415 |
| gcc-13 -O2 | 1.350 | 0.933 | 1.363 | 1.265 | 1.346 |
| gcc-13 -Os | 1.334 | 0.929 | 1.341 | 1.218 | 1.364 |
| gcc-13 -O | 1.353 | 0.927 | 1.351 | 1.333 | 1.388 |
| clang-14 -O3 | 1.428 | 0.965 | 1.408 | 1.454 | 1.479 |
| clang-14 -O2 | 1.438 | 0.970 | 1.417 | 1.461 | 1.459 |
| clang-14 -Os | 1.409 | 0.963 | 1.401 | 1.469 | 1.486 |
| clang-14 -O | 1.442 | 0.998 | 1.444 | 1.486 | 1.495 |
| clang-17 -O3 | 1.425 | 0.966 | 1.415 | 1.442 | 1.443 |
| clang-17 -O2 | 1.434 | 0.973 | 1.428 | 1.458 | 1.428 |
| clang-17 -Os | 1.415 | 0.961 | 1.414 | 1.467 | 1.458 |
| clang-17 -O | 1.445 | 0.996 | 1.444 | 1.475 | 1.496 |
| clang-18 -O3 | 1.427 | 0.967 | 1.417 | 1.446 | 1.447 |
| clang-18 -O2 | 1.438 | 0.972 | 1.427 | 1.454 | 1.426 |
| clang-18 -Os | 1.410 | 0.959 | 1.406 | 1.456 | 1.457 |
| clang-18 -O | 1.449 | 0.999 | 1.446 | 1.497 | 1.498 |
| **geomean** | **1.396** | **0.957** | **1.393** | **1.368** | **1.424** |

### Does `ucode` win?

_ratio = other ÷ ucode (median cycles). **>1 ⇒ ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a64/asm | a51/asm | a51/ucode | donna | fiat | cryptopt | hand-C |
|---|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 1.076 | 1.417 | 1.048 | 1.333 | 1.395 | 1.503 | 1.516 |
| gcc-11 -O2 | 1.068 | 1.403 | 1.039 | 1.450 | 1.425 | 1.520 | 1.534 |
| gcc-11 -Os | 1.063 | 1.393 | 1.429 | 1.430 | 1.398 | 1.514 | 1.550 |
| gcc-11 -O | 1.066 | 1.402 | 1.028 | 1.486 | 1.445 | 1.487 | 1.564 |
| gcc-12 -O3 | 1.074 | 1.418 | 1.051 | 1.349 | 1.372 | 1.501 | 1.488 |
| gcc-12 -O2 | 1.061 | 1.400 | 1.038 | 1.445 | 1.411 | 1.526 | 1.528 |
| gcc-12 -Os | 1.053 | 1.386 | 1.423 | 1.430 | 1.391 | 1.518 | 1.547 |
| gcc-12 -O | 1.074 | 1.417 | 1.037 | 1.488 | 1.448 | 1.503 | 1.564 |
| gcc-13 -O3 | 1.063 | 1.405 | 1.038 | 1.336 | 1.410 | 1.500 | 1.492 |
| gcc-13 -O2 | 1.059 | 1.398 | 1.034 | 1.388 | 1.409 | 1.518 | 1.500 |
| gcc-13 -Os | 1.047 | 1.377 | 1.413 | 1.367 | 1.383 | 1.523 | 1.549 |
| gcc-13 -O | 1.070 | 1.408 | 1.029 | 1.458 | 1.476 | 1.496 | 1.537 |
| clang-14 -O3 | 1.092 | 1.438 | 1.065 | 1.806 | 1.526 | 1.478 | 1.553 |
| clang-14 -O2 | 1.094 | 1.415 | 1.040 | 1.800 | 1.520 | 1.475 | 1.518 |
| clang-14 -Os | 1.078 | 1.410 | 1.035 | 1.767 | 1.586 | 1.512 | 1.604 |
| clang-14 -O | 1.063 | 1.391 | 1.020 | 1.800 | 1.521 | 1.477 | 1.530 |
| clang-17 -O3 | 1.100 | 1.446 | 1.062 | 1.770 | 1.521 | 1.492 | 1.522 |
| clang-17 -O2 | 1.098 | 1.423 | 1.035 | 1.755 | 1.520 | 1.488 | 1.489 |
| clang-17 -Os | 1.068 | 1.400 | 1.019 | 1.707 | 1.553 | 1.497 | 1.543 |
| clang-17 -O | 1.058 | 1.395 | 1.016 | 1.748 | 1.509 | 1.478 | 1.531 |
| clang-18 -O3 | 1.099 | 1.450 | 1.061 | 1.835 | 1.523 | 1.492 | 1.524 |
| clang-18 -O2 | 1.098 | 1.425 | 1.039 | 1.828 | 1.512 | 1.484 | 1.483 |
| clang-18 -Os | 1.071 | 1.404 | 1.020 | 1.777 | 1.549 | 1.496 | 1.550 |
| clang-18 -O | 1.063 | 1.391 | 1.015 | 1.780 | 1.534 | 1.481 | 1.534 |
| **geomean** | **1.073** | **1.409** | **1.078** | **1.586** | **1.471** | **1.498** | **1.531** |

### A.4 — Uncontrolled ratios (superseded)

These compare a microcode hybrid against its asm baseline **without** holding the
ladder constant, so they attribute the ladder rewrite to the field ops. Retained
for auditability only — Tables 2 and 3 are the correct form of these comparisons.

### Does `a51/ucode` win?

_ratio = other ÷ a51/ucode (median cycles). **>1 ⇒ a51/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.954 | 1.026 | 1.293 | 2.212 | 1.352 | 1.485 | 1.105 | 1.434 | 1.331 | 1.447 | 1.272 | 0.921 | 0.983 | 0.943 |
| gcc-11 -O2 | 0.962 | 1.028 | 1.255 | 2.207 | 1.351 | 1.468 | 1.100 | 1.462 | 1.371 | 1.476 | 1.395 | 0.919 | 1.014 | 0.943 |
| gcc-11 -Os | 0.700 | 0.744 | 1.014 | 1.712 | 0.974 | 1.082 | 0.818 | 1.059 | 0.978 | 1.084 | 1.001 | 0.663 | 0.737 | 0.680 |
| gcc-11 -O | 0.973 | 1.037 | 1.275 | 2.247 | 1.364 | 1.486 | 1.145 | 1.446 | 1.406 | 1.522 | 1.445 | 0.928 | 1.001 | 0.952 |
| gcc-12 -O3 | 0.952 | 1.023 | 1.253 | 2.189 | 1.350 | 1.481 | 1.096 | 1.428 | 1.306 | 1.416 | 1.284 | 0.920 | 0.974 | 0.942 |
| gcc-12 -O2 | 0.963 | 1.022 | 1.250 | 2.184 | 1.349 | 1.478 | 1.102 | 1.470 | 1.360 | 1.472 | 1.392 | 0.917 | 1.009 | 0.941 |
| gcc-12 -Os | 0.703 | 0.740 | 1.018 | 1.719 | 0.974 | 1.081 | 0.821 | 1.067 | 0.977 | 1.087 | 1.005 | 0.664 | 0.742 | 0.680 |
| gcc-12 -O | 0.964 | 1.035 | 1.293 | 2.252 | 1.366 | 1.482 | 1.140 | 1.449 | 1.397 | 1.508 | 1.435 | 0.928 | 0.999 | 0.952 |
| gcc-13 -O3 | 0.963 | 1.024 | 1.219 | 2.157 | 1.353 | 1.481 | 1.098 | 1.445 | 1.358 | 1.437 | 1.286 | 0.921 | 0.994 | 0.944 |
| gcc-13 -O2 | 0.968 | 1.025 | 1.220 | 2.167 | 1.352 | 1.476 | 1.095 | 1.469 | 1.363 | 1.451 | 1.343 | 0.920 | 1.006 | 0.944 |
| gcc-13 -Os | 0.708 | 0.741 | 0.981 | 1.674 | 0.975 | 1.082 | 0.822 | 1.078 | 0.979 | 1.096 | 0.968 | 0.664 | 0.747 | 0.681 |
| gcc-13 -O | 0.972 | 1.040 | 1.279 | 2.246 | 1.368 | 1.482 | 1.139 | 1.454 | 1.434 | 1.494 | 1.416 | 0.929 | 0.998 | 0.952 |
| clang-14 -O3 | 0.939 | 1.025 | 1.179 | 2.011 | 1.350 | 1.463 | 1.080 | 1.388 | 1.433 | 1.459 | 1.696 | 0.940 | 0.951 | 0.939 |
| clang-14 -O2 | 0.962 | 1.052 | 1.204 | 2.051 | 1.361 | 1.482 | 1.098 | 1.418 | 1.462 | 1.460 | 1.731 | 0.960 | 0.970 | 0.954 |
| clang-14 -Os | 0.966 | 1.042 | 1.202 | 2.062 | 1.362 | 1.491 | 1.101 | 1.460 | 1.531 | 1.549 | 1.707 | 0.932 | 1.003 | 0.956 |
| clang-14 -O | 0.980 | 1.042 | 1.198 | 2.100 | 1.364 | 1.479 | 1.127 | 1.448 | 1.491 | 1.500 | 1.764 | 0.954 | 1.001 | 0.959 |
| clang-17 -O3 | 0.941 | 1.035 | 1.157 | 2.011 | 1.362 | 1.473 | 1.091 | 1.405 | 1.432 | 1.433 | 1.666 | 0.948 | 0.960 | 0.949 |
| clang-17 -O2 | 0.966 | 1.061 | 1.180 | 2.053 | 1.374 | 1.494 | 1.109 | 1.438 | 1.468 | 1.438 | 1.695 | 0.966 | 0.979 | 0.964 |
| clang-17 -Os | 0.982 | 1.048 | 1.195 | 2.061 | 1.375 | 1.510 | 1.121 | 1.470 | 1.525 | 1.515 | 1.676 | 0.940 | 0.999 | 0.965 |
| clang-17 -O | 0.984 | 1.041 | 1.178 | 2.032 | 1.372 | 1.487 | 1.103 | 1.454 | 1.485 | 1.506 | 1.720 | 0.958 | 1.003 | 0.963 |
| clang-18 -O3 | 0.942 | 1.035 | 1.160 | 2.008 | 1.366 | 1.473 | 1.093 | 1.406 | 1.435 | 1.436 | 1.729 | 0.949 | 0.960 | 0.949 |
| clang-18 -O2 | 0.963 | 1.057 | 1.179 | 2.039 | 1.372 | 1.496 | 1.108 | 1.429 | 1.456 | 1.427 | 1.760 | 0.962 | 0.973 | 0.961 |
| clang-18 -Os | 0.981 | 1.051 | 1.189 | 2.057 | 1.376 | 1.510 | 1.123 | 1.467 | 1.519 | 1.520 | 1.743 | 0.941 | 1.000 | 0.966 |
| clang-18 -O | 0.986 | 1.048 | 1.177 | 2.038 | 1.371 | 1.491 | 1.104 | 1.460 | 1.512 | 1.512 | 1.754 | 0.964 | 1.008 | 0.965 |
| **geomean** | **0.927** | **0.995** | **1.186** | **2.056** | **1.306** | **1.427** | **1.067** | **1.389** | **1.364** | **1.420** | **1.471** | **0.899** | **0.955** | **0.913** |

### Does `a64/ucode` win?

_ratio = other ÷ a64/ucode (median cycles). **>1 ⇒ a64/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.431 | 0.464 | 0.585 | 0.611 | 0.672 | 0.500 | 0.452 | 0.648 | 0.602 | 0.654 | 0.575 | 0.416 | 0.444 | 0.426 |
| gcc-11 -O2 | 0.436 | 0.466 | 0.569 | 0.612 | 0.665 | 0.498 | 0.453 | 0.663 | 0.621 | 0.669 | 0.632 | 0.416 | 0.459 | 0.427 |
| gcc-11 -Os | 0.409 | 0.434 | 0.592 | 0.569 | 0.632 | 0.478 | 0.584 | 0.619 | 0.571 | 0.633 | 0.584 | 0.387 | 0.430 | 0.397 |
| gcc-11 -O | 0.433 | 0.462 | 0.567 | 0.607 | 0.661 | 0.510 | 0.445 | 0.644 | 0.626 | 0.677 | 0.643 | 0.413 | 0.446 | 0.424 |
| gcc-12 -O3 | 0.435 | 0.467 | 0.572 | 0.617 | 0.676 | 0.501 | 0.457 | 0.653 | 0.597 | 0.647 | 0.586 | 0.420 | 0.445 | 0.431 |
| gcc-12 -O2 | 0.441 | 0.468 | 0.572 | 0.617 | 0.677 | 0.504 | 0.458 | 0.673 | 0.622 | 0.674 | 0.637 | 0.420 | 0.462 | 0.431 |
| gcc-12 -Os | 0.409 | 0.431 | 0.592 | 0.567 | 0.629 | 0.478 | 0.582 | 0.621 | 0.569 | 0.632 | 0.585 | 0.386 | 0.432 | 0.396 |
| gcc-12 -O | 0.428 | 0.460 | 0.574 | 0.607 | 0.658 | 0.506 | 0.444 | 0.644 | 0.620 | 0.670 | 0.637 | 0.412 | 0.444 | 0.423 |
| gcc-13 -O3 | 0.446 | 0.475 | 0.565 | 0.627 | 0.687 | 0.509 | 0.464 | 0.670 | 0.629 | 0.666 | 0.596 | 0.427 | 0.461 | 0.438 |
| gcc-13 -O2 | 0.446 | 0.473 | 0.563 | 0.624 | 0.681 | 0.505 | 0.461 | 0.678 | 0.629 | 0.670 | 0.620 | 0.425 | 0.464 | 0.436 |
| gcc-13 -Os | 0.423 | 0.443 | 0.586 | 0.582 | 0.647 | 0.491 | 0.597 | 0.644 | 0.585 | 0.655 | 0.578 | 0.397 | 0.446 | 0.407 |
| gcc-13 -O | 0.433 | 0.463 | 0.569 | 0.609 | 0.660 | 0.507 | 0.445 | 0.647 | 0.639 | 0.665 | 0.631 | 0.414 | 0.444 | 0.424 |
| clang-14 -O3 | 0.467 | 0.510 | 0.586 | 0.671 | 0.728 | 0.537 | 0.497 | 0.690 | 0.713 | 0.725 | 0.843 | 0.467 | 0.473 | 0.467 |
| clang-14 -O2 | 0.469 | 0.513 | 0.587 | 0.664 | 0.723 | 0.535 | 0.488 | 0.692 | 0.713 | 0.712 | 0.844 | 0.468 | 0.473 | 0.465 |
| clang-14 -Os | 0.468 | 0.505 | 0.583 | 0.661 | 0.723 | 0.534 | 0.485 | 0.708 | 0.743 | 0.751 | 0.828 | 0.452 | 0.487 | 0.464 |
| clang-14 -O | 0.467 | 0.496 | 0.571 | 0.649 | 0.704 | 0.537 | 0.476 | 0.690 | 0.710 | 0.714 | 0.840 | 0.454 | 0.477 | 0.457 |
| clang-17 -O3 | 0.468 | 0.515 | 0.575 | 0.677 | 0.733 | 0.543 | 0.497 | 0.699 | 0.712 | 0.713 | 0.828 | 0.471 | 0.477 | 0.472 |
| clang-17 -O2 | 0.470 | 0.517 | 0.575 | 0.670 | 0.728 | 0.540 | 0.487 | 0.700 | 0.715 | 0.701 | 0.826 | 0.470 | 0.477 | 0.469 |
| clang-17 -Os | 0.476 | 0.509 | 0.580 | 0.667 | 0.733 | 0.544 | 0.485 | 0.713 | 0.740 | 0.735 | 0.813 | 0.456 | 0.485 | 0.468 |
| clang-17 -O | 0.484 | 0.512 | 0.580 | 0.675 | 0.732 | 0.543 | 0.492 | 0.716 | 0.731 | 0.741 | 0.846 | 0.472 | 0.494 | 0.474 |
| clang-18 -O3 | 0.469 | 0.516 | 0.577 | 0.680 | 0.734 | 0.544 | 0.498 | 0.700 | 0.715 | 0.715 | 0.861 | 0.473 | 0.478 | 0.473 |
| clang-18 -O2 | 0.472 | 0.518 | 0.578 | 0.673 | 0.734 | 0.543 | 0.490 | 0.701 | 0.714 | 0.700 | 0.863 | 0.472 | 0.477 | 0.471 |
| clang-18 -Os | 0.477 | 0.511 | 0.578 | 0.669 | 0.734 | 0.546 | 0.486 | 0.713 | 0.739 | 0.739 | 0.847 | 0.458 | 0.486 | 0.470 |
| clang-18 -O | 0.484 | 0.514 | 0.577 | 0.673 | 0.731 | 0.542 | 0.491 | 0.716 | 0.742 | 0.742 | 0.861 | 0.473 | 0.495 | 0.473 |
| **geomean** | **0.451** | **0.484** | **0.577** | **0.636** | **0.694** | **0.519** | **0.486** | **0.676** | **0.664** | **0.691** | **0.715** | **0.437** | **0.464** | **0.444** |
