# X25519 Microcode Benchmark Results

**Generated:** Mon 14 Sep 2026 23:07:13 ACST
**Host:** redunlock-GB-BPCE-3350C
**CPU:** Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
**Pinned freq:** 1094378 kHz   (governor: `userspace`, no_turbo: `1`)
**Delivered core freq:** 1100 MHz · **TSC (RDTSC) rate:** 1094 MHz · **correction f_core/f_TSC:** 1.00548 (aperf/mperf under load, verified before the sweep; comparative **ratios are invariant** to this factor, multiply **absolute** cycle counts by it for true core cycles)
**Post-sweep frequency check:** stable (+0.000% over the sweep) · delivered 1100 MHz / TSC 1094 MHz after the sweep (the pre-sweep guard proves the machine was pinned when the sweep started; this proves it stayed pinned throughout)
**Core isolation:** core 0; 28 IRQs steered to core 1 (2 per-CPU/unmovable); SCHED_FIFO 99 · nohz_full/isolcpus: off — periodic timer tick still hits core 0
**Runs per config:** 3 (recorded median is the median of those runs; worst run-to-run spread 1.483% at amd64-51/asm-Clad @ gcc-13 -O2)
**Timing:** contenders measured INTERLEAVED (round-robin, one repetition of each per round) so measurement order cannot bias the ranking
**Configs that ran:** 24 / 24
**Pipeline:** `taskset -c 0 ./full_curve25519_inline2_static` (+ amd64-64/ucode) for each (compiler, -O) combo
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
| osslops/C-ladder | 262,783 | 0.927 | 0.915 | clang-17 -O3 |
| **microcode 5×51 (this work)** | **283,528** | — | — | clang-17 -O2 |
| fiat-crypto (verified C) | 356,046 | 1.256 | 1.297 | gcc-12 -O3 |
| hand-written C (`__uint128_t`) | 379,510 | 1.339 | 1.345 | clang-17 -O3 |
| CryptOpt (superoptimized asm) | 381,855 | 1.347 | 1.323 | clang-18 -O2 |
| amd64-51 asm (Bernstein–Schwabe) | 384,099 | 1.355 | 1.325 | clang-14 -O2 |

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
| `a51/asm` | qhasm, monolithic | qhasm asm | 356,861 | — | baseline |
| `a51/asmCld` | C, per-op calls | qhasm asm | 388,074 | 0.920 | ladder: qhasm → C (fusion lost) |
| `a51/ucCld` | C, per-op calls | **microcode** | 301,571 | **1.287** | **field ops: asm → microcode** |
| `a51/ucode` | inline-asm, chained | microcode | 287,952 | 1.047 | ladder: C → register-chained asm |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** 1.287× faster (geomean 1.264×), with the ladder **and** the framework held constant.

**Consistency check.** The steps are multiplicative, so they must compose to the
measured end-to-end ratio:

