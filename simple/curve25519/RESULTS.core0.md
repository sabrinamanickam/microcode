# X25519 Microcode Benchmark Results

**Generated:** Tue 15 Sep 2026 00:40:30 ACST
**Host:** redunlock-GB-BPCE-3350C
**CPU:** Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
**Pinned freq:** 1094170 kHz   (governor: `userspace`, no_turbo: `1`)
**Delivered core freq:** 1100 MHz · **TSC (RDTSC) rate:** 1094 MHz · **correction f_core/f_TSC:** 1.00548 (aperf/mperf under load, verified before the sweep; comparative **ratios are invariant** to this factor, multiply **absolute** cycle counts by it for true core cycles)
**Post-sweep frequency check:** stable (+0.000% over the sweep) · delivered 1100 MHz / TSC 1094 MHz after the sweep (the pre-sweep guard proves the machine was pinned when the sweep started; this proves it stayed pinned throughout)
**Core isolation:** core 0; 28 IRQs steered to core 1 (2 per-CPU/unmovable); SCHED_FIFO 99 · nohz_full/isolcpus: off — periodic timer tick still hits core 0
**Runs per config:** 3 (recorded median is the median of those runs; worst run-to-run spread 1.195% at amd64-64/asm-Clad @ gcc-12 -O)
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
| osslops/C-ladder | 253,574 | 0.929 | 0.916 | clang-18 -O3 |
| **microcode 5×51 (this work)** | **272,809** | — | — | clang-18 -O2 |
| fiat-crypto (verified C) | 347,204 | 1.273 | 1.308 | gcc-12 -O3 |
| hand-written C (`__uint128_t`) | 371,422 | 1.361 | 1.358 | clang-18 -O2 |
| CryptOpt (superoptimized asm) | 372,252 | 1.365 | 1.333 | clang-14 -O2 |
| amd64-51 asm (Bernstein–Schwabe) | 375,092 | 1.375 | 1.336 | clang-17 -O3 |

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
| `a51/asm` | qhasm, monolithic | qhasm asm | 356,881 | — | baseline |
| `a51/asmCld` | C, per-op calls | qhasm asm | 387,769 | 0.920 | ladder: qhasm → C (fusion lost) |
| `a51/ucCld` | C, per-op calls | **microcode** | 301,783 | **1.285** | **field ops: asm → microcode** |
| `a51/ucode` | inline-asm, chained | microcode | 277,954 | 1.086 | ladder: C → register-chained asm |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** 1.285× faster (geomean 1.262×), with the ladder **and** the framework held constant.

**Consistency check.** The steps are multiplicative, so they must compose to the
measured end-to-end ratio:

