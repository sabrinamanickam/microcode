# X25519 Microcode Benchmark Results

**Generated:** Mon 07 Sep 2026 23:03:30 ACST
**Host:** redunlock-GB-BPCE-3350C
**CPU:** Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
**Pinned freq:** 1094400 kHz   (governor: `userspace`, no_turbo: `1`)
**Delivered core freq:** 1100 MHz · **TSC (RDTSC) rate:** 1094 MHz · **correction f_core/f_TSC:** 1.00548 (aperf/mperf under load, verified before the sweep; comparative **ratios are invariant** to this factor, multiply **absolute** cycle counts by it for true core cycles)
**Post-sweep frequency check:** stable (+0.000% over the sweep) · delivered 1100 MHz / TSC 1094 MHz after the sweep (the pre-sweep guard proves the machine was pinned when the sweep started; this proves it stayed pinned throughout)
**Core isolation:** core 0; 28 IRQs steered to core 1 (2 per-CPU/unmovable); SCHED_FIFO 99 · nohz_full/isolcpus: off — periodic timer tick still hits core 0
**Runs per config:** 3 (recorded median is the median of those runs; worst run-to-run spread 2.719% at amd64-51/ucode @ clang-14 -Os)
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
| **microcode 5×51 (this work)** | **307,358** | — | — | clang-14 -O2 |
| fiat-crypto (verified C) | 355,283 | 1.156 | 1.195 | gcc-12 -O3 |
| CryptOpt (superoptimized asm) | 381,025 | 1.240 | 1.217 | clang-14 -O2 |
| hand-written C (`__uint128_t`) | 381,115 | 1.240 | 1.241 | clang-17 -O3 |
| amd64-51 asm (Bernstein–Schwabe) | 383,608 | 1.248 | 1.220 | clang-14 -O3 |

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
| `a51/asm` | qhasm, monolithic | qhasm asm | 356,704 | — | baseline |
| `a51/asmCld` | C, per-op calls | qhasm asm | 387,458 | 0.921 | ladder: qhasm → C (fusion lost) |
| `a51/ucCld` | C, per-op calls | **microcode** | 329,131 | **1.177** | **field ops: asm → microcode** |
| `a51/ucode` | inline-asm, chained | microcode | 314,851 | 1.045 | ladder: C → register-chained asm |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** 1.177× faster (geomean 1.166×), with the ladder **and** the framework held constant.

**Consistency check.** The steps are multiplicative, so they must compose to the
measured end-to-end ratio:

```
  0.921 (ladder) x 1.177 (field ops) x 1.045 (chaining)  =  1.13293
  measured  356704 / 314851                              =  1.13293
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
| `a64/asm` | qhasm, monolithic | qhasm asm | 271,756 | — | baseline |
| `a64/asmCld` | C, per-op calls | qhasm asm | 306,275 | 0.887 | ladder: qhasm → C (fusion lost) |
| `a64/ucode` | C, per-op calls | **microcode** | 514,891 | **0.595** | **field ops: asm → microcode** |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** microcode is 1.681× **slower** than the asm it replaces (geomean 1.680×), with the ladder held constant — against 1.895× if the ladder rewrite is wrongly charged to the field ops.

---

## Table 4 — End-to-end standing (orientation, not the claim)

**Held constant:** nothing — these are whole implementations differing in
representation, ladder, inversion and field ops at once. Useful for placing the
work against shipped code; useless for attributing the difference to microcode.
For that, see Table 1.

| implementation | cyc/X25519 | best config |
|---|---:|---|
| amd64-64 asm (Bernstein–Schwabe, 4×64) | 271,756 | gcc-13 -O3 |
| **microcode 5×51 + inline-asm ladder (this work)** | **300,205** | clang-14 -O3 |
| amd64-51 framework + microcode | 314,851 | clang-18 -O |
| donna c64 (portable C) | 337,876 | gcc-11 -O3 |
| fiat-crypto (verified C) | 355,283 | gcc-12 -O3 |
| amd64-51 asm (Bernstein–Schwabe, 5×51) | 356,704 | clang-17 -O3 |
| CryptOpt (superoptimized asm) | 381,025 | clang-14 -O2 |
| hand-written C (`__uint128_t`) | 381,115 | clang-17 -O3 |


### Dispersion at each contender's best config

_Median is the headline; min and the p10–p90 range show run-to-run spread at that config. A tight p90−p10 relative to the inter-contender gaps means the ranking is not noise._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| ucode | 300205 | 300011 | 300075 | 311128 | 11053 | clang-14 -O3 |
| a64/asm | 271756 | 271603 | 271738 | 278989 | 7251 | gcc-13 -O3 |
| a64/asmCld | 306275 | 306134 | — | — | — | clang-17 -O3 |
| a64/ucode | 514891 | 514732 | — | — | — | clang-18 -O3 |
| a51/asm | 356704 | 356580 | 356631 | 366940 | 10309 | clang-17 -O3 |
| a51/asmCld | 387458 | 387336 | 387364 | 399485 | 12121 | clang-18 -O3 |
| a51/ucCld | 329131 | 329000 | 329069 | 337173 | 8104 | clang-18 -O |
| a51/ucode | 314851 | 314697 | 314811 | 324600 | 9789 | clang-18 -O |
| cryptopt | 381025 | 380764 | 380966 | 391191 | 10225 | clang-14 -O2 |
| fiat | 355283 | 355159 | 355195 | 365584 | 10389 | gcc-12 -O3 |
| hand-C | 381115 | 380200 | 380996 | 389884 | 8888 | clang-17 -O3 |
| donna | 337876 | 337232 | 337819 | 345582 | 7763 | gcc-11 -O3 |

---

# Appendix A — full per-config sweep

The raw 24-config matrices behind the best-per-contender numbers above.
Present so the selection rule can be audited and so per-compiler behaviour is
visible; not intended to be read row by row.

### A.1 — X25519 end-to-end, every contender

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 305338 | 272423 | 336414 | 559702 | 358729 | 392105 | 335239 | 319181 | 387036 | 358269 | 389520 | **337876** |
| gcc-11 -O2 | 310731 | 272990 | 326993 | 553065 | 357999 | 390414 | 334912 | 323398 | 399330 | 376132 | 404234 | 371241 |
| gcc-11 -Os | 312952 | 273785 | 362266 | 596780 | 358938 | 398929 | 342137 | 424933 | 403749 | 371555 | 410003 | 374415 |
| gcc-11 -O | 306774 | 272703 | 335571 | 568862 | 358300 | 391179 | 346552 | 323180 | 392170 | 380461 | 411957 | 385702 |
| gcc-12 -O3 | 306007 | 273568 | 324053 | 552220 | 358741 | 393523 | 334787 | 323383 | 385984 | **355283** | 386413 | 341412 |
| gcc-12 -O2 | 310741 | 271854 | 324250 | 552639 | 358872 | 391631 | 334810 | 324933 | 400987 | 372061 | 401874 | 371032 |
| gcc-12 -Os | 313699 | 272340 | 364017 | 595068 | 358705 | 398597 | 344561 | 424234 | 404415 | 372177 | 411162 | 370967 |
| gcc-12 -O | 307545 | 272647 | 340317 | 568788 | 358436 | 389935 | 345269 | 321875 | 392091 | 380283 | 408558 | 377705 |
| gcc-13 -O3 | 308234 | **271756** | 314669 | 540153 | 358762 | 391785 | 334708 | 321142 | 389418 | 366539 | 388834 | 341362 |
| gcc-13 -O2 | 311078 | 271898 | 314013 | 542328 | 359031 | 391615 | 334732 | 322948 | 399303 | 371789 | 395140 | 356691 |
| gcc-13 -Os | 314695 | 272399 | 348591 | 581015 | 358686 | 398469 | 344612 | 424065 | 408912 | 372817 | 414769 | 357107 |
| gcc-13 -O | 307906 | 273511 | 336347 | 566586 | 359462 | 389830 | 344329 | 321588 | 391829 | 389098 | 404272 | 372939 |
| clang-14 -O3 | **300205** | 275931 | 312716 | 521735 | 356918 | 388540 | 332245 | 316171 | 381562 | 390157 | 388143 | 446907 |
| clang-14 -O2 | 300262 | 272838 | 312836 | 521008 | 356719 | 389156 | 332866 | 316122 | **381025** | 390082 | 388145 | 443116 |
| clang-14 -Os | 304441 | 273022 | 314907 | 522831 | 356966 | 390415 | 330008 | 317539 | 389921 | 409191 | 413348 | 446699 |
| clang-14 -O | 306263 | 273048 | 314418 | 528008 | 358348 | 387981 | 335822 | 316918 | 387040 | 398573 | 402774 | 456272 |
| clang-17 -O3 | 300974 | 275448 | **306275** | 518169 | **356704** | 388982 | 331431 | 316273 | 381841 | 387791 | **381115** | 434287 |
| clang-17 -O2 | 301084 | 272820 | 306330 | 516803 | 356704 | 388050 | 329826 | 316537 | 381182 | 386776 | 381336 | 429503 |
| clang-17 -Os | 305579 | 272494 | 310228 | 518083 | 356916 | 391957 | 331306 | 316391 | 388957 | 405838 | 402382 | 438910 |
| clang-17 -O | 305365 | 271823 | 307751 | 515575 | 358340 | 388486 | 329226 | 316145 | 387370 | 395852 | 402074 | 443039 |
| clang-18 -O3 | 301033 | 275536 | 306800 | **514891** | 356944 | **387458** | 330712 | 316324 | 381792 | 388092 | 382553 | 450365 |
| clang-18 -O2 | 301113 | 273354 | 306839 | 516929 | 356784 | 387478 | 329791 | 314935 | 381464 | 387094 | 382527 | 448230 |
| clang-18 -Os | 305418 | 272556 | 308451 | 515999 | 356732 | 390144 | 331854 | 318475 | 389768 | 398890 | 402332 | 455054 |
| clang-18 -O | 305373 | 272986 | 306878 | 515241 | 358638 | 388674 | **329131** | **314851** | 388980 | 399839 | 402481 | 449850 |

### A.2 — Same C ladder, only the field op differs

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | uc/Clad | a51op/Clad | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 312934 | 388381 | 387036 | 358269 | 389520 |
| gcc-11 -O2 | 337947 | 395690 | 399330 | 376132 | 404234 |
| gcc-11 -Os | 341487 | 399983 | 403749 | 371555 | 410003 |
| gcc-11 -O | 334794 | 393416 | 392170 | 380461 | 411957 |
| gcc-12 -O3 | 316664 | 385180 | 385984 | **355283** | 386413 |
| gcc-12 -O2 | 333683 | 398086 | 400987 | 372061 | 401874 |
| gcc-12 -Os | 341863 | 403501 | 404415 | 372177 | 411162 |
| gcc-12 -O | 331762 | 394819 | 392091 | 380283 | 408558 |
| gcc-13 -O3 | 315402 | 393542 | 389418 | 366539 | 388834 |
| gcc-13 -O2 | 332577 | 395802 | 399303 | 371789 | 395140 |
| gcc-13 -Os | 345283 | 405272 | 408912 | 372817 | 414769 |
| gcc-13 -O | 332799 | 393471 | 391829 | 389098 | 404272 |
| clang-14 -O3 | 307470 | **383608** | 381562 | 390157 | 388143 |
| clang-14 -O2 | **307358** | 383899 | **381025** | 390082 | 388145 |
| clang-14 -Os | 314968 | 390615 | 389921 | 409191 | 413348 |
| clang-14 -O | 310634 | 389341 | 387040 | 398573 | 402774 |
| clang-17 -O3 | 310159 | 385845 | 381841 | 387791 | **381115** |
| clang-17 -O2 | 309938 | 385566 | 381182 | 386776 | 381336 |
| clang-17 -Os | 314153 | 390328 | 388957 | 405838 | 402382 |
| clang-17 -O | 311949 | 388667 | 387370 | 395852 | 402074 |
| clang-18 -O3 | 308354 | 385796 | 381792 | 388092 | 382553 |
| clang-18 -O2 | 308267 | 385764 | 381464 | 387094 | 382527 |
| clang-18 -Os | 313922 | 390431 | 389768 | 398890 | 402332 |
| clang-18 -O | 311332 | 389945 | 388980 | 399839 | 402481 |

### A.3 — Per-config ratios


### Does `uc/Clad` win?

_ratio = other ÷ uc/Clad (median cycles). **>1 ⇒ uc/Clad is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a51op/Clad | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|
| gcc-11 -O3 | 1.241 | 1.237 | 1.145 | 1.245 |
| gcc-11 -O2 | 1.171 | 1.182 | 1.113 | 1.196 |
| gcc-11 -Os | 1.171 | 1.182 | 1.088 | 1.201 |
| gcc-11 -O | 1.175 | 1.171 | 1.136 | 1.230 |
| gcc-12 -O3 | 1.216 | 1.219 | 1.122 | 1.220 |
| gcc-12 -O2 | 1.193 | 1.202 | 1.115 | 1.204 |
| gcc-12 -Os | 1.180 | 1.183 | 1.089 | 1.203 |
| gcc-12 -O | 1.190 | 1.182 | 1.146 | 1.231 |
| gcc-13 -O3 | 1.248 | 1.235 | 1.162 | 1.233 |
| gcc-13 -O2 | 1.190 | 1.201 | 1.118 | 1.188 |
| gcc-13 -Os | 1.174 | 1.184 | 1.080 | 1.201 |
| gcc-13 -O | 1.182 | 1.177 | 1.169 | 1.215 |
| clang-14 -O3 | 1.248 | 1.241 | 1.269 | 1.262 |
| clang-14 -O2 | 1.249 | 1.240 | 1.269 | 1.263 |
| clang-14 -Os | 1.240 | 1.238 | 1.299 | 1.312 |
| clang-14 -O | 1.253 | 1.246 | 1.283 | 1.297 |
| clang-17 -O3 | 1.244 | 1.231 | 1.250 | 1.229 |
| clang-17 -O2 | 1.244 | 1.230 | 1.248 | 1.230 |
| clang-17 -Os | 1.242 | 1.238 | 1.292 | 1.281 |
| clang-17 -O | 1.246 | 1.242 | 1.269 | 1.289 |
| clang-18 -O3 | 1.251 | 1.238 | 1.259 | 1.241 |
| clang-18 -O2 | 1.251 | 1.237 | 1.256 | 1.241 |
| clang-18 -Os | 1.244 | 1.242 | 1.271 | 1.282 |
| clang-18 -O | 1.253 | 1.249 | 1.284 | 1.293 |
| **geomean** | **1.220** | **1.217** | **1.195** | **1.241** |

### Does `ucode` win?

_ratio = other ÷ ucode (median cycles). **>1 ⇒ ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a64/asm | a51/asm | a51/ucode | donna | fiat | cryptopt | hand-C |
|---|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.892 | 1.175 | 1.045 | 1.107 | 1.173 | 1.268 | 1.276 |
| gcc-11 -O2 | 0.879 | 1.152 | 1.041 | 1.195 | 1.210 | 1.285 | 1.301 |
| gcc-11 -Os | 0.875 | 1.147 | 1.358 | 1.196 | 1.187 | 1.290 | 1.310 |
| gcc-11 -O | 0.889 | 1.168 | 1.053 | 1.257 | 1.240 | 1.278 | 1.343 |
| gcc-12 -O3 | 0.894 | 1.172 | 1.057 | 1.116 | 1.161 | 1.261 | 1.263 |
| gcc-12 -O2 | 0.875 | 1.155 | 1.046 | 1.194 | 1.197 | 1.290 | 1.293 |
| gcc-12 -Os | 0.868 | 1.143 | 1.352 | 1.183 | 1.186 | 1.289 | 1.311 |
| gcc-12 -O | 0.887 | 1.165 | 1.047 | 1.228 | 1.237 | 1.275 | 1.328 |
| gcc-13 -O3 | 0.882 | 1.164 | 1.042 | 1.107 | 1.189 | 1.263 | 1.261 |
| gcc-13 -O2 | 0.874 | 1.154 | 1.038 | 1.147 | 1.195 | 1.284 | 1.270 |
| gcc-13 -Os | 0.866 | 1.140 | 1.348 | 1.135 | 1.185 | 1.299 | 1.318 |
| gcc-13 -O | 0.888 | 1.167 | 1.044 | 1.211 | 1.264 | 1.273 | 1.313 |
| clang-14 -O3 | 0.919 | 1.189 | 1.053 | 1.489 | 1.300 | 1.271 | 1.293 |
| clang-14 -O2 | 0.909 | 1.188 | 1.053 | 1.476 | 1.299 | 1.269 | 1.293 |
| clang-14 -Os | 0.897 | 1.173 | 1.043 | 1.467 | 1.344 | 1.281 | 1.358 |
| clang-14 -O | 0.892 | 1.170 | 1.035 | 1.490 | 1.301 | 1.264 | 1.315 |
| clang-17 -O3 | 0.915 | 1.185 | 1.051 | 1.443 | 1.288 | 1.269 | 1.266 |
| clang-17 -O2 | 0.906 | 1.185 | 1.051 | 1.427 | 1.285 | 1.266 | 1.267 |
| clang-17 -Os | 0.892 | 1.168 | 1.035 | 1.436 | 1.328 | 1.273 | 1.317 |
| clang-17 -O | 0.890 | 1.173 | 1.035 | 1.451 | 1.296 | 1.269 | 1.317 |
| clang-18 -O3 | 0.915 | 1.186 | 1.051 | 1.496 | 1.289 | 1.268 | 1.271 |
| clang-18 -O2 | 0.908 | 1.185 | 1.046 | 1.489 | 1.286 | 1.267 | 1.270 |
| clang-18 -Os | 0.892 | 1.168 | 1.043 | 1.490 | 1.306 | 1.276 | 1.317 |
| clang-18 -O | 0.894 | 1.174 | 1.031 | 1.473 | 1.309 | 1.274 | 1.318 |
| **geomean** | **0.891** | **1.169** | **1.079** | **1.312** | **1.251** | **1.275** | **1.299** |

### A.4 — Uncontrolled ratios (superseded)

These compare a microcode hybrid against its asm baseline **without** holding the
ladder constant, so they attribute the ladder rewrite to the field ops. Retained
for auditability only — Tables 2 and 3 are the correct form of these comparisons.

### Does `a51/ucode` win?

_ratio = other ÷ a51/ucode (median cycles). **>1 ⇒ a51/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | cryptopt | fiat | hand-C | donna |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.957 | 0.854 | 1.054 | 1.754 | 1.124 | 1.228 | 1.050 | 1.213 | 1.122 | 1.220 | 1.059 |
| gcc-11 -O2 | 0.961 | 0.844 | 1.011 | 1.710 | 1.107 | 1.207 | 1.036 | 1.235 | 1.163 | 1.250 | 1.148 |
| gcc-11 -Os | 0.736 | 0.644 | 0.853 | 1.404 | 0.845 | 0.939 | 0.805 | 0.950 | 0.874 | 0.965 | 0.881 |
| gcc-11 -O | 0.949 | 0.844 | 1.038 | 1.760 | 1.109 | 1.210 | 1.072 | 1.213 | 1.177 | 1.275 | 1.193 |
| gcc-12 -O3 | 0.946 | 0.846 | 1.002 | 1.708 | 1.109 | 1.217 | 1.035 | 1.194 | 1.099 | 1.195 | 1.056 |
| gcc-12 -O2 | 0.956 | 0.837 | 0.998 | 1.701 | 1.104 | 1.205 | 1.030 | 1.234 | 1.145 | 1.237 | 1.142 |
| gcc-12 -Os | 0.739 | 0.642 | 0.858 | 1.403 | 0.846 | 0.940 | 0.812 | 0.953 | 0.877 | 0.969 | 0.874 |
| gcc-12 -O | 0.955 | 0.847 | 1.057 | 1.767 | 1.114 | 1.211 | 1.073 | 1.218 | 1.181 | 1.269 | 1.173 |
| gcc-13 -O3 | 0.960 | 0.846 | 0.980 | 1.682 | 1.117 | 1.220 | 1.042 | 1.213 | 1.141 | 1.211 | 1.063 |
| gcc-13 -O2 | 0.963 | 0.842 | 0.972 | 1.679 | 1.112 | 1.213 | 1.036 | 1.236 | 1.151 | 1.224 | 1.104 |
| gcc-13 -Os | 0.742 | 0.642 | 0.822 | 1.370 | 0.846 | 0.940 | 0.813 | 0.964 | 0.879 | 0.978 | 0.842 |
| gcc-13 -O | 0.957 | 0.851 | 1.046 | 1.762 | 1.118 | 1.212 | 1.071 | 1.218 | 1.210 | 1.257 | 1.160 |
| clang-14 -O3 | 0.950 | 0.873 | 0.989 | 1.650 | 1.129 | 1.229 | 1.051 | 1.207 | 1.234 | 1.228 | 1.413 |
| clang-14 -O2 | 0.950 | 0.863 | 0.990 | 1.648 | 1.128 | 1.231 | 1.053 | 1.205 | 1.234 | 1.228 | 1.402 |
| clang-14 -Os | 0.959 | 0.860 | 0.992 | 1.647 | 1.124 | 1.230 | 1.039 | 1.228 | 1.289 | 1.302 | 1.407 |
| clang-14 -O | 0.966 | 0.862 | 0.992 | 1.666 | 1.131 | 1.224 | 1.060 | 1.221 | 1.258 | 1.271 | 1.440 |
| clang-17 -O3 | 0.952 | 0.871 | 0.968 | 1.638 | 1.128 | 1.230 | 1.048 | 1.207 | 1.226 | 1.205 | 1.373 |
| clang-17 -O2 | 0.951 | 0.862 | 0.968 | 1.633 | 1.127 | 1.226 | 1.042 | 1.204 | 1.222 | 1.205 | 1.357 |
| clang-17 -Os | 0.966 | 0.861 | 0.981 | 1.637 | 1.128 | 1.239 | 1.047 | 1.229 | 1.283 | 1.272 | 1.387 |
| clang-17 -O | 0.966 | 0.860 | 0.973 | 1.631 | 1.133 | 1.229 | 1.041 | 1.225 | 1.252 | 1.272 | 1.401 |
| clang-18 -O3 | 0.952 | 0.871 | 0.970 | 1.628 | 1.128 | 1.225 | 1.045 | 1.207 | 1.227 | 1.209 | 1.424 |
| clang-18 -O2 | 0.956 | 0.868 | 0.974 | 1.641 | 1.133 | 1.230 | 1.047 | 1.211 | 1.229 | 1.215 | 1.423 |
| clang-18 -Os | 0.959 | 0.856 | 0.969 | 1.620 | 1.120 | 1.225 | 1.042 | 1.224 | 1.253 | 1.263 | 1.429 |
| clang-18 -O | 0.970 | 0.867 | 0.975 | 1.636 | 1.139 | 1.234 | 1.045 | 1.235 | 1.270 | 1.278 | 1.429 |
| **geomean** | **0.927** | **0.826** | **0.975** | **1.637** | **1.083** | **1.183** | **1.015** | **1.182** | **1.160** | **1.204** | **1.216** |

### Does `a64/ucode` win?

_ratio = other ÷ a64/ucode (median cycles). **>1 ⇒ a64/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.546 | 0.487 | 0.601 | 0.641 | 0.701 | 0.599 | 0.570 | 0.692 | 0.640 | 0.696 | 0.604 |
| gcc-11 -O2 | 0.562 | 0.494 | 0.591 | 0.647 | 0.706 | 0.606 | 0.585 | 0.722 | 0.680 | 0.731 | 0.671 |
| gcc-11 -Os | 0.524 | 0.459 | 0.607 | 0.601 | 0.668 | 0.573 | 0.712 | 0.677 | 0.623 | 0.687 | 0.627 |
| gcc-11 -O | 0.539 | 0.479 | 0.590 | 0.630 | 0.688 | 0.609 | 0.568 | 0.689 | 0.669 | 0.724 | 0.678 |
| gcc-12 -O3 | 0.554 | 0.495 | 0.587 | 0.650 | 0.713 | 0.606 | 0.586 | 0.699 | 0.643 | 0.700 | 0.618 |
| gcc-12 -O2 | 0.562 | 0.492 | 0.587 | 0.649 | 0.709 | 0.606 | 0.588 | 0.726 | 0.673 | 0.727 | 0.671 |
| gcc-12 -Os | 0.527 | 0.458 | 0.612 | 0.603 | 0.670 | 0.579 | 0.713 | 0.680 | 0.625 | 0.691 | 0.623 |
| gcc-12 -O | 0.541 | 0.479 | 0.598 | 0.630 | 0.686 | 0.607 | 0.566 | 0.689 | 0.669 | 0.718 | 0.664 |
| gcc-13 -O3 | 0.571 | 0.503 | 0.583 | 0.664 | 0.725 | 0.620 | 0.595 | 0.721 | 0.679 | 0.720 | 0.632 |
| gcc-13 -O2 | 0.574 | 0.501 | 0.579 | 0.662 | 0.722 | 0.617 | 0.595 | 0.736 | 0.686 | 0.729 | 0.658 |
| gcc-13 -Os | 0.542 | 0.469 | 0.600 | 0.617 | 0.686 | 0.593 | 0.730 | 0.704 | 0.642 | 0.714 | 0.615 |
| gcc-13 -O | 0.543 | 0.483 | 0.594 | 0.634 | 0.688 | 0.608 | 0.568 | 0.692 | 0.687 | 0.714 | 0.658 |
| clang-14 -O3 | 0.575 | 0.529 | 0.599 | 0.684 | 0.745 | 0.637 | 0.606 | 0.731 | 0.748 | 0.744 | 0.857 |
| clang-14 -O2 | 0.576 | 0.524 | 0.600 | 0.685 | 0.747 | 0.639 | 0.607 | 0.731 | 0.749 | 0.745 | 0.850 |
| clang-14 -Os | 0.582 | 0.522 | 0.602 | 0.683 | 0.747 | 0.631 | 0.607 | 0.746 | 0.783 | 0.791 | 0.854 |
| clang-14 -O | 0.580 | 0.517 | 0.595 | 0.679 | 0.735 | 0.636 | 0.600 | 0.733 | 0.755 | 0.763 | 0.864 |
| clang-17 -O3 | 0.581 | 0.532 | 0.591 | 0.688 | 0.751 | 0.640 | 0.610 | 0.737 | 0.748 | 0.736 | 0.838 |
| clang-17 -O2 | 0.583 | 0.528 | 0.593 | 0.690 | 0.751 | 0.638 | 0.612 | 0.738 | 0.748 | 0.738 | 0.831 |
| clang-17 -Os | 0.590 | 0.526 | 0.599 | 0.689 | 0.757 | 0.639 | 0.611 | 0.751 | 0.783 | 0.777 | 0.847 |
| clang-17 -O | 0.592 | 0.527 | 0.597 | 0.695 | 0.754 | 0.639 | 0.613 | 0.751 | 0.768 | 0.780 | 0.859 |
| clang-18 -O3 | 0.585 | 0.535 | 0.596 | 0.693 | 0.753 | 0.642 | 0.614 | 0.742 | 0.754 | 0.743 | 0.875 |
| clang-18 -O2 | 0.583 | 0.529 | 0.594 | 0.690 | 0.750 | 0.638 | 0.609 | 0.738 | 0.749 | 0.740 | 0.867 |
| clang-18 -Os | 0.592 | 0.528 | 0.598 | 0.691 | 0.756 | 0.643 | 0.617 | 0.755 | 0.773 | 0.780 | 0.882 |
| clang-18 -O | 0.593 | 0.530 | 0.596 | 0.696 | 0.754 | 0.639 | 0.611 | 0.755 | 0.776 | 0.781 | 0.873 |
| **geomean** | **0.566** | **0.505** | **0.595** | **0.662** | **0.723** | **0.620** | **0.611** | **0.722** | **0.708** | **0.736** | **0.743** |