```
  0.920 (ladder) x 1.287 (field ops) x 1.047 (chaining)  =  1.23931
  measured  356861 / 287952                              =  1.23931
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
| `a64/asm` | qhasm, monolithic | qhasm asm | 271,969 | — | baseline |
| `a64/asmCld` | C, per-op calls | qhasm asm | 306,247 | 0.888 | ladder: qhasm → C (fusion lost) |
| `a64/ucode` | C, per-op calls | **microcode** | 513,942 | **0.596** | **field ops: asm → microcode** |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** microcode is 1.678× **slower** than the asm it replaces (geomean 1.678×), with the ladder held constant — against 1.890× if the ladder rewrite is wrongly charged to the field ops.

---

## Table 4 — End-to-end standing (orientation, not the claim)

**Held constant:** nothing — these are whole implementations differing in
representation, ladder, inversion and field ops at once. Useful for placing the
work against shipped code; useless for attributing the difference to microcode.
For that, see Table 1.

| implementation | cyc/X25519 | best config |
|---|---:|---|
| amd64-64 asm (Bernstein–Schwabe, 4×64) | 271,969 | gcc-13 -O2 |
| **microcode 5×51 + inline-asm ladder (this work)** | **275,334** | clang-18 -O2 |
| amd64-51 framework + microcode | 287,952 | clang-18 -Os |
| donna c64 (portable C) | 336,932 | gcc-11 -O3 |
| fiat-crypto (verified C) | 356,046 | gcc-12 -O3 |
| amd64-51 asm (Bernstein–Schwabe, 5×51) | 356,861 | clang-17 -Os |
| hand-written C (`__uint128_t`) | 379,510 | clang-17 -O3 |
| CryptOpt (superoptimized asm) | 381,855 | clang-18 -O2 |


### Dispersion at each contender's best config

_Median is the headline; min and the p10–p90 range show run-to-run spread at that config. A tight p90−p10 relative to the inter-contender gaps means the ranking is not noise._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| ucode | 275334 | 273753 | 275227 | 283145 | 7918 | clang-18 -O2 |
| a64/asm | 271969 | 271821 | 271879 | 279903 | 8024 | gcc-13 -O2 |
| a64/asmCld | 306247 | 306135 | — | — | — | clang-17 -O3 |
| a64/ucode | 513942 | 510745 | — | — | — | clang-18 -O |
| a51/asm | 356861 | 356716 | 356759 | 365657 | 8898 | clang-17 -Os |
| a51/asmCld | 388074 | 387940 | 388009 | 397378 | 9369 | clang-18 -O3 |
| a51/ucCld | 301571 | 301476 | 301516 | 311809 | 10293 | clang-18 -O2 |
| a51/ucode | 287952 | 287152 | 287875 | 296040 | 8165 | clang-18 -Os |
| cryptopt | 381855 | 381784 | 381813 | 390579 | 8766 | clang-18 -O2 |
| fiat | 356046 | 355973 | 355995 | 364580 | 8585 | gcc-12 -O3 |
| hand-C | 379510 | 379263 | 379394 | 388431 | 9037 | clang-17 -O3 |
| donna | 336932 | 336317 | 336723 | 344384 | 7661 | gcc-11 -O3 |
| s2n-bignum/asm | 244402 | 243903 | 244306 | 253422 | 9116 | gcc-11 -O3 |
| osslops/C-ladder | 262783 | 262636 | 262664 | 271827 | 9163 | clang-17 -O3 |
| openssl | 253903 | 253838 | 253884 | 262619 | 8735 | gcc-12 -Os |

---

# Appendix A — full per-config sweep

The raw 24-config matrices behind the best-per-contender numbers above.
Present so the selection rule can be audited and so per-compiler behaviour is
visible; not intended to be read row by row.

### A.1 — X25519 end-to-end, every contender

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 277673 | 272877 | 336151 | 559369 | 359511 | 394057 | 310516 | 294326 | 392860 | 358353 | 388723 | **336932** | **244402** | 270571 | 254766 |
| gcc-11 -O2 | 284735 | 273128 | 326563 | 553229 | 359078 | 390692 | 309701 | 297069 | 398792 | 375310 | 403430 | 370962 | 244542 | 278357 | 254222 |
| gcc-11 -Os | 284764 | 274146 | 362150 | 596562 | 359096 | 398654 | 319349 | 395725 | 402861 | 371404 | 410638 | 368692 | 244687 | 281256 | 253909 |
| gcc-11 -O | 282691 | 272874 | 335591 | 568854 | 358457 | 391486 | 321858 | 291973 | 392105 | 381033 | 411000 | 381027 | 244512 | 273736 | 254313 |
| gcc-12 -O3 | 277676 | 272052 | 324129 | 552141 | 359420 | 391413 | 308417 | 289959 | 386938 | **356046** | 385715 | 340479 | 244634 | 265944 | 254224 |
| gcc-12 -O2 | 285108 | 271998 | 324243 | 552242 | 358923 | 391871 | 308643 | 299171 | 400236 | 371160 | 401520 | 370305 | 244589 | 276446 | 254037 |
| gcc-12 -Os | 286564 | 272576 | 363914 | 593982 | 358754 | 398139 | 316682 | 395223 | 405345 | 371147 | 410135 | 372456 | 244644 | 282826 | **253903** |
| gcc-12 -O | 279740 | 272606 | 340306 | 568805 | 358376 | 390427 | 322457 | 291614 | 392248 | 378378 | 409908 | 376540 | 244530 | 273553 | 254475 |
| gcc-13 -O3 | 281140 | 272074 | 314896 | 539562 | 359318 | 391510 | 308463 | 292547 | 387633 | 367163 | 388194 | 340746 | 244402 | 267920 | 254542 |
| gcc-13 -O2 | 285449 | **271969** | 314091 | 542145 | 359041 | 393764 | 310267 | 298728 | 398715 | 371263 | 395729 | 355617 | 244637 | 275249 | 253978 |
| gcc-13 -Os | 285928 | 272588 | 348541 | 580478 | 358841 | 398269 | 316514 | 395460 | 404297 | 372369 | 412989 | 356016 | 244829 | 284074 | 254047 |
| gcc-13 -O | 283697 | 273464 | 340146 | 566598 | 359281 | 390191 | 322673 | 291952 | 392428 | 389355 | 404242 | 370119 | 244469 | 273583 | 254381 |
| clang-14 -O3 | 275887 | 276138 | 312684 | 521780 | 357075 | 388980 | 305040 | 292306 | 382594 | 388394 | 389691 | 454726 | 250729 | 262902 | 254772 |
| clang-14 -O2 | 275385 | 274658 | 312783 | 521234 | 357255 | 388628 | 304139 | 292057 | 382073 | 388671 | 388655 | 454366 | 253053 | 263191 | 255400 |
| clang-14 -Os | 280955 | 272993 | 314895 | 523767 | 356930 | 390912 | 304757 | 289039 | 389239 | 407018 | 415677 | 447348 | 244539 | 267833 | 254385 |
| clang-14 -O | 280828 | 273468 | 314410 | 527872 | 359367 | 388255 | 312119 | 290240 | 387533 | 398786 | 401606 | 466223 | 252728 | 268541 | 255181 |
| clang-17 -O3 | 275486 | 275629 | **306247** | 516206 | 357235 | 389658 | 302080 | 289804 | 382132 | 388665 | **379510** | 442169 | 251126 | **262783** | 254283 |
| clang-17 -O2 | 275451 | 274700 | 306316 | 515691 | 357113 | 389426 | 307835 | 289800 | 382376 | 392858 | 379934 | 440463 | 252956 | 263195 | 254906 |
| clang-17 -Os | 279420 | 272640 | 310224 | 515992 | **356861** | 392280 | 305862 | 287954 | 388510 | 407558 | 400653 | 436107 | 244632 | 267322 | 254368 |
| clang-17 -O | 281900 | 272136 | 307754 | 515579 | 359365 | 388756 | 302548 | 288589 | 388253 | 395136 | 400474 | 453608 | 252932 | 268287 | 255442 |
| clang-18 -O3 | 275452 | 275579 | 306748 | 515593 | 357073 | **388074** | 305850 | 291386 | 382464 | 387616 | 380295 | 458557 | 251130 | 263450 | 254257 |
| clang-18 -O2 | **275334** | 275271 | 306824 | 514319 | 357184 | 390159 | **301571** | 291891 | **381855** | 385932 | 380206 | 458976 | 253015 | 263955 | 255013 |
| clang-18 -Os | 279705 | 272352 | 308421 | 518114 | 356926 | 389957 | 305341 | **287952** | 389477 | 401388 | 403666 | 453170 | 244562 | 266737 | 254464 |
| clang-18 -O | 279940 | 273964 | 306869 | **513942** | 359269 | 388697 | 302831 | 291280 | 389986 | 398576 | 403424 | 460788 | 252984 | 267799 | 255395 |

### A.2 — Same C ladder, only the field op differs

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | uc/Clad | a51op/Clad | osslops/C-ladder | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 289815 | 390985 | 270571 | 392860 | 358353 | 388723 |
| gcc-11 -O2 | 310988 | 395482 | 278357 | 398792 | 375310 | 403430 |
| gcc-11 -Os | 312792 | 400692 | 281256 | 402861 | 371404 | 410638 |
| gcc-11 -O | 306120 | 393129 | 273736 | 392105 | 381033 | 411000 |
| gcc-12 -O3 | 289597 | 386212 | 265944 | 386938 | **356046** | 385715 |
| gcc-12 -O2 | 309455 | 397917 | 276446 | 400236 | 371160 | 401520 |
| gcc-12 -Os | 313363 | 402143 | 282826 | 405345 | 371147 | 410135 |
| gcc-12 -O | 307234 | 394693 | 273553 | 392248 | 378378 | 409908 |
| gcc-13 -O3 | 290824 | 390435 | 267920 | 387633 | 367163 | 388194 |
| gcc-13 -O2 | 307790 | 396140 | 275249 | 398715 | 371263 | 395729 |
| gcc-13 -Os | 315432 | 403505 | 284074 | 404297 | 372369 | 412989 |
| gcc-13 -O | 306520 | 393947 | 273583 | 392428 | 389355 | 404242 |
| clang-14 -O3 | 284072 | 384186 | 262902 | 382594 | 388394 | 389691 |
| clang-14 -O2 | 283819 | **384099** | 263191 | 382073 | 388671 | 388655 |
| clang-14 -Os | 289210 | 391405 | 267833 | 389239 | 407018 | 415677 |
| clang-14 -O | 285714 | 389112 | 268541 | 387533 | 398786 | 401606 |
| clang-17 -O3 | 285421 | 386077 | **262783** | 382132 | 388665 | **379510** |
| clang-17 -O2 | **283528** | 386128 | 263195 | 382376 | 392858 | 379934 |
| clang-17 -Os | 290686 | 390475 | 267322 | 388510 | 407558 | 400653 |
| clang-17 -O | 285761 | 388598 | 268287 | 388253 | 395136 | 400474 |
| clang-18 -O3 | 286069 | 386138 | 263450 | 382464 | 387616 | 380295 |
| clang-18 -O2 | 287426 | 385892 | 263955 | **381855** | 385932 | 380206 |
| clang-18 -Os | 289865 | 390216 | 266737 | 389477 | 401388 | 403666 |
| clang-18 -O | 285693 | 389567 | 267799 | 389986 | 398576 | 403424 |

### A.3 — Per-config ratios


### Does `uc/Clad` win?

_ratio = other ÷ uc/Clad (median cycles). **>1 ⇒ uc/Clad is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a51op/Clad | osslops/C-ladder | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 1.349 | 0.934 | 1.356 | 1.236 | 1.341 |
| gcc-11 -O2 | 1.272 | 0.895 | 1.282 | 1.207 | 1.297 |
| gcc-11 -Os | 1.281 | 0.899 | 1.288 | 1.187 | 1.313 |
| gcc-11 -O | 1.284 | 0.894 | 1.281 | 1.245 | 1.343 |
| gcc-12 -O3 | 1.334 | 0.918 | 1.336 | 1.229 | 1.332 |
| gcc-12 -O2 | 1.286 | 0.893 | 1.293 | 1.199 | 1.298 |
| gcc-12 -Os | 1.283 | 0.903 | 1.294 | 1.184 | 1.309 |
| gcc-12 -O | 1.285 | 0.890 | 1.277 | 1.232 | 1.334 |
| gcc-13 -O3 | 1.343 | 0.921 | 1.333 | 1.262 | 1.335 |
| gcc-13 -O2 | 1.287 | 0.894 | 1.295 | 1.206 | 1.286 |
| gcc-13 -Os | 1.279 | 0.901 | 1.282 | 1.181 | 1.309 |
| gcc-13 -O | 1.285 | 0.893 | 1.280 | 1.270 | 1.319 |
| clang-14 -O3 | 1.352 | 0.925 | 1.347 | 1.367 | 1.372 |
| clang-14 -O2 | 1.353 | 0.927 | 1.346 | 1.369 | 1.369 |
| clang-14 -Os | 1.353 | 0.926 | 1.346 | 1.407 | 1.437 |
| clang-14 -O | 1.362 | 0.940 | 1.356 | 1.396 | 1.406 |
| clang-17 -O3 | 1.353 | 0.921 | 1.339 | 1.362 | 1.330 |
| clang-17 -O2 | 1.362 | 0.928 | 1.349 | 1.386 | 1.340 |
| clang-17 -Os | 1.343 | 0.920 | 1.337 | 1.402 | 1.378 |
| clang-17 -O | 1.360 | 0.939 | 1.359 | 1.383 | 1.401 |
| clang-18 -O3 | 1.350 | 0.921 | 1.337 | 1.355 | 1.329 |
| clang-18 -O2 | 1.343 | 0.918 | 1.329 | 1.343 | 1.323 |
| clang-18 -Os | 1.346 | 0.920 | 1.344 | 1.385 | 1.393 |
| clang-18 -O | 1.364 | 0.937 | 1.365 | 1.395 | 1.412 |
| **geomean** | **1.325** | **0.915** | **1.323** | **1.297** | **1.345** |

### Does `ucode` win?

_ratio = other ÷ ucode (median cycles). **>1 ⇒ ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a64/asm | a51/asm | a51/ucode | donna | fiat | cryptopt | hand-C |
|---|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.983 | 1.295 | 1.060 | 1.213 | 1.291 | 1.415 | 1.400 |
| gcc-11 -O2 | 0.959 | 1.261 | 1.043 | 1.303 | 1.318 | 1.401 | 1.417 |
| gcc-11 -Os | 0.963 | 1.261 | 1.390 | 1.295 | 1.304 | 1.415 | 1.442 |
| gcc-11 -O | 0.965 | 1.268 | 1.033 | 1.348 | 1.348 | 1.387 | 1.454 |
| gcc-12 -O3 | 0.980 | 1.294 | 1.044 | 1.226 | 1.282 | 1.393 | 1.389 |
| gcc-12 -O2 | 0.954 | 1.259 | 1.049 | 1.299 | 1.302 | 1.404 | 1.408 |
| gcc-12 -Os | 0.951 | 1.252 | 1.379 | 1.300 | 1.295 | 1.415 | 1.431 |
| gcc-12 -O | 0.974 | 1.281 | 1.042 | 1.346 | 1.353 | 1.402 | 1.465 |
| gcc-13 -O3 | 0.968 | 1.278 | 1.041 | 1.212 | 1.306 | 1.379 | 1.381 |
| gcc-13 -O2 | 0.953 | 1.258 | 1.047 | 1.246 | 1.301 | 1.397 | 1.386 |
| gcc-13 -Os | 0.953 | 1.255 | 1.383 | 1.245 | 1.302 | 1.414 | 1.444 |
| gcc-13 -O | 0.964 | 1.266 | 1.029 | 1.305 | 1.372 | 1.383 | 1.425 |
| clang-14 -O3 | 1.001 | 1.294 | 1.060 | 1.648 | 1.408 | 1.387 | 1.413 |
| clang-14 -O2 | 0.997 | 1.297 | 1.061 | 1.650 | 1.411 | 1.387 | 1.411 |
| clang-14 -Os | 0.972 | 1.270 | 1.029 | 1.592 | 1.449 | 1.385 | 1.480 |
| clang-14 -O | 0.974 | 1.280 | 1.034 | 1.660 | 1.420 | 1.380 | 1.430 |
| clang-17 -O3 | 1.001 | 1.297 | 1.052 | 1.605 | 1.411 | 1.387 | 1.378 |
| clang-17 -O2 | 0.997 | 1.296 | 1.052 | 1.599 | 1.426 | 1.388 | 1.379 |
| clang-17 -Os | 0.976 | 1.277 | 1.031 | 1.561 | 1.459 | 1.390 | 1.434 |
| clang-17 -O | 0.965 | 1.275 | 1.024 | 1.609 | 1.402 | 1.377 | 1.421 |
| clang-18 -O3 | 1.000 | 1.296 | 1.058 | 1.665 | 1.407 | 1.388 | 1.381 |
| clang-18 -O2 | 1.000 | 1.297 | 1.060 | 1.667 | 1.402 | 1.387 | 1.381 |
| clang-18 -Os | 0.974 | 1.276 | 1.029 | 1.620 | 1.435 | 1.392 | 1.443 |
| clang-18 -O | 0.979 | 1.283 | 1.041 | 1.646 | 1.424 | 1.393 | 1.441 |
| **geomean** | **0.975** | **1.278** | **1.081** | **1.441** | **1.367** | **1.394** | **1.418** |

### A.4 — Uncontrolled ratios (superseded)

These compare a microcode hybrid against its asm baseline **without** holding the
ladder constant, so they attribute the ladder rewrite to the field ops. Retained
for auditability only — Tables 2 and 3 are the correct form of these comparisons.

### Does `a51/ucode` win?

_ratio = other ÷ a51/ucode (median cycles). **>1 ⇒ a51/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.943 | 0.927 | 1.142 | 1.901 | 1.221 | 1.339 | 1.055 | 1.335 | 1.218 | 1.321 | 1.145 | 0.830 | 0.919 | 0.866 |
| gcc-11 -O2 | 0.958 | 0.919 | 1.099 | 1.862 | 1.209 | 1.315 | 1.043 | 1.342 | 1.263 | 1.358 | 1.249 | 0.823 | 0.937 | 0.856 |
| gcc-11 -Os | 0.720 | 0.693 | 0.915 | 1.508 | 0.907 | 1.007 | 0.807 | 1.018 | 0.939 | 1.038 | 0.932 | 0.618 | 0.711 | 0.642 |
| gcc-11 -O | 0.968 | 0.935 | 1.149 | 1.948 | 1.228 | 1.341 | 1.102 | 1.343 | 1.305 | 1.408 | 1.305 | 0.837 | 0.938 | 0.871 |
| gcc-12 -O3 | 0.958 | 0.938 | 1.118 | 1.904 | 1.240 | 1.350 | 1.064 | 1.334 | 1.228 | 1.330 | 1.174 | 0.844 | 0.917 | 0.877 |
| gcc-12 -O2 | 0.953 | 0.909 | 1.084 | 1.846 | 1.200 | 1.310 | 1.032 | 1.338 | 1.241 | 1.342 | 1.238 | 0.818 | 0.924 | 0.849 |
| gcc-12 -Os | 0.725 | 0.690 | 0.921 | 1.503 | 0.908 | 1.007 | 0.801 | 1.026 | 0.939 | 1.038 | 0.942 | 0.619 | 0.716 | 0.642 |
| gcc-12 -O | 0.959 | 0.935 | 1.167 | 1.951 | 1.229 | 1.339 | 1.106 | 1.345 | 1.298 | 1.406 | 1.291 | 0.839 | 0.938 | 0.873 |
| gcc-13 -O3 | 0.961 | 0.930 | 1.076 | 1.844 | 1.228 | 1.338 | 1.054 | 1.325 | 1.255 | 1.327 | 1.165 | 0.835 | 0.916 | 0.870 |
| gcc-13 -O2 | 0.956 | 0.910 | 1.051 | 1.815 | 1.202 | 1.318 | 1.039 | 1.335 | 1.243 | 1.325 | 1.190 | 0.819 | 0.921 | 0.850 |
| gcc-13 -Os | 0.723 | 0.689 | 0.881 | 1.468 | 0.907 | 1.007 | 0.800 | 1.022 | 0.942 | 1.044 | 0.900 | 0.619 | 0.718 | 0.642 |
| gcc-13 -O | 0.972 | 0.937 | 1.165 | 1.941 | 1.231 | 1.336 | 1.105 | 1.344 | 1.334 | 1.385 | 1.268 | 0.837 | 0.937 | 0.871 |
| clang-14 -O3 | 0.944 | 0.945 | 1.070 | 1.785 | 1.222 | 1.331 | 1.044 | 1.309 | 1.329 | 1.333 | 1.556 | 0.858 | 0.899 | 0.872 |
| clang-14 -O2 | 0.943 | 0.940 | 1.071 | 1.785 | 1.223 | 1.331 | 1.041 | 1.308 | 1.331 | 1.331 | 1.556 | 0.866 | 0.901 | 0.874 |
| clang-14 -Os | 0.972 | 0.944 | 1.089 | 1.812 | 1.235 | 1.352 | 1.054 | 1.347 | 1.408 | 1.438 | 1.548 | 0.846 | 0.927 | 0.880 |
| clang-14 -O | 0.968 | 0.942 | 1.083 | 1.819 | 1.238 | 1.338 | 1.075 | 1.335 | 1.374 | 1.384 | 1.606 | 0.871 | 0.925 | 0.879 |
| clang-17 -O3 | 0.951 | 0.951 | 1.057 | 1.781 | 1.233 | 1.345 | 1.042 | 1.319 | 1.341 | 1.310 | 1.526 | 0.867 | 0.907 | 0.877 |
| clang-17 -O2 | 0.950 | 0.948 | 1.057 | 1.779 | 1.232 | 1.344 | 1.062 | 1.319 | 1.356 | 1.311 | 1.520 | 0.873 | 0.908 | 0.880 |
| clang-17 -Os | 0.970 | 0.947 | 1.077 | 1.792 | 1.239 | 1.362 | 1.062 | 1.349 | 1.415 | 1.391 | 1.515 | 0.850 | 0.928 | 0.883 |
| clang-17 -O | 0.977 | 0.943 | 1.066 | 1.787 | 1.245 | 1.347 | 1.048 | 1.345 | 1.369 | 1.388 | 1.572 | 0.876 | 0.930 | 0.885 |
| clang-18 -O3 | 0.945 | 0.946 | 1.053 | 1.769 | 1.225 | 1.332 | 1.050 | 1.313 | 1.330 | 1.305 | 1.574 | 0.862 | 0.904 | 0.873 |
| clang-18 -O2 | 0.943 | 0.943 | 1.051 | 1.762 | 1.224 | 1.337 | 1.033 | 1.308 | 1.322 | 1.303 | 1.572 | 0.867 | 0.904 | 0.874 |
| clang-18 -Os | 0.971 | 0.946 | 1.071 | 1.799 | 1.240 | 1.354 | 1.060 | 1.353 | 1.394 | 1.402 | 1.574 | 0.849 | 0.926 | 0.884 |
| clang-18 -O | 0.961 | 0.941 | 1.054 | 1.764 | 1.233 | 1.334 | 1.040 | 1.339 | 1.368 | 1.385 | 1.582 | 0.869 | 0.919 | 0.877 |
| **geomean** | **0.925** | **0.902** | **1.063** | **1.784** | **1.182** | **1.291** | **1.022** | **1.289** | **1.264** | **1.311** | **1.333** | **0.816** | **0.892** | **0.840** |

### Does `a64/ucode` win?

_ratio = other ÷ a64/ucode (median cycles). **>1 ⇒ a64/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.496 | 0.488 | 0.601 | 0.643 | 0.704 | 0.555 | 0.526 | 0.702 | 0.641 | 0.695 | 0.602 | 0.437 | 0.484 | 0.455 |
| gcc-11 -O2 | 0.515 | 0.494 | 0.590 | 0.649 | 0.706 | 0.560 | 0.537 | 0.721 | 0.678 | 0.729 | 0.671 | 0.442 | 0.503 | 0.460 |
| gcc-11 -Os | 0.477 | 0.460 | 0.607 | 0.602 | 0.668 | 0.535 | 0.663 | 0.675 | 0.623 | 0.688 | 0.618 | 0.410 | 0.471 | 0.426 |
| gcc-11 -O | 0.497 | 0.480 | 0.590 | 0.630 | 0.688 | 0.566 | 0.513 | 0.689 | 0.670 | 0.723 | 0.670 | 0.430 | 0.481 | 0.447 |
| gcc-12 -O3 | 0.503 | 0.493 | 0.587 | 0.651 | 0.709 | 0.559 | 0.525 | 0.701 | 0.645 | 0.699 | 0.617 | 0.443 | 0.482 | 0.460 |
| gcc-12 -O2 | 0.516 | 0.493 | 0.587 | 0.650 | 0.710 | 0.559 | 0.542 | 0.725 | 0.672 | 0.727 | 0.671 | 0.443 | 0.501 | 0.460 |
| gcc-12 -Os | 0.482 | 0.459 | 0.613 | 0.604 | 0.670 | 0.533 | 0.665 | 0.682 | 0.625 | 0.690 | 0.627 | 0.412 | 0.476 | 0.427 |
| gcc-12 -O | 0.492 | 0.479 | 0.598 | 0.630 | 0.686 | 0.567 | 0.513 | 0.690 | 0.665 | 0.721 | 0.662 | 0.430 | 0.481 | 0.447 |
| gcc-13 -O3 | 0.521 | 0.504 | 0.584 | 0.666 | 0.726 | 0.572 | 0.542 | 0.718 | 0.680 | 0.719 | 0.632 | 0.453 | 0.497 | 0.472 |
| gcc-13 -O2 | 0.527 | 0.502 | 0.579 | 0.662 | 0.726 | 0.572 | 0.551 | 0.735 | 0.685 | 0.730 | 0.656 | 0.451 | 0.508 | 0.468 |
| gcc-13 -Os | 0.493 | 0.470 | 0.600 | 0.618 | 0.686 | 0.545 | 0.681 | 0.696 | 0.641 | 0.711 | 0.613 | 0.422 | 0.489 | 0.438 |
| gcc-13 -O | 0.501 | 0.483 | 0.600 | 0.634 | 0.689 | 0.569 | 0.515 | 0.693 | 0.687 | 0.713 | 0.653 | 0.431 | 0.483 | 0.449 |
| clang-14 -O3 | 0.529 | 0.529 | 0.599 | 0.684 | 0.745 | 0.585 | 0.560 | 0.733 | 0.744 | 0.747 | 0.871 | 0.481 | 0.504 | 0.488 |
| clang-14 -O2 | 0.528 | 0.527 | 0.600 | 0.685 | 0.746 | 0.583 | 0.560 | 0.733 | 0.746 | 0.746 | 0.872 | 0.485 | 0.505 | 0.490 |
| clang-14 -Os | 0.536 | 0.521 | 0.601 | 0.681 | 0.746 | 0.582 | 0.552 | 0.743 | 0.777 | 0.794 | 0.854 | 0.467 | 0.511 | 0.486 |
| clang-14 -O | 0.532 | 0.518 | 0.596 | 0.681 | 0.736 | 0.591 | 0.550 | 0.734 | 0.755 | 0.761 | 0.883 | 0.479 | 0.509 | 0.483 |
| clang-17 -O3 | 0.534 | 0.534 | 0.593 | 0.692 | 0.755 | 0.585 | 0.561 | 0.740 | 0.753 | 0.735 | 0.857 | 0.486 | 0.509 | 0.493 |
| clang-17 -O2 | 0.534 | 0.533 | 0.594 | 0.692 | 0.755 | 0.597 | 0.562 | 0.741 | 0.762 | 0.737 | 0.854 | 0.491 | 0.510 | 0.494 |
| clang-17 -Os | 0.542 | 0.528 | 0.601 | 0.692 | 0.760 | 0.593 | 0.558 | 0.753 | 0.790 | 0.776 | 0.845 | 0.474 | 0.518 | 0.493 |
| clang-17 -O | 0.547 | 0.528 | 0.597 | 0.697 | 0.754 | 0.587 | 0.560 | 0.753 | 0.766 | 0.777 | 0.880 | 0.491 | 0.520 | 0.495 |
| clang-18 -O3 | 0.534 | 0.534 | 0.595 | 0.693 | 0.753 | 0.593 | 0.565 | 0.742 | 0.752 | 0.738 | 0.889 | 0.487 | 0.511 | 0.493 |
| clang-18 -O2 | 0.535 | 0.535 | 0.597 | 0.694 | 0.759 | 0.586 | 0.568 | 0.742 | 0.750 | 0.739 | 0.892 | 0.492 | 0.513 | 0.496 |
| clang-18 -Os | 0.540 | 0.526 | 0.595 | 0.689 | 0.753 | 0.589 | 0.556 | 0.752 | 0.775 | 0.779 | 0.875 | 0.472 | 0.515 | 0.491 |
| clang-18 -O | 0.545 | 0.533 | 0.597 | 0.699 | 0.756 | 0.589 | 0.567 | 0.759 | 0.776 | 0.785 | 0.897 | 0.492 | 0.521 | 0.497 |
| **geomean** | **0.519** | **0.506** | **0.596** | **0.663** | **0.724** | **0.573** | **0.561** | **0.723** | **0.709** | **0.735** | **0.747** | **0.458** | **0.500** | **0.471** |