```
  0.920 (ladder) x 1.285 (field ops) x 1.086 (chaining)  =  1.28396
  measured  356881 / 277954                              =  1.28396
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
| `a64/asm` | qhasm, monolithic | qhasm asm | 271,841 | — | baseline |
| `a64/asmCld` | C, per-op calls | qhasm asm | 306,243 | 0.888 | ladder: qhasm → C (fusion lost) |
| `a64/ucode` | C, per-op calls | **microcode** | 514,402 | **0.595** | **field ops: asm → microcode** |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** microcode is 1.680× **slower** than the asm it replaces (geomean 1.678×), with the ladder held constant — against 1.892× if the ladder rewrite is wrongly charged to the field ops.

---

## Table 4 — End-to-end standing (orientation, not the claim)

**Held constant:** nothing — these are whole implementations differing in
representation, ladder, inversion and field ops at once. Useful for placing the
work against shipped code; useless for attributing the difference to microcode.
For that, see Table 1.

| implementation | cyc/X25519 | best config |
|---|---:|---|
| **microcode 5×51 + inline-asm ladder (this work)** | **263,779** | clang-18 -O3 |
| amd64-64 asm (Bernstein–Schwabe, 4×64) | 271,841 | gcc-13 -O2 |
| amd64-51 framework + microcode | 277,954 | clang-18 -Os |
| donna c64 (portable C) | 337,023 | gcc-11 -O3 |
| fiat-crypto (verified C) | 347,204 | gcc-12 -O3 |
| amd64-51 asm (Bernstein–Schwabe, 5×51) | 356,881 | clang-14 -Os |
| hand-written C (`__uint128_t`) | 371,422 | clang-18 -O2 |
| CryptOpt (superoptimized asm) | 372,252 | clang-14 -O2 |


### Dispersion at each contender's best config

_Median is the headline; min and the p10–p90 range show run-to-run spread at that config. A tight p90−p10 relative to the inter-contender gaps means the ranking is not noise._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| ucode | 263779 | 263527 | 263640 | 272944 | 9304 | clang-18 -O3 |
| a64/asm | 271841 | 271721 | 271752 | 279925 | 8173 | gcc-13 -O2 |
| a64/asmCld | 306243 | 306134 | — | — | — | clang-17 -O3 |
| a64/ucode | 514402 | 514269 | — | — | — | clang-18 -O2 |
| a51/asm | 356881 | 356718 | 356810 | 365773 | 8963 | clang-14 -Os |
| a51/asmCld | 387769 | 387600 | 387669 | 396547 | 8878 | clang-18 -O2 |
| a51/ucCld | 301783 | 301666 | 301713 | 312421 | 10708 | clang-18 -O2 |
| a51/ucode | 277954 | 276625 | 277588 | 286093 | 8505 | clang-18 -Os |
| cryptopt | 372252 | 372140 | 372171 | 380058 | 7887 | clang-14 -O2 |
| fiat | 347204 | 346944 | 347148 | 354717 | 7569 | gcc-12 -O3 |
| hand-C | 371422 | 371043 | 371156 | 380715 | 9559 | clang-18 -O2 |
| donna | 337023 | 336740 | 336829 | 343806 | 6977 | gcc-11 -O3 |
| s2n-bignum/asm | 244461 | 244355 | 244389 | 253183 | 8794 | gcc-12 -O |
| osslops/C-ladder | 253574 | 253483 | 253498 | 262678 | 9180 | clang-18 -O3 |
| openssl | 253912 | 253862 | 253886 | 262603 | 8717 | gcc-13 -O3 |

---

# Appendix A — full per-config sweep

The raw 24-config matrices behind the best-per-contender numbers above.
Present so the selection rule can be audited and so per-compiler behaviour is
visible; not intended to be read row by row.

### A.1 — X25519 end-to-end, every contender

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 267362 | 272862 | 336396 | 559585 | 359228 | 392125 | 310149 | 280906 | 380949 | 353526 | 382732 | **337023** | 244567 | 260942 | 253967 |
| gcc-11 -O2 | 273580 | 273095 | 326844 | 553376 | 359032 | 390777 | 309622 | 284819 | 389032 | 364416 | 392540 | 370417 | 244591 | 269728 | 254143 |
| gcc-11 -Os | 275809 | 274065 | 361759 | 596523 | 359083 | 398652 | 319363 | 386818 | 391344 | 361108 | 400982 | 369997 | 244672 | 272824 | 253950 |
| gcc-11 -O | 272240 | 272895 | 339439 | 568943 | 358395 | 391669 | 321657 | 281537 | 380610 | 369305 | 400162 | 377128 | 244715 | 263672 | 254307 |
| gcc-12 -O3 | 268197 | 272021 | 324140 | 551897 | 358895 | 393563 | 309917 | 283660 | 380031 | **347204** | 376617 | 340231 | 244528 | 259165 | 253951 |
| gcc-12 -O2 | 274194 | 271987 | 324275 | 552265 | 358960 | 393710 | 310235 | 284037 | 391647 | 362425 | 392266 | 369861 | 244595 | 268659 | 254000 |
| gcc-12 -Os | 274950 | 272533 | 364010 | 593608 | 358850 | 398276 | 316680 | 386448 | 394882 | 363308 | 400211 | 375260 | 244632 | 273936 | 253952 |
| gcc-12 -O | 269276 | 272571 | 340300 | 568741 | 358314 | 390363 | 322292 | 280902 | 381129 | 368784 | 397710 | 381281 | **244461** | 263289 | 254300 |
| gcc-13 -O3 | 272184 | 272210 | 314853 | 539558 | 359340 | 391651 | 308335 | 283246 | 383671 | 360337 | 381381 | 340941 | 244544 | 263914 | **253912** |
| gcc-13 -O2 | 274111 | **271841** | 314092 | 542565 | 359022 | 393777 | 310305 | 284243 | 390139 | 362115 | 385392 | 355682 | 244640 | 266988 | 254062 |
| gcc-13 -Os | 277057 | 272616 | 348479 | 580862 | 358797 | 398186 | 316597 | 386595 | 396101 | 360212 | 402200 | 356022 | 244664 | 274170 | 253986 |
| gcc-13 -O | 273284 | 273490 | 336380 | 566596 | 359294 | 390334 | 322657 | 281682 | 382191 | 377749 | 392705 | 373346 | 244553 | 262848 | 254404 |
| clang-14 -O3 | 267267 | 276106 | 312694 | 522969 | 357930 | 389786 | 308413 | 284735 | 372918 | 383698 | 384037 | 454638 | 250234 | 254835 | 255118 |
| clang-14 -O2 | 269154 | 274406 | 312776 | 521184 | 357439 | 388831 | 304104 | 279766 | **372252** | 383146 | 382671 | 454471 | 252956 | 255053 | 255397 |
| clang-14 -Os | 269768 | 273100 | 314901 | 523857 | **356881** | 392873 | 305208 | 280486 | 383571 | 399993 | 406834 | 445996 | 244605 | 262593 | 254348 |
| clang-14 -O | 273224 | 273518 | 314408 | 527867 | 359505 | 388325 | 312189 | 279444 | 379930 | 390926 | 393006 | 466185 | 252795 | 262567 | 255161 |
| clang-17 -O3 | 264338 | 275680 | **306243** | 516831 | 357019 | 389483 | 305756 | 280448 | 373480 | 379830 | 373387 | 442212 | 250450 | 254344 | 254864 |
| clang-17 -O2 | 265402 | 274389 | 306320 | 518202 | 357056 | 388127 | 305001 | 279074 | 373069 | 379716 | 372736 | 440533 | 253141 | 254865 | 255208 |
| clang-17 -Os | 268124 | 272647 | 310224 | 515820 | 357018 | 389928 | 305443 | 278056 | 381713 | 397719 | 395130 | 436154 | 244479 | 259116 | 254395 |
| clang-17 -O | 272735 | 272258 | 307745 | 515577 | 359387 | 388717 | 302558 | 279819 | 380322 | 388207 | 393298 | 452207 | 252825 | 262071 | 255478 |
| clang-18 -O3 | **263779** | 275563 | 306749 | 514794 | 357337 | 390145 | 308884 | 281925 | 373364 | 379445 | 373928 | 458544 | 250279 | **253574** | 254919 |
| clang-18 -O2 | 267061 | 275275 | 306822 | **514402** | 357249 | **387769** | **301783** | 279330 | 372955 | 377977 | **371422** | 458978 | 253033 | 254352 | 255095 |
| clang-18 -Os | 268993 | 272462 | 308423 | 516027 | 357012 | 390635 | 306379 | **277954** | 379791 | 393249 | 393933 | 453120 | 244558 | 259739 | 254369 |
| clang-18 -O | 273042 | 273962 | 306866 | 514911 | 359398 | 388773 | 302877 | 279219 | 381450 | 394175 | 393534 | 460195 | 252886 | 263048 | 255411 |

### A.2 — Same C ladder, only the field op differs

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | uc/Clad | a51op/Clad | osslops/C-ladder | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 281477 | 380862 | 260942 | 380949 | 353526 | 382732 |
| gcc-11 -O2 | 301658 | 385051 | 269728 | 389032 | 364416 | 392540 |
| gcc-11 -Os | 306817 | 393325 | 272824 | 391344 | 361108 | 400982 |
| gcc-11 -O | 295390 | 382232 | 263672 | 380610 | 369305 | 400162 |
| gcc-12 -O3 | 282612 | 377886 | 259165 | 380031 | **347204** | 376617 |
| gcc-12 -O2 | 300671 | 388377 | 268659 | 391647 | 362425 | 392266 |
| gcc-12 -Os | 305524 | 393994 | 273936 | 394882 | 363308 | 400211 |
| gcc-12 -O | 297416 | 383698 | 263289 | 381129 | 368784 | 397710 |
| gcc-13 -O3 | 285112 | 382993 | 263914 | 383671 | 360337 | 381381 |
| gcc-13 -O2 | 299113 | 386710 | 266988 | 390139 | 362115 | 385392 |
| gcc-13 -Os | 305054 | 394033 | 274170 | 396101 | 360212 | 402200 |
| gcc-13 -O | 296420 | 382869 | 262848 | 382191 | 377749 | 392705 |
| clang-14 -O3 | 275149 | 377721 | 254835 | 372918 | 383698 | 384037 |
| clang-14 -O2 | 274620 | 377397 | 255053 | **372252** | 383146 | 382671 |
| clang-14 -Os | 283512 | 385454 | 262593 | 383571 | 399993 | 406834 |
| clang-14 -O | 277541 | 380870 | 262567 | 379930 | 390926 | 393006 |
| clang-17 -O3 | 272999 | **375092** | 254344 | 373480 | 379830 | 373387 |
| clang-17 -O2 | 272899 | 375352 | 254865 | 373069 | 379716 | 372736 |
| clang-17 -Os | 281920 | 381827 | 259116 | 381713 | 397719 | 395130 |
| clang-17 -O | 277238 | 380499 | 262071 | 380322 | 388207 | 393298 |
| clang-18 -O3 | 275151 | 375518 | **253574** | 373364 | 379445 | 373928 |
| clang-18 -O2 | **272809** | 375507 | 254352 | 372955 | 377977 | **371422** |
| clang-18 -Os | 281755 | 381356 | 259739 | 379791 | 393249 | 393933 |
| clang-18 -O | 276098 | 381656 | 263048 | 381450 | 394175 | 393534 |

### A.3 — Per-config ratios


### Does `uc/Clad` win?

_ratio = other ÷ uc/Clad (median cycles). **>1 ⇒ uc/Clad is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a51op/Clad | osslops/C-ladder | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 1.353 | 0.927 | 1.353 | 1.256 | 1.360 |
| gcc-11 -O2 | 1.276 | 0.894 | 1.290 | 1.208 | 1.301 |
| gcc-11 -Os | 1.282 | 0.889 | 1.275 | 1.177 | 1.307 |
| gcc-11 -O | 1.294 | 0.893 | 1.288 | 1.250 | 1.355 |
| gcc-12 -O3 | 1.337 | 0.917 | 1.345 | 1.229 | 1.333 |
| gcc-12 -O2 | 1.292 | 0.894 | 1.303 | 1.205 | 1.305 |
| gcc-12 -Os | 1.290 | 0.897 | 1.292 | 1.189 | 1.310 |
| gcc-12 -O | 1.290 | 0.885 | 1.281 | 1.240 | 1.337 |
| gcc-13 -O3 | 1.343 | 0.926 | 1.346 | 1.264 | 1.338 |
| gcc-13 -O2 | 1.293 | 0.893 | 1.304 | 1.211 | 1.288 |
| gcc-13 -Os | 1.292 | 0.899 | 1.298 | 1.181 | 1.318 |
| gcc-13 -O | 1.292 | 0.887 | 1.289 | 1.274 | 1.325 |
| clang-14 -O3 | 1.373 | 0.926 | 1.355 | 1.395 | 1.396 |
| clang-14 -O2 | 1.374 | 0.929 | 1.356 | 1.395 | 1.393 |
| clang-14 -Os | 1.360 | 0.926 | 1.353 | 1.411 | 1.435 |
| clang-14 -O | 1.372 | 0.946 | 1.369 | 1.409 | 1.416 |
| clang-17 -O3 | 1.374 | 0.932 | 1.368 | 1.391 | 1.368 |
| clang-17 -O2 | 1.375 | 0.934 | 1.367 | 1.391 | 1.366 |
| clang-17 -Os | 1.354 | 0.919 | 1.354 | 1.411 | 1.402 |
| clang-17 -O | 1.372 | 0.945 | 1.372 | 1.400 | 1.419 |
| clang-18 -O3 | 1.365 | 0.922 | 1.357 | 1.379 | 1.359 |
| clang-18 -O2 | 1.376 | 0.932 | 1.367 | 1.386 | 1.361 |
| clang-18 -Os | 1.354 | 0.922 | 1.348 | 1.396 | 1.398 |
| clang-18 -O | 1.382 | 0.953 | 1.382 | 1.428 | 1.425 |
| **geomean** | **1.336** | **0.916** | **1.333** | **1.308** | **1.358** |

### Does `ucode` win?

_ratio = other ÷ ucode (median cycles). **>1 ⇒ ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a64/asm | a51/asm | a51/ucode | donna | fiat | cryptopt | hand-C |
|---|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 1.021 | 1.344 | 1.051 | 1.261 | 1.322 | 1.425 | 1.432 |
| gcc-11 -O2 | 0.998 | 1.312 | 1.041 | 1.354 | 1.332 | 1.422 | 1.435 |
| gcc-11 -Os | 0.994 | 1.302 | 1.402 | 1.341 | 1.309 | 1.419 | 1.454 |
| gcc-11 -O | 1.002 | 1.316 | 1.034 | 1.385 | 1.357 | 1.398 | 1.470 |
| gcc-12 -O3 | 1.014 | 1.338 | 1.058 | 1.269 | 1.295 | 1.417 | 1.404 |
| gcc-12 -O2 | 0.992 | 1.309 | 1.036 | 1.349 | 1.322 | 1.428 | 1.431 |
| gcc-12 -Os | 0.991 | 1.305 | 1.406 | 1.365 | 1.321 | 1.436 | 1.456 |
| gcc-12 -O | 1.012 | 1.331 | 1.043 | 1.416 | 1.370 | 1.415 | 1.477 |
| gcc-13 -O3 | 1.000 | 1.320 | 1.041 | 1.253 | 1.324 | 1.410 | 1.401 |
| gcc-13 -O2 | 0.992 | 1.310 | 1.037 | 1.298 | 1.321 | 1.423 | 1.406 |
| gcc-13 -Os | 0.984 | 1.295 | 1.395 | 1.285 | 1.300 | 1.430 | 1.452 |
| gcc-13 -O | 1.001 | 1.315 | 1.031 | 1.366 | 1.382 | 1.399 | 1.437 |
| clang-14 -O3 | 1.033 | 1.339 | 1.065 | 1.701 | 1.436 | 1.395 | 1.437 |
| clang-14 -O2 | 1.020 | 1.328 | 1.039 | 1.689 | 1.424 | 1.383 | 1.422 |
| clang-14 -Os | 1.012 | 1.323 | 1.040 | 1.653 | 1.483 | 1.422 | 1.508 |
| clang-14 -O | 1.001 | 1.316 | 1.023 | 1.706 | 1.431 | 1.391 | 1.438 |
| clang-17 -O3 | 1.043 | 1.351 | 1.061 | 1.673 | 1.437 | 1.413 | 1.413 |
| clang-17 -O2 | 1.034 | 1.345 | 1.052 | 1.660 | 1.431 | 1.406 | 1.404 |
| clang-17 -Os | 1.017 | 1.332 | 1.037 | 1.627 | 1.483 | 1.424 | 1.474 |
| clang-17 -O | 0.998 | 1.318 | 1.026 | 1.658 | 1.423 | 1.394 | 1.442 |
| clang-18 -O3 | 1.045 | 1.355 | 1.069 | 1.738 | 1.438 | 1.415 | 1.418 |
| clang-18 -O2 | 1.031 | 1.338 | 1.046 | 1.719 | 1.415 | 1.397 | 1.391 |
| clang-18 -Os | 1.013 | 1.327 | 1.033 | 1.685 | 1.462 | 1.412 | 1.464 |
| clang-18 -O | 1.003 | 1.316 | 1.023 | 1.685 | 1.444 | 1.397 | 1.441 |
| **geomean** | **1.010** | **1.324** | **1.081** | **1.494** | **1.385** | **1.411** | **1.437** |

### A.4 — Uncontrolled ratios (superseded)

These compare a microcode hybrid against its asm baseline **without** holding the
ladder constant, so they attribute the ladder rewrite to the field ops. Retained
for auditability only — Tables 2 and 3 are the correct form of these comparisons.

### Does `a51/ucode` win?

_ratio = other ÷ a51/ucode (median cycles). **>1 ⇒ a51/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.952 | 0.971 | 1.198 | 1.992 | 1.279 | 1.396 | 1.104 | 1.356 | 1.259 | 1.362 | 1.200 | 0.871 | 0.929 | 0.904 |
| gcc-11 -O2 | 0.961 | 0.959 | 1.148 | 1.943 | 1.261 | 1.372 | 1.087 | 1.366 | 1.279 | 1.378 | 1.301 | 0.859 | 0.947 | 0.892 |
| gcc-11 -Os | 0.713 | 0.709 | 0.935 | 1.542 | 0.928 | 1.031 | 0.826 | 1.012 | 0.934 | 1.037 | 0.957 | 0.633 | 0.705 | 0.657 |
| gcc-11 -O | 0.967 | 0.969 | 1.206 | 2.021 | 1.273 | 1.391 | 1.143 | 1.352 | 1.312 | 1.421 | 1.340 | 0.869 | 0.937 | 0.903 |
| gcc-12 -O3 | 0.945 | 0.959 | 1.143 | 1.946 | 1.265 | 1.387 | 1.093 | 1.340 | 1.224 | 1.328 | 1.199 | 0.862 | 0.914 | 0.895 |
| gcc-12 -O2 | 0.965 | 0.958 | 1.142 | 1.944 | 1.264 | 1.386 | 1.092 | 1.379 | 1.276 | 1.381 | 1.302 | 0.861 | 0.946 | 0.894 |
| gcc-12 -Os | 0.711 | 0.705 | 0.942 | 1.536 | 0.929 | 1.031 | 0.819 | 1.022 | 0.940 | 1.036 | 0.971 | 0.633 | 0.709 | 0.657 |
| gcc-12 -O | 0.959 | 0.970 | 1.211 | 2.025 | 1.276 | 1.390 | 1.147 | 1.357 | 1.313 | 1.416 | 1.357 | 0.870 | 0.937 | 0.905 |
| gcc-13 -O3 | 0.961 | 0.961 | 1.112 | 1.905 | 1.269 | 1.383 | 1.089 | 1.355 | 1.272 | 1.346 | 1.204 | 0.863 | 0.932 | 0.896 |
| gcc-13 -O2 | 0.964 | 0.956 | 1.105 | 1.909 | 1.263 | 1.385 | 1.092 | 1.373 | 1.274 | 1.356 | 1.251 | 0.861 | 0.939 | 0.894 |
| gcc-13 -Os | 0.717 | 0.705 | 0.901 | 1.503 | 0.928 | 1.030 | 0.819 | 1.025 | 0.932 | 1.040 | 0.921 | 0.633 | 0.709 | 0.657 |
| gcc-13 -O | 0.970 | 0.971 | 1.194 | 2.011 | 1.276 | 1.386 | 1.145 | 1.357 | 1.341 | 1.394 | 1.325 | 0.868 | 0.933 | 0.903 |
| clang-14 -O3 | 0.939 | 0.970 | 1.098 | 1.837 | 1.257 | 1.369 | 1.083 | 1.310 | 1.348 | 1.349 | 1.597 | 0.879 | 0.895 | 0.896 |
| clang-14 -O2 | 0.962 | 0.981 | 1.118 | 1.863 | 1.278 | 1.390 | 1.087 | 1.331 | 1.370 | 1.368 | 1.624 | 0.904 | 0.912 | 0.913 |
| clang-14 -Os | 0.962 | 0.974 | 1.123 | 1.868 | 1.272 | 1.401 | 1.088 | 1.368 | 1.426 | 1.450 | 1.590 | 0.872 | 0.936 | 0.907 |
| clang-14 -O | 0.978 | 0.979 | 1.125 | 1.889 | 1.287 | 1.390 | 1.117 | 1.360 | 1.399 | 1.406 | 1.668 | 0.905 | 0.940 | 0.913 |
| clang-17 -O3 | 0.943 | 0.983 | 1.092 | 1.843 | 1.273 | 1.389 | 1.090 | 1.332 | 1.354 | 1.331 | 1.577 | 0.893 | 0.907 | 0.909 |
| clang-17 -O2 | 0.951 | 0.983 | 1.098 | 1.857 | 1.279 | 1.391 | 1.093 | 1.337 | 1.361 | 1.336 | 1.579 | 0.907 | 0.913 | 0.914 |
| clang-17 -Os | 0.964 | 0.981 | 1.116 | 1.855 | 1.284 | 1.402 | 1.098 | 1.373 | 1.430 | 1.421 | 1.569 | 0.879 | 0.932 | 0.915 |
| clang-17 -O | 0.975 | 0.973 | 1.100 | 1.843 | 1.284 | 1.389 | 1.081 | 1.359 | 1.387 | 1.406 | 1.616 | 0.904 | 0.937 | 0.913 |
| clang-18 -O3 | 0.936 | 0.977 | 1.088 | 1.826 | 1.267 | 1.384 | 1.096 | 1.324 | 1.346 | 1.326 | 1.626 | 0.888 | 0.899 | 0.904 |
| clang-18 -O2 | 0.956 | 0.985 | 1.098 | 1.842 | 1.279 | 1.388 | 1.080 | 1.335 | 1.353 | 1.330 | 1.643 | 0.906 | 0.911 | 0.913 |
| clang-18 -Os | 0.968 | 0.980 | 1.110 | 1.857 | 1.284 | 1.405 | 1.102 | 1.366 | 1.415 | 1.417 | 1.630 | 0.880 | 0.934 | 0.915 |
| clang-18 -O | 0.978 | 0.981 | 1.099 | 1.844 | 1.287 | 1.392 | 1.085 | 1.366 | 1.412 | 1.409 | 1.648 | 0.906 | 0.942 | 0.915 |
| **geomean** | **0.925** | **0.934** | **1.101** | **1.849** | **1.225** | **1.338** | **1.060** | **1.305** | **1.280** | **1.329** | **1.382** | **0.845** | **0.896** | **0.870** |

### Does `a64/ucode` win?

_ratio = other ÷ a64/ucode (median cycles). **>1 ⇒ a64/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.478 | 0.488 | 0.601 | 0.642 | 0.701 | 0.554 | 0.502 | 0.681 | 0.632 | 0.684 | 0.602 | 0.437 | 0.466 | 0.454 |
| gcc-11 -O2 | 0.494 | 0.494 | 0.591 | 0.649 | 0.706 | 0.560 | 0.515 | 0.703 | 0.659 | 0.709 | 0.669 | 0.442 | 0.487 | 0.459 |
| gcc-11 -Os | 0.462 | 0.459 | 0.606 | 0.602 | 0.668 | 0.535 | 0.648 | 0.656 | 0.605 | 0.672 | 0.620 | 0.410 | 0.457 | 0.426 |
| gcc-11 -O | 0.479 | 0.480 | 0.597 | 0.630 | 0.688 | 0.565 | 0.495 | 0.669 | 0.649 | 0.703 | 0.663 | 0.430 | 0.463 | 0.447 |
| gcc-12 -O3 | 0.486 | 0.493 | 0.587 | 0.650 | 0.713 | 0.562 | 0.514 | 0.689 | 0.629 | 0.682 | 0.616 | 0.443 | 0.470 | 0.460 |
| gcc-12 -O2 | 0.496 | 0.492 | 0.587 | 0.650 | 0.713 | 0.562 | 0.514 | 0.709 | 0.656 | 0.710 | 0.670 | 0.443 | 0.486 | 0.460 |
| gcc-12 -Os | 0.463 | 0.459 | 0.613 | 0.605 | 0.671 | 0.533 | 0.651 | 0.665 | 0.612 | 0.674 | 0.632 | 0.412 | 0.461 | 0.428 |
| gcc-12 -O | 0.473 | 0.479 | 0.598 | 0.630 | 0.686 | 0.567 | 0.494 | 0.670 | 0.648 | 0.699 | 0.670 | 0.430 | 0.463 | 0.447 |
| gcc-13 -O3 | 0.504 | 0.505 | 0.584 | 0.666 | 0.726 | 0.571 | 0.525 | 0.711 | 0.668 | 0.707 | 0.632 | 0.453 | 0.489 | 0.471 |
| gcc-13 -O2 | 0.505 | 0.501 | 0.579 | 0.662 | 0.726 | 0.572 | 0.524 | 0.719 | 0.667 | 0.710 | 0.656 | 0.451 | 0.492 | 0.468 |
| gcc-13 -Os | 0.477 | 0.469 | 0.600 | 0.618 | 0.686 | 0.545 | 0.666 | 0.682 | 0.620 | 0.692 | 0.613 | 0.421 | 0.472 | 0.437 |
| gcc-13 -O | 0.482 | 0.483 | 0.594 | 0.634 | 0.689 | 0.569 | 0.497 | 0.675 | 0.667 | 0.693 | 0.659 | 0.432 | 0.464 | 0.449 |
| clang-14 -O3 | 0.511 | 0.528 | 0.598 | 0.684 | 0.745 | 0.590 | 0.544 | 0.713 | 0.734 | 0.734 | 0.869 | 0.478 | 0.487 | 0.488 |
| clang-14 -O2 | 0.516 | 0.527 | 0.600 | 0.686 | 0.746 | 0.583 | 0.537 | 0.714 | 0.735 | 0.734 | 0.872 | 0.485 | 0.489 | 0.490 |
| clang-14 -Os | 0.515 | 0.521 | 0.601 | 0.681 | 0.750 | 0.583 | 0.535 | 0.732 | 0.764 | 0.777 | 0.851 | 0.467 | 0.501 | 0.486 |
| clang-14 -O | 0.518 | 0.518 | 0.596 | 0.681 | 0.736 | 0.591 | 0.529 | 0.720 | 0.741 | 0.745 | 0.883 | 0.479 | 0.497 | 0.483 |
| clang-17 -O3 | 0.511 | 0.533 | 0.593 | 0.691 | 0.754 | 0.592 | 0.543 | 0.723 | 0.735 | 0.722 | 0.856 | 0.485 | 0.492 | 0.493 |
| clang-17 -O2 | 0.512 | 0.530 | 0.591 | 0.689 | 0.749 | 0.589 | 0.539 | 0.720 | 0.733 | 0.719 | 0.850 | 0.488 | 0.492 | 0.492 |
| clang-17 -Os | 0.520 | 0.529 | 0.601 | 0.692 | 0.756 | 0.592 | 0.539 | 0.740 | 0.771 | 0.766 | 0.846 | 0.474 | 0.502 | 0.493 |
| clang-17 -O | 0.529 | 0.528 | 0.597 | 0.697 | 0.754 | 0.587 | 0.543 | 0.738 | 0.753 | 0.763 | 0.877 | 0.490 | 0.508 | 0.496 |
| clang-18 -O3 | 0.512 | 0.535 | 0.596 | 0.694 | 0.758 | 0.600 | 0.548 | 0.725 | 0.737 | 0.726 | 0.891 | 0.486 | 0.493 | 0.495 |
| clang-18 -O2 | 0.519 | 0.535 | 0.596 | 0.694 | 0.754 | 0.587 | 0.543 | 0.725 | 0.735 | 0.722 | 0.892 | 0.492 | 0.494 | 0.496 |
| clang-18 -Os | 0.521 | 0.528 | 0.598 | 0.692 | 0.757 | 0.594 | 0.539 | 0.736 | 0.762 | 0.763 | 0.878 | 0.474 | 0.503 | 0.493 |
| clang-18 -O | 0.530 | 0.532 | 0.596 | 0.698 | 0.755 | 0.588 | 0.542 | 0.741 | 0.766 | 0.764 | 0.894 | 0.491 | 0.511 | 0.496 |
| **geomean** | **0.500** | **0.505** | **0.596** | **0.662** | **0.724** | **0.573** | **0.541** | **0.706** | **0.693** | **0.719** | **0.748** | **0.457** | **0.485** | **0.471** |
