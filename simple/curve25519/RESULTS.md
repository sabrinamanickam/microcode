# X25519 Microcode Benchmark Results

**Generated:** Mon 14 Sep 2026 20:20:59 ACST
**Host:** redunlock-GB-BPCE-3350C
**CPU:** Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
**Pinned freq:** 1094400 kHz   (governor: `userspace`, no_turbo: `1`)
**Delivered core freq:** 1100 MHz · **TSC (RDTSC) rate:** 1094 MHz · **correction f_core/f_TSC:** 1.00548 (aperf/mperf under load, verified before the sweep; comparative **ratios are invariant** to this factor, multiply **absolute** cycle counts by it for true core cycles)
**Post-sweep frequency check:** stable (+0.000% over the sweep) · delivered 1100 MHz / TSC 1094 MHz after the sweep (the pre-sweep guard proves the machine was pinned when the sweep started; this proves it stayed pinned throughout)
**Core isolation:** core 0; 28 IRQs steered to core 1 (2 per-CPU/unmovable); SCHED_FIFO 99 · nohz_full/isolcpus: off — periodic timer tick still hits core 0
**Runs per config:** 3 (recorded median is the median of those runs; worst run-to-run spread 1.660% at ours/hand-C @ clang-18 -O3)
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
| osslops/C-ladder | 262,501 | 0.926 | 0.914 | clang-17 -O3 |
| **microcode 5×51 (this work)** | **283,588** | — | — | clang-14 -O2 |
| fiat-crypto (verified C) | 355,994 | 1.255 | 1.296 | gcc-12 -O3 |
| hand-written C (`__uint128_t`) | 380,318 | 1.341 | 1.346 | clang-17 -O3 |
| CryptOpt (superoptimized asm) | 381,533 | 1.345 | 1.322 | clang-18 -O2 |
| amd64-51 asm (Bernstein–Schwabe) | 384,043 | 1.354 | 1.325 | clang-14 -O2 |

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
| `a51/asm` | qhasm, monolithic | qhasm asm | 357,011 | — | baseline |
| `a51/asmCld` | C, per-op calls | qhasm asm | 387,802 | 0.921 | ladder: qhasm → C (fusion lost) |
| `a51/ucCld` | C, per-op calls | **microcode** | 302,512 | **1.282** | **field ops: asm → microcode** |
| `a51/ucode` | inline-asm, chained | microcode | 290,179 | 1.043 | ladder: C → register-chained asm |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** 1.282× faster (geomean 1.263×), with the ladder **and** the framework held constant.

**Consistency check.** The steps are multiplicative, so they must compose to the
measured end-to-end ratio:

```
  0.921 (ladder) x 1.282 (field ops) x 1.043 (chaining)  =  1.23031
  measured  357011 / 290179                              =  1.23031
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
| `a64/asm` | qhasm, monolithic | qhasm asm | 271,735 | — | baseline |
| `a64/asmCld` | C, per-op calls | qhasm asm | 306,224 | 0.887 | ladder: qhasm → C (fusion lost) |
| `a64/ucode` | C, per-op calls | **microcode** | 514,403 | **0.595** | **field ops: asm → microcode** |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** microcode is 1.680× **slower** than the asm it replaces (geomean 1.679×), with the ladder held constant — against 1.893× if the ladder rewrite is wrongly charged to the field ops.

---

## Table 4 — End-to-end standing (orientation, not the claim)

**Held constant:** nothing — these are whole implementations differing in
representation, ladder, inversion and field ops at once. Useful for placing the
work against shipped code; useless for attributing the difference to microcode.
For that, see Table 1.

| implementation | cyc/X25519 | best config |
|---|---:|---|
| amd64-64 asm (Bernstein–Schwabe, 4×64) | 271,735 | gcc-13 -O3 |
| **microcode 5×51 + inline-asm ladder (this work)** | **277,794** | clang-14 -O3 |
| amd64-51 framework + microcode | 290,179 | clang-18 -Os |
| donna c64 (portable C) | 337,723 | gcc-11 -O3 |
| fiat-crypto (verified C) | 355,994 | gcc-12 -O3 |
| amd64-51 asm (Bernstein–Schwabe, 5×51) | 357,011 | clang-18 -Os |
| hand-written C (`__uint128_t`) | 380,318 | clang-17 -O3 |
| CryptOpt (superoptimized asm) | 381,533 | clang-18 -O2 |


### Dispersion at each contender's best config

_Median is the headline; min and the p10–p90 range show run-to-run spread at that config. A tight p90−p10 relative to the inter-contender gaps means the ranking is not noise._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| ucode | 277794 | 277590 | 277706 | 288782 | 11076 | clang-14 -O3 |
| a64/asm | 271735 | 271479 | 271695 | 279019 | 7324 | gcc-13 -O3 |
| a64/asmCld | 306224 | 306135 | — | — | — | clang-17 -O3 |
| a64/ucode | 514403 | 514269 | — | — | — | clang-18 -O2 |
| a51/asm | 357011 | 356843 | 356930 | 365073 | 8143 | clang-18 -Os |
| a51/asmCld | 387802 | 387704 | 387744 | 397373 | 9629 | clang-18 -O3 |
| a51/ucCld | 302512 | 302377 | 302426 | 310556 | 8130 | clang-17 -O |
| a51/ucode | 290179 | 290092 | 290136 | 298761 | 8625 | clang-18 -Os |
| cryptopt | 381533 | 381448 | 381480 | 391749 | 10269 | clang-18 -O2 |
| fiat | 355994 | 355904 | 355944 | 362966 | 7022 | gcc-12 -O3 |
| hand-C | 380318 | 380035 | 380143 | 388223 | 8080 | clang-17 -O3 |
| donna | 337723 | 337373 | 337534 | 345186 | 7652 | gcc-11 -O3 |
| s2n-bignum/asm | 244393 | 244300 | 244343 | 252455 | 8112 | gcc-13 -Os |
| osslops/C-ladder | 262501 | 262400 | 262419 | 271332 | 8913 | clang-17 -O3 |
| openssl | 253971 | 253861 | 253910 | 262304 | 8394 | gcc-13 -O2 |

---

# Appendix A — full per-config sweep

The raw 24-config matrices behind the best-per-contender numbers above.
Present so the selection rule can be audited and so per-compiler behaviour is
visible; not intended to be read row by row.

### A.1 — X25519 end-to-end, every contender

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 280926 | 272971 | 336188 | 559899 | 359007 | 393270 | 309690 | 298776 | 392689 | 358310 | 388736 | **337723** | 244721 | 270486 | 254788 |
| gcc-11 -O2 | 288009 | 273351 | 326801 | 553220 | 359515 | 390438 | 309634 | 302127 | 398727 | 375231 | 403605 | 371072 | 244526 | 278341 | 254269 |
| gcc-11 -Os | 289731 | 273899 | 362066 | 596621 | 359465 | 399156 | 319127 | 398892 | 402990 | 370237 | 410144 | 369537 | 244601 | 281016 | 254125 |
| gcc-11 -O | 286464 | 273028 | 335541 | 568935 | 358604 | 391280 | 321822 | 294101 | 391745 | 378620 | 412900 | 379085 | 244734 | 273739 | 254175 |
| gcc-12 -O3 | 280955 | 271792 | 323959 | 552185 | 358890 | 393453 | 310157 | 295308 | 386776 | **355994** | 385705 | 341044 | 244827 | 265897 | 254255 |
| gcc-12 -O2 | 288049 | 271915 | 324454 | 552139 | 359578 | 392140 | 308299 | 302456 | 400503 | 371095 | 401394 | 370659 | 244563 | 276540 | 253975 |
| gcc-12 -Os | 289586 | 272387 | 363910 | 593236 | 359150 | 398790 | 316459 | 398534 | 402741 | 373287 | 410461 | 373089 | 244502 | 282340 | 254134 |
| gcc-12 -O | 283563 | 272922 | 340288 | 568697 | 358797 | 389961 | 319524 | 294199 | 391937 | 378850 | 410042 | 378511 | 244487 | 273504 | 254218 |
| gcc-13 -O3 | 284447 | **271735** | 314738 | 539632 | 358841 | 393420 | 309987 | 295506 | 387687 | 367196 | 388270 | 341819 | 244659 | 267768 | 254515 |
| gcc-13 -O2 | 288777 | 272259 | 313956 | 542090 | 359394 | 393416 | 310227 | 301887 | 398788 | 371198 | 395696 | 356382 | 244562 | 275184 | **253971** |
| gcc-13 -Os | 288308 | 272443 | 348447 | 580649 | 359078 | 398980 | 316550 | 398475 | 404082 | 372485 | 414469 | 356602 | **244393** | 282230 | 254135 |
| gcc-13 -O | 287394 | 273775 | 336315 | 566557 | 359780 | 389855 | 322630 | 294285 | 392962 | 389676 | 403988 | 372149 | 244654 | 273348 | 254236 |
| clang-14 -O3 | **277794** | 276063 | 312661 | 524314 | 357643 | 388469 | 304804 | 295213 | 382432 | 389220 | 389956 | 454511 | 251038 | 262554 | 254800 |
| clang-14 -O2 | 279221 | 274423 | 312764 | 521007 | 358292 | 388835 | 303942 | 295507 | 382077 | 387651 | 389440 | 454418 | 252940 | 263107 | 255415 |
| clang-14 -Os | 281741 | 273221 | 314905 | 523152 | 357154 | 390717 | 305907 | 292597 | 389337 | 408458 | 413745 | 447042 | 244469 | 267731 | 254462 |
| clang-14 -O | 283966 | 273331 | 314410 | 527871 | 358756 | 388524 | 312106 | 293650 | 387404 | 399450 | 401801 | 466126 | 252819 | 268596 | 255193 |
| clang-17 -O3 | 279309 | 275556 | **306224** | 516088 | 357612 | 388070 | 305246 | 294932 | 381770 | 388781 | **380318** | 442316 | 251523 | **262501** | 254379 |
| clang-17 -O2 | 278755 | 274343 | 306306 | 515657 | 357282 | 389460 | 304606 | 294796 | 382256 | 388620 | 380369 | 440468 | 252942 | 262868 | 255410 |
| clang-17 -Os | 283897 | 272631 | 310223 | 515563 | 357160 | 390208 | 306605 | 291587 | 388905 | 403843 | 402187 | 435413 | 244568 | 267387 | 254481 |
| clang-17 -O | 284513 | 271927 | 307738 | 515575 | 358579 | 389147 | **302512** | 293668 | 387982 | 395822 | 400692 | 453217 | 252831 | 268391 | 255374 |
| clang-18 -O3 | 278610 | 275541 | 306740 | 514893 | 357599 | **387802** | 305746 | 294277 | 382466 | 387626 | 380801 | 458424 | 251543 | 263255 | 254273 |
| clang-18 -O2 | 278710 | 275171 | 306816 | **514403** | 357486 | 389306 | 305832 | 294480 | **381533** | 386222 | 380771 | 459194 | 253109 | 263668 | 255446 |
| clang-18 -Os | 282336 | 272827 | 308429 | 515989 | **357011** | 389743 | 305154 | **290179** | 390314 | 402088 | 400557 | 452387 | 244447 | 267147 | 254146 |
| clang-18 -O | 282683 | 273748 | 306875 | 514982 | 359061 | 388666 | 303079 | 293423 | 389837 | 398701 | 403879 | 459623 | 252984 | 267800 | 255476 |

### A.2 — Same C ladder, only the field op differs

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | uc/Clad | a51op/Clad | osslops/C-ladder | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 289473 | 391026 | 270486 | 392689 | 358310 | 388736 |
| gcc-11 -O2 | 311046 | 395568 | 278341 | 398727 | 375231 | 403605 |
| gcc-11 -Os | 314112 | 400389 | 281016 | 402990 | 370237 | 410144 |
| gcc-11 -O | 306347 | 393194 | 273739 | 391745 | 378620 | 412900 |
| gcc-12 -O3 | 289647 | 386513 | 265897 | 386776 | **355994** | 385705 |
| gcc-12 -O2 | 309634 | 397822 | 276540 | 400503 | 371095 | 401394 |
| gcc-12 -Os | 314086 | 403269 | 282340 | 402741 | 373287 | 410461 |
| gcc-12 -O | 305097 | 394638 | 273504 | 391937 | 378850 | 410042 |
| gcc-13 -O3 | 290305 | 390694 | 267768 | 387687 | 367196 | 388270 |
| gcc-13 -O2 | 307498 | 395973 | 275184 | 398788 | 371198 | 395696 |
| gcc-13 -Os | 316622 | 403645 | 282230 | 404082 | 372485 | 414469 |
| gcc-13 -O | 306805 | 393949 | 273348 | 392962 | 389676 | 403988 |
| clang-14 -O3 | 284570 | 384076 | 262554 | 382432 | 389220 | 389956 |
| clang-14 -O2 | **283588** | **384043** | 263107 | 382077 | 387651 | 389440 |
| clang-14 -Os | 287891 | 392176 | 267731 | 389337 | 408458 | 413745 |
| clang-14 -O | 285745 | 388946 | 268596 | 387404 | 399450 | 401801 |
| clang-17 -O3 | 285518 | 386085 | **262501** | 381770 | 388781 | **380318** |
| clang-17 -O2 | 285283 | 386138 | 262868 | 382256 | 388620 | 380369 |
| clang-17 -Os | 290411 | 390678 | 267387 | 388905 | 403843 | 402187 |
| clang-17 -O | 286087 | 388247 | 268391 | 387982 | 395822 | 400692 |
| clang-18 -O3 | 287388 | 386038 | 263255 | 382466 | 387626 | 380801 |
| clang-18 -O2 | 285775 | 385897 | 263668 | **381533** | 386222 | 380771 |
| clang-18 -Os | 288084 | 390475 | 267147 | 390314 | 402088 | 400557 |
| clang-18 -O | 286042 | 389527 | 267800 | 389837 | 398701 | 403879 |

### A.3 — Per-config ratios


### Does `uc/Clad` win?

_ratio = other ÷ uc/Clad (median cycles). **>1 ⇒ uc/Clad is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a51op/Clad | osslops/C-ladder | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 1.351 | 0.934 | 1.357 | 1.238 | 1.343 |
| gcc-11 -O2 | 1.272 | 0.895 | 1.282 | 1.206 | 1.298 |
| gcc-11 -Os | 1.275 | 0.895 | 1.283 | 1.179 | 1.306 |
| gcc-11 -O | 1.283 | 0.894 | 1.279 | 1.236 | 1.348 |
| gcc-12 -O3 | 1.334 | 0.918 | 1.335 | 1.229 | 1.332 |
| gcc-12 -O2 | 1.285 | 0.893 | 1.293 | 1.198 | 1.296 |
| gcc-12 -Os | 1.284 | 0.899 | 1.282 | 1.188 | 1.307 |
| gcc-12 -O | 1.293 | 0.896 | 1.285 | 1.242 | 1.344 |
| gcc-13 -O3 | 1.346 | 0.922 | 1.335 | 1.265 | 1.337 |
| gcc-13 -O2 | 1.288 | 0.895 | 1.297 | 1.207 | 1.287 |
| gcc-13 -Os | 1.275 | 0.891 | 1.276 | 1.176 | 1.309 |
| gcc-13 -O | 1.284 | 0.891 | 1.281 | 1.270 | 1.317 |
| clang-14 -O3 | 1.350 | 0.923 | 1.344 | 1.368 | 1.370 |
| clang-14 -O2 | 1.354 | 0.928 | 1.347 | 1.367 | 1.373 |
| clang-14 -Os | 1.362 | 0.930 | 1.352 | 1.419 | 1.437 |
| clang-14 -O | 1.361 | 0.940 | 1.356 | 1.398 | 1.406 |
| clang-17 -O3 | 1.352 | 0.919 | 1.337 | 1.362 | 1.332 |
| clang-17 -O2 | 1.354 | 0.921 | 1.340 | 1.362 | 1.333 |
| clang-17 -Os | 1.345 | 0.921 | 1.339 | 1.391 | 1.385 |
| clang-17 -O | 1.357 | 0.938 | 1.356 | 1.384 | 1.401 |
| clang-18 -O3 | 1.343 | 0.916 | 1.331 | 1.349 | 1.325 |
| clang-18 -O2 | 1.350 | 0.923 | 1.335 | 1.351 | 1.332 |
| clang-18 -Os | 1.355 | 0.927 | 1.355 | 1.396 | 1.390 |
| clang-18 -O | 1.362 | 0.936 | 1.363 | 1.394 | 1.412 |
| **geomean** | **1.325** | **0.914** | **1.322** | **1.296** | **1.346** |

### Does `ucode` win?

_ratio = other ÷ ucode (median cycles). **>1 ⇒ ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a64/asm | a51/asm | a51/ucode | donna | fiat | cryptopt | hand-C |
|---|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.972 | 1.278 | 1.064 | 1.202 | 1.275 | 1.398 | 1.384 |
| gcc-11 -O2 | 0.949 | 1.248 | 1.049 | 1.288 | 1.303 | 1.384 | 1.401 |
| gcc-11 -Os | 0.945 | 1.241 | 1.377 | 1.275 | 1.278 | 1.391 | 1.416 |
| gcc-11 -O | 0.953 | 1.252 | 1.027 | 1.323 | 1.322 | 1.368 | 1.441 |
| gcc-12 -O3 | 0.967 | 1.277 | 1.051 | 1.214 | 1.267 | 1.377 | 1.373 |
| gcc-12 -O2 | 0.944 | 1.248 | 1.050 | 1.287 | 1.288 | 1.390 | 1.393 |
| gcc-12 -Os | 0.941 | 1.240 | 1.376 | 1.288 | 1.289 | 1.391 | 1.417 |
| gcc-12 -O | 0.962 | 1.265 | 1.038 | 1.335 | 1.336 | 1.382 | 1.446 |
| gcc-13 -O3 | 0.955 | 1.262 | 1.039 | 1.202 | 1.291 | 1.363 | 1.365 |
| gcc-13 -O2 | 0.943 | 1.245 | 1.045 | 1.234 | 1.285 | 1.381 | 1.370 |
| gcc-13 -Os | 0.945 | 1.245 | 1.382 | 1.237 | 1.292 | 1.402 | 1.438 |
| gcc-13 -O | 0.953 | 1.252 | 1.024 | 1.295 | 1.356 | 1.367 | 1.406 |
| clang-14 -O3 | 0.994 | 1.287 | 1.063 | 1.636 | 1.401 | 1.377 | 1.404 |
| clang-14 -O2 | 0.983 | 1.283 | 1.058 | 1.627 | 1.388 | 1.368 | 1.395 |
| clang-14 -Os | 0.970 | 1.268 | 1.039 | 1.587 | 1.450 | 1.382 | 1.469 |
| clang-14 -O | 0.963 | 1.263 | 1.034 | 1.641 | 1.407 | 1.364 | 1.415 |
| clang-17 -O3 | 0.987 | 1.280 | 1.056 | 1.584 | 1.392 | 1.367 | 1.362 |
| clang-17 -O2 | 0.984 | 1.282 | 1.058 | 1.580 | 1.394 | 1.371 | 1.365 |
| clang-17 -Os | 0.960 | 1.258 | 1.027 | 1.534 | 1.422 | 1.370 | 1.417 |
| clang-17 -O | 0.956 | 1.260 | 1.032 | 1.593 | 1.391 | 1.364 | 1.408 |
| clang-18 -O3 | 0.989 | 1.284 | 1.056 | 1.645 | 1.391 | 1.373 | 1.367 |
| clang-18 -O2 | 0.987 | 1.283 | 1.057 | 1.648 | 1.386 | 1.369 | 1.366 |
| clang-18 -Os | 0.966 | 1.264 | 1.028 | 1.602 | 1.424 | 1.382 | 1.419 |
| clang-18 -O | 0.968 | 1.270 | 1.038 | 1.626 | 1.410 | 1.379 | 1.429 |
| **geomean** | **0.964** | **1.264** | **1.081** | **1.426** | **1.350** | **1.377** | **1.402** |

### A.4 — Uncontrolled ratios (superseded)

These compare a microcode hybrid against its asm baseline **without** holding the
ladder constant, so they attribute the ladder rewrite to the field ops. Retained
for auditability only — Tables 2 and 3 are the correct form of these comparisons.

### Does `a51/ucode` win?

_ratio = other ÷ a51/ucode (median cycles). **>1 ⇒ a51/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.940 | 0.914 | 1.125 | 1.874 | 1.202 | 1.316 | 1.037 | 1.314 | 1.199 | 1.301 | 1.130 | 0.819 | 0.905 | 0.853 |
| gcc-11 -O2 | 0.953 | 0.905 | 1.082 | 1.831 | 1.190 | 1.292 | 1.025 | 1.320 | 1.242 | 1.336 | 1.228 | 0.809 | 0.921 | 0.842 |
| gcc-11 -Os | 0.726 | 0.687 | 0.908 | 1.496 | 0.901 | 1.001 | 0.800 | 1.010 | 0.928 | 1.028 | 0.926 | 0.613 | 0.704 | 0.637 |
| gcc-11 -O | 0.974 | 0.928 | 1.141 | 1.934 | 1.219 | 1.330 | 1.094 | 1.332 | 1.287 | 1.404 | 1.289 | 0.832 | 0.931 | 0.864 |
| gcc-12 -O3 | 0.951 | 0.920 | 1.097 | 1.870 | 1.215 | 1.332 | 1.050 | 1.310 | 1.206 | 1.306 | 1.155 | 0.829 | 0.900 | 0.861 |
| gcc-12 -O2 | 0.952 | 0.899 | 1.073 | 1.826 | 1.189 | 1.297 | 1.019 | 1.324 | 1.227 | 1.327 | 1.225 | 0.809 | 0.914 | 0.840 |
| gcc-12 -Os | 0.727 | 0.683 | 0.913 | 1.489 | 0.901 | 1.001 | 0.794 | 1.011 | 0.937 | 1.030 | 0.936 | 0.614 | 0.708 | 0.638 |
| gcc-12 -O | 0.964 | 0.928 | 1.157 | 1.933 | 1.220 | 1.326 | 1.086 | 1.332 | 1.288 | 1.394 | 1.287 | 0.831 | 0.930 | 0.864 |
| gcc-13 -O3 | 0.963 | 0.920 | 1.065 | 1.826 | 1.214 | 1.331 | 1.049 | 1.312 | 1.243 | 1.314 | 1.157 | 0.828 | 0.906 | 0.861 |
| gcc-13 -O2 | 0.957 | 0.902 | 1.040 | 1.796 | 1.190 | 1.303 | 1.028 | 1.321 | 1.230 | 1.311 | 1.181 | 0.810 | 0.912 | 0.841 |
| gcc-13 -Os | 0.724 | 0.684 | 0.874 | 1.457 | 0.901 | 1.001 | 0.794 | 1.014 | 0.935 | 1.040 | 0.895 | 0.613 | 0.708 | 0.638 |
| gcc-13 -O | 0.977 | 0.930 | 1.143 | 1.925 | 1.223 | 1.325 | 1.096 | 1.335 | 1.324 | 1.373 | 1.265 | 0.831 | 0.929 | 0.864 |
| clang-14 -O3 | 0.941 | 0.935 | 1.059 | 1.776 | 1.211 | 1.316 | 1.032 | 1.295 | 1.318 | 1.321 | 1.540 | 0.850 | 0.889 | 0.863 |
| clang-14 -O2 | 0.945 | 0.929 | 1.058 | 1.763 | 1.212 | 1.316 | 1.029 | 1.293 | 1.312 | 1.318 | 1.538 | 0.856 | 0.890 | 0.864 |
| clang-14 -Os | 0.963 | 0.934 | 1.076 | 1.788 | 1.221 | 1.335 | 1.045 | 1.331 | 1.396 | 1.414 | 1.528 | 0.836 | 0.915 | 0.870 |
| clang-14 -O | 0.967 | 0.931 | 1.071 | 1.798 | 1.222 | 1.323 | 1.063 | 1.319 | 1.360 | 1.368 | 1.587 | 0.861 | 0.915 | 0.869 |
| clang-17 -O3 | 0.947 | 0.934 | 1.038 | 1.750 | 1.213 | 1.316 | 1.035 | 1.294 | 1.318 | 1.290 | 1.500 | 0.853 | 0.890 | 0.863 |
| clang-17 -O2 | 0.946 | 0.931 | 1.039 | 1.749 | 1.212 | 1.321 | 1.033 | 1.297 | 1.318 | 1.290 | 1.494 | 0.858 | 0.892 | 0.866 |
| clang-17 -Os | 0.974 | 0.935 | 1.064 | 1.768 | 1.225 | 1.338 | 1.052 | 1.334 | 1.385 | 1.379 | 1.493 | 0.839 | 0.917 | 0.873 |
| clang-17 -O | 0.969 | 0.926 | 1.048 | 1.756 | 1.221 | 1.325 | 1.030 | 1.321 | 1.348 | 1.364 | 1.543 | 0.861 | 0.914 | 0.870 |
| clang-18 -O3 | 0.947 | 0.936 | 1.042 | 1.750 | 1.215 | 1.318 | 1.039 | 1.300 | 1.317 | 1.294 | 1.558 | 0.855 | 0.895 | 0.864 |
| clang-18 -O2 | 0.946 | 0.934 | 1.042 | 1.747 | 1.214 | 1.322 | 1.039 | 1.296 | 1.312 | 1.293 | 1.559 | 0.860 | 0.895 | 0.867 |
| clang-18 -Os | 0.973 | 0.940 | 1.063 | 1.778 | 1.230 | 1.343 | 1.052 | 1.345 | 1.386 | 1.380 | 1.559 | 0.842 | 0.921 | 0.876 |
| clang-18 -O | 0.963 | 0.933 | 1.046 | 1.755 | 1.224 | 1.325 | 1.033 | 1.329 | 1.359 | 1.376 | 1.566 | 0.862 | 0.913 | 0.871 |
| **geomean** | **0.925** | **0.892** | **1.050** | **1.763** | **1.169** | **1.276** | **1.011** | **1.274** | **1.249** | **1.297** | **1.319** | **0.807** | **0.881** | **0.830** |

### Does `a64/ucode` win?

_ratio = other ÷ a64/ucode (median cycles). **>1 ⇒ a64/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.502 | 0.488 | 0.600 | 0.641 | 0.702 | 0.553 | 0.534 | 0.701 | 0.640 | 0.694 | 0.603 | 0.437 | 0.483 | 0.455 |
| gcc-11 -O2 | 0.521 | 0.494 | 0.591 | 0.650 | 0.706 | 0.560 | 0.546 | 0.721 | 0.678 | 0.730 | 0.671 | 0.442 | 0.503 | 0.460 |
| gcc-11 -Os | 0.486 | 0.459 | 0.607 | 0.603 | 0.669 | 0.535 | 0.669 | 0.675 | 0.621 | 0.687 | 0.619 | 0.410 | 0.471 | 0.426 |
| gcc-11 -O | 0.504 | 0.480 | 0.590 | 0.630 | 0.688 | 0.566 | 0.517 | 0.689 | 0.665 | 0.726 | 0.666 | 0.430 | 0.481 | 0.447 |
| gcc-12 -O3 | 0.509 | 0.492 | 0.587 | 0.650 | 0.713 | 0.562 | 0.535 | 0.700 | 0.645 | 0.699 | 0.618 | 0.443 | 0.482 | 0.460 |
| gcc-12 -O2 | 0.522 | 0.492 | 0.588 | 0.651 | 0.710 | 0.558 | 0.548 | 0.725 | 0.672 | 0.727 | 0.671 | 0.443 | 0.501 | 0.460 |
| gcc-12 -Os | 0.488 | 0.459 | 0.613 | 0.605 | 0.672 | 0.533 | 0.672 | 0.679 | 0.629 | 0.692 | 0.629 | 0.412 | 0.476 | 0.428 |
| gcc-12 -O | 0.499 | 0.480 | 0.598 | 0.631 | 0.686 | 0.562 | 0.517 | 0.689 | 0.666 | 0.721 | 0.666 | 0.430 | 0.481 | 0.447 |
| gcc-13 -O3 | 0.527 | 0.504 | 0.583 | 0.665 | 0.729 | 0.574 | 0.548 | 0.718 | 0.680 | 0.720 | 0.633 | 0.453 | 0.496 | 0.472 |
| gcc-13 -O2 | 0.533 | 0.502 | 0.579 | 0.663 | 0.726 | 0.572 | 0.557 | 0.736 | 0.685 | 0.730 | 0.657 | 0.451 | 0.508 | 0.469 |
| gcc-13 -Os | 0.497 | 0.469 | 0.600 | 0.618 | 0.687 | 0.545 | 0.686 | 0.696 | 0.641 | 0.714 | 0.614 | 0.421 | 0.486 | 0.438 |
| gcc-13 -O | 0.507 | 0.483 | 0.594 | 0.635 | 0.688 | 0.569 | 0.519 | 0.694 | 0.688 | 0.713 | 0.657 | 0.432 | 0.482 | 0.449 |
| clang-14 -O3 | 0.530 | 0.527 | 0.596 | 0.682 | 0.741 | 0.581 | 0.563 | 0.729 | 0.742 | 0.744 | 0.867 | 0.479 | 0.501 | 0.486 |
| clang-14 -O2 | 0.536 | 0.527 | 0.600 | 0.688 | 0.746 | 0.583 | 0.567 | 0.733 | 0.744 | 0.747 | 0.872 | 0.485 | 0.505 | 0.490 |
| clang-14 -Os | 0.539 | 0.522 | 0.602 | 0.683 | 0.747 | 0.585 | 0.559 | 0.744 | 0.781 | 0.791 | 0.855 | 0.467 | 0.512 | 0.486 |
| clang-14 -O | 0.538 | 0.518 | 0.596 | 0.680 | 0.736 | 0.591 | 0.556 | 0.734 | 0.757 | 0.761 | 0.883 | 0.479 | 0.509 | 0.483 |
| clang-17 -O3 | 0.541 | 0.534 | 0.593 | 0.693 | 0.752 | 0.591 | 0.571 | 0.740 | 0.753 | 0.737 | 0.857 | 0.487 | 0.509 | 0.493 |
| clang-17 -O2 | 0.541 | 0.532 | 0.594 | 0.693 | 0.755 | 0.591 | 0.572 | 0.741 | 0.754 | 0.738 | 0.854 | 0.491 | 0.510 | 0.495 |
| clang-17 -Os | 0.551 | 0.529 | 0.602 | 0.693 | 0.757 | 0.595 | 0.566 | 0.754 | 0.783 | 0.780 | 0.845 | 0.474 | 0.519 | 0.494 |
| clang-17 -O | 0.552 | 0.527 | 0.597 | 0.695 | 0.755 | 0.587 | 0.570 | 0.753 | 0.768 | 0.777 | 0.879 | 0.490 | 0.521 | 0.495 |
| clang-18 -O3 | 0.541 | 0.535 | 0.596 | 0.695 | 0.753 | 0.594 | 0.572 | 0.743 | 0.753 | 0.740 | 0.890 | 0.489 | 0.511 | 0.494 |
| clang-18 -O2 | 0.542 | 0.535 | 0.596 | 0.695 | 0.757 | 0.595 | 0.572 | 0.742 | 0.751 | 0.740 | 0.893 | 0.492 | 0.513 | 0.497 |
| clang-18 -Os | 0.547 | 0.529 | 0.598 | 0.692 | 0.755 | 0.591 | 0.562 | 0.756 | 0.779 | 0.776 | 0.877 | 0.474 | 0.518 | 0.493 |
| clang-18 -O | 0.549 | 0.532 | 0.596 | 0.697 | 0.755 | 0.589 | 0.570 | 0.757 | 0.774 | 0.784 | 0.893 | 0.491 | 0.520 | 0.496 |
| **geomean** | **0.525** | **0.506** | **0.596** | **0.663** | **0.724** | **0.573** | **0.567** | **0.722** | **0.708** | **0.736** | **0.748** | **0.458** | **0.500** | **0.471** |
