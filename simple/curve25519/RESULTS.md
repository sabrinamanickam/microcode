# X25519 Microcode Benchmark Results

**Generated:** Fri 04 Sep 2026 21:27:24 ACST
**Host:** redunlock-GB-BPCE-3350C
**CPU:** Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
**Pinned freq:** 1094431 kHz   (governor: `userspace`, no_turbo: `1`)
**Delivered core freq:** 1100 MHz · **TSC (RDTSC) rate:** 1094 MHz · **correction f_core/f_TSC:** 1.00548 (aperf/mperf under load, verified before the sweep; comparative **ratios are invariant** to this factor, multiply **absolute** cycle counts by it for true core cycles)
**Post-sweep frequency check:** stable (+0.000% over the sweep) · delivered 1100 MHz / TSC 1094 MHz after the sweep (the pre-sweep guard proves the machine was pinned when the sweep started; this proves it stayed pinned throughout)
**Core isolation:** core 0; 28 IRQs steered to core 1 (2 per-CPU/unmovable); SCHED_FIFO 99 · nohz_full/isolcpus: off — periodic timer tick still hits core 0
**Runs per config:** 3 (recorded median is the median of those runs; worst run-to-run spread 2.206% at amd64-51/asm @ clang-14 -Os)
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
| **microcode 5×51 (this work)** | **307,389** | — | — | clang-14 -O3 |
| fiat-crypto (verified C) | 355,584 | 1.157 | 1.194 | gcc-12 -O3 |
| hand-written C (`__uint128_t`) | 380,915 | 1.239 | 1.240 | clang-17 -O2 |
| CryptOpt (superoptimized asm) | 381,112 | 1.240 | 1.217 | clang-14 -O2 |
| amd64-51 asm (Bernstein–Schwabe) | 383,801 | 1.249 | 1.220 | clang-14 -O3 |

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
| `a51/asm` | qhasm, monolithic | qhasm asm | 356,649 | — | baseline |
| `a51/asmCld` | C, per-op calls | qhasm asm | 387,938 | 0.919 | ladder: qhasm → C (fusion lost) |
| `a51/ucCld` | C, per-op calls | **microcode** | 329,110 | **1.179** | **field ops: asm → microcode** |
| `a51/ucode` | inline-asm, chained | microcode | 314,787 | 1.046 | ladder: C → register-chained asm |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** 1.179× faster (geomean 1.167×), with the ladder **and** the framework held constant.

**Consistency check.** The steps are multiplicative, so they must compose to the
measured end-to-end ratio:

```
  0.919 (ladder) x 1.179 (field ops) x 1.046 (chaining)  =  1.13299
  measured  356649 / 314787                              =  1.13299
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
| `a64/asm` | qhasm, monolithic | qhasm asm | 271,750 | — | baseline |
| `a64/asmCld` | C, per-op calls | qhasm asm | 306,243 | 0.887 | ladder: qhasm → C (fusion lost) |
| `a64/ucode` | C, per-op calls | **microcode** | 514,355 | **0.595** | **field ops: asm → microcode** |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** microcode is 1.680× **slower** than the asm it replaces (geomean 1.678×), with the ladder held constant — against 1.893× if the ladder rewrite is wrongly charged to the field ops.

---

## Table 4 — End-to-end standing (orientation, not the claim)

**Held constant:** nothing — these are whole implementations differing in
representation, ladder, inversion and field ops at once. Useful for placing the
work against shipped code; useless for attributing the difference to microcode.
For that, see Table 1.

| implementation | cyc/X25519 | best config |
|---|---:|---|
| amd64-64 asm (Bernstein–Schwabe, 4×64) | 271,750 | gcc-13 -O2 |
| **microcode 5×51 + inline-asm ladder (this work)** | **300,102** | clang-14 -O2 |
| amd64-51 framework + microcode | 314,787 | clang-18 -O |
| donna c64 (portable C) | 338,430 | gcc-11 -O3 |
| fiat-crypto (verified C) | 355,584 | gcc-12 -O3 |
| amd64-51 asm (Bernstein–Schwabe, 5×51) | 356,649 | clang-18 -O3 |
| hand-written C (`__uint128_t`) | 380,915 | clang-17 -O2 |
| CryptOpt (superoptimized asm) | 381,112 | clang-14 -O2 |


### Dispersion at each contender's best config

_Median is the headline; min and the p10–p90 range show run-to-run spread at that config. A tight p90−p10 relative to the inter-contender gaps means the ranking is not noise._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| ucode | 300102 | 299734 | 300007 | 308164 | 8157 | clang-14 -O2 |
| a64/asm | 271750 | 271594 | 271707 | 278519 | 6812 | gcc-13 -O2 |
| a64/asmCld | 306243 | 306131 | — | — | — | clang-17 -O3 |
| a64/ucode | 514355 | 514270 | — | — | — | clang-18 -O2 |
| a51/asm | 356649 | 356523 | 356568 | 365565 | 8997 | clang-18 -O3 |
| a51/asmCld | 387938 | 387822 | 387879 | 397206 | 9327 | clang-14 -O |
| a51/ucCld | 329110 | 329000 | 329045 | 336428 | 7383 | clang-17 -O |
| a51/ucode | 314787 | 313384 | 314636 | 325653 | 11017 | clang-18 -O |
| cryptopt | 381112 | 380795 | 381055 | 389500 | 8445 | clang-14 -O2 |
| fiat | 355584 | 355487 | 355522 | 363880 | 8358 | gcc-12 -O3 |
| hand-C | 380915 | 380293 | 380837 | 389853 | 9016 | clang-17 -O2 |
| donna | 338430 | 337821 | 338339 | 346327 | 7988 | gcc-11 -O3 |

---

# Appendix A — full per-config sweep

The raw 24-config matrices behind the best-per-contender numbers above.
Present so the selection rule can be audited and so per-compiler behaviour is
visible; not intended to be read row by row.

### A.1 — X25519 end-to-end, every contender

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 305127 | 272501 | 336510 | 559504 | 359187 | 393492 | 334729 | 321321 | 387065 | 358646 | 389900 | **338430** |
| gcc-11 -O2 | 310568 | 273073 | 326923 | 553272 | 358614 | 390676 | 334905 | 324541 | 399006 | 375793 | 403737 | 371813 |
| gcc-11 -Os | 312617 | 273972 | 362011 | 595915 | 359491 | 398570 | 342411 | 424436 | 403700 | 371280 | 410048 | 374341 |
| gcc-11 -O | 306650 | 272603 | 339453 | 568892 | 357966 | 391214 | 346450 | 321920 | 392221 | 380365 | 411844 | 386020 |
| gcc-12 -O3 | 305669 | 273527 | 323809 | 552383 | 359041 | 393500 | 334632 | 321645 | 385951 | **355584** | 386501 | 341756 |
| gcc-12 -O2 | 311344 | 271768 | 324309 | 552291 | 358508 | 393410 | 334863 | 323971 | 400895 | 372214 | 401553 | 371286 |
| gcc-12 -Os | 312585 | 272596 | 363869 | 593723 | 359223 | 398209 | 344799 | 423833 | 404273 | 371841 | 411029 | 370756 |
| gcc-12 -O | 307337 | 272478 | 340290 | 568693 | 358074 | 390063 | 345129 | 322516 | 392070 | 380113 | 408385 | 377922 |
| gcc-13 -O3 | 307941 | 271839 | 314951 | 539340 | 358966 | 393422 | 334726 | 322667 | 389787 | 366589 | 388955 | 342094 |
| gcc-13 -O2 | 310935 | **271750** | 314082 | 542346 | 358950 | 393309 | 334798 | 322968 | 399226 | 371960 | 394831 | 357014 |
| gcc-13 -Os | 314373 | 272582 | 348648 | 582261 | 359197 | 398162 | 344661 | 423854 | 408896 | 372361 | 414844 | 356703 |
| gcc-13 -O | 307710 | 273383 | 340148 | 566570 | 359151 | 389978 | 344191 | 321744 | 391871 | 389043 | 404375 | 373077 |
| clang-14 -O3 | 300663 | 275869 | 312679 | 521723 | 356706 | 388966 | 333650 | 315973 | 381391 | 388679 | 388286 | 446776 |
| clang-14 -O2 | **300102** | 273416 | 312780 | 521263 | 356740 | 389137 | 331809 | 315969 | **381112** | 388416 | 388186 | 442291 |
| clang-14 -Os | 304563 | 272895 | 314902 | 523858 | 356727 | 390790 | 329908 | 317655 | 389763 | 409242 | 413317 | 446652 |
| clang-14 -O | 306182 | 273130 | 314426 | 527866 | 358630 | **387938** | 335726 | 316712 | 387136 | 398436 | 402520 | 455717 |
| clang-17 -O3 | 300946 | 275516 | **306243** | 518160 | 356775 | 388980 | 331854 | 316057 | 381803 | 387691 | 380984 | 434139 |
| clang-17 -O2 | 300897 | 273364 | 306332 | 515673 | 356745 | 387980 | 329837 | 315849 | 381224 | 388007 | **380915** | 428898 |
| clang-17 -Os | 305393 | 272501 | 310224 | 515995 | 356773 | 392071 | 331172 | 316304 | 388948 | 405907 | 402288 | 437325 |
| clang-17 -O | 305342 | 271898 | 307735 | 515578 | 358630 | 388459 | **329110** | 315474 | 387406 | 395745 | 402198 | 442657 |
| clang-18 -O3 | 301064 | 275564 | 306752 | 514789 | **356649** | 387946 | 330820 | 316285 | 381676 | 387075 | 381340 | 450063 |
| clang-18 -O2 | 300944 | 273760 | 306840 | **514355** | 356749 | 389688 | 331569 | 316330 | 381459 | 387987 | 382751 | 447289 |
| clang-18 -Os | 305361 | 272485 | 308433 | 515855 | 356973 | 390139 | 331282 | 317039 | 389572 | 399010 | 402031 | 454343 |
| clang-18 -O | 305471 | 273209 | 306870 | 515939 | 358791 | 388683 | 329134 | **314787** | 389467 | 399420 | 402512 | 449494 |

### A.2 — Same C ladder, only the field op differs

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | uc/Clad | a51op/Clad | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 312633 | 388319 | 387065 | 358646 | 389900 |
| gcc-11 -O2 | 338022 | 395375 | 399006 | 375793 | 403737 |
| gcc-11 -Os | 341561 | 400097 | 403700 | 371280 | 410048 |
| gcc-11 -O | 334802 | 393274 | 392221 | 380365 | 411844 |
| gcc-12 -O3 | 316291 | 385157 | 385951 | **355584** | 386501 |
| gcc-12 -O2 | 333786 | 398068 | 400895 | 372214 | 401553 |
| gcc-12 -Os | 341848 | 403456 | 404273 | 371841 | 411029 |
| gcc-12 -O | 331746 | 394672 | 392070 | 380113 | 408385 |
| gcc-13 -O3 | 314880 | 393605 | 389787 | 366589 | 388955 |
| gcc-13 -O2 | 332514 | 395822 | 399226 | 371960 | 394831 |
| gcc-13 -Os | 344394 | 405007 | 408896 | 372361 | 414844 |
| gcc-13 -O | 332658 | 393807 | 391871 | 389043 | 404375 |
| clang-14 -O3 | **307389** | **383801** | 381391 | 388679 | 388286 |
| clang-14 -O2 | 307830 | 384044 | **381112** | 388416 | 388186 |
| clang-14 -Os | 315167 | 390641 | 389763 | 409242 | 413317 |
| clang-14 -O | 310617 | 389571 | 387136 | 398436 | 402520 |
| clang-17 -O3 | 310280 | 385874 | 381803 | 387691 | 380984 |
| clang-17 -O2 | 310614 | 385666 | 381224 | 388007 | **380915** |
| clang-17 -Os | 314371 | 390266 | 388948 | 405907 | 402288 |
| clang-17 -O | 312263 | 388862 | 387406 | 395745 | 402198 |
| clang-18 -O3 | 308834 | 385740 | 381676 | 387075 | 381340 |
| clang-18 -O2 | 308726 | 385630 | 381459 | 387987 | 382751 |
| clang-18 -Os | 316429 | 390363 | 389572 | 399010 | 402031 |
| clang-18 -O | 311642 | 390098 | 389467 | 399420 | 402512 |

### A.3 — Per-config ratios


### Does `uc/Clad` win?

_ratio = other ÷ uc/Clad (median cycles). **>1 ⇒ uc/Clad is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a51op/Clad | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|
| gcc-11 -O3 | 1.242 | 1.238 | 1.147 | 1.247 |
| gcc-11 -O2 | 1.170 | 1.180 | 1.112 | 1.194 |
| gcc-11 -Os | 1.171 | 1.182 | 1.087 | 1.201 |
| gcc-11 -O | 1.175 | 1.172 | 1.136 | 1.230 |
| gcc-12 -O3 | 1.218 | 1.220 | 1.124 | 1.222 |
| gcc-12 -O2 | 1.193 | 1.201 | 1.115 | 1.203 |
| gcc-12 -Os | 1.180 | 1.183 | 1.088 | 1.202 |
| gcc-12 -O | 1.190 | 1.182 | 1.146 | 1.231 |
| gcc-13 -O3 | 1.250 | 1.238 | 1.164 | 1.235 |
| gcc-13 -O2 | 1.190 | 1.201 | 1.119 | 1.187 |
| gcc-13 -Os | 1.176 | 1.187 | 1.081 | 1.205 |
| gcc-13 -O | 1.184 | 1.178 | 1.169 | 1.216 |
| clang-14 -O3 | 1.249 | 1.241 | 1.264 | 1.263 |
| clang-14 -O2 | 1.248 | 1.238 | 1.262 | 1.261 |
| clang-14 -Os | 1.239 | 1.237 | 1.298 | 1.311 |
| clang-14 -O | 1.254 | 1.246 | 1.283 | 1.296 |
| clang-17 -O3 | 1.244 | 1.231 | 1.249 | 1.228 |
| clang-17 -O2 | 1.242 | 1.227 | 1.249 | 1.226 |
| clang-17 -Os | 1.241 | 1.237 | 1.291 | 1.280 |
| clang-17 -O | 1.245 | 1.241 | 1.267 | 1.288 |
| clang-18 -O3 | 1.249 | 1.236 | 1.253 | 1.235 |
| clang-18 -O2 | 1.249 | 1.236 | 1.257 | 1.240 |
| clang-18 -Os | 1.234 | 1.231 | 1.261 | 1.271 |
| clang-18 -O | 1.252 | 1.250 | 1.282 | 1.292 |
| **geomean** | **1.220** | **1.217** | **1.194** | **1.240** |

### Does `ucode` win?

_ratio = other ÷ ucode (median cycles). **>1 ⇒ ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a64/asm | a51/asm | a51/ucode | donna | fiat | cryptopt | hand-C |
|---|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.893 | 1.177 | 1.053 | 1.109 | 1.175 | 1.269 | 1.278 |
| gcc-11 -O2 | 0.879 | 1.155 | 1.045 | 1.197 | 1.210 | 1.285 | 1.300 |
| gcc-11 -Os | 0.876 | 1.150 | 1.358 | 1.197 | 1.188 | 1.291 | 1.312 |
| gcc-11 -O | 0.889 | 1.167 | 1.050 | 1.259 | 1.240 | 1.279 | 1.343 |
| gcc-12 -O3 | 0.895 | 1.175 | 1.052 | 1.118 | 1.163 | 1.263 | 1.264 |
| gcc-12 -O2 | 0.873 | 1.151 | 1.041 | 1.193 | 1.196 | 1.288 | 1.290 |
| gcc-12 -Os | 0.872 | 1.149 | 1.356 | 1.186 | 1.190 | 1.293 | 1.315 |
| gcc-12 -O | 0.887 | 1.165 | 1.049 | 1.230 | 1.237 | 1.276 | 1.329 |
| gcc-13 -O3 | 0.883 | 1.166 | 1.048 | 1.111 | 1.190 | 1.266 | 1.263 |
| gcc-13 -O2 | 0.874 | 1.154 | 1.039 | 1.148 | 1.196 | 1.284 | 1.270 |
| gcc-13 -Os | 0.867 | 1.143 | 1.348 | 1.135 | 1.184 | 1.301 | 1.320 |
| gcc-13 -O | 0.888 | 1.167 | 1.046 | 1.212 | 1.264 | 1.274 | 1.314 |
| clang-14 -O3 | 0.918 | 1.186 | 1.051 | 1.486 | 1.293 | 1.268 | 1.291 |
| clang-14 -O2 | 0.911 | 1.189 | 1.053 | 1.474 | 1.294 | 1.270 | 1.294 |
| clang-14 -Os | 0.896 | 1.171 | 1.043 | 1.467 | 1.344 | 1.280 | 1.357 |
| clang-14 -O | 0.892 | 1.171 | 1.034 | 1.488 | 1.301 | 1.264 | 1.315 |
| clang-17 -O3 | 0.915 | 1.186 | 1.050 | 1.443 | 1.288 | 1.269 | 1.266 |
| clang-17 -O2 | 0.908 | 1.186 | 1.050 | 1.425 | 1.290 | 1.267 | 1.266 |
| clang-17 -Os | 0.892 | 1.168 | 1.036 | 1.432 | 1.329 | 1.274 | 1.317 |
| clang-17 -O | 0.890 | 1.175 | 1.033 | 1.450 | 1.296 | 1.269 | 1.317 |
| clang-18 -O3 | 0.915 | 1.185 | 1.051 | 1.495 | 1.286 | 1.268 | 1.267 |
| clang-18 -O2 | 0.910 | 1.185 | 1.051 | 1.486 | 1.289 | 1.268 | 1.272 |
| clang-18 -Os | 0.892 | 1.169 | 1.038 | 1.488 | 1.307 | 1.276 | 1.317 |
| clang-18 -O | 0.894 | 1.175 | 1.030 | 1.471 | 1.308 | 1.275 | 1.318 |
| **geomean** | **0.892** | **1.169** | **1.079** | **1.312** | **1.251** | **1.276** | **1.299** |

### A.4 — Uncontrolled ratios (superseded)

These compare a microcode hybrid against its asm baseline **without** holding the
ladder constant, so they attribute the ladder rewrite to the field ops. Retained
for auditability only — Tables 2 and 3 are the correct form of these comparisons.

### Does `a51/ucode` win?

_ratio = other ÷ a51/ucode (median cycles). **>1 ⇒ a51/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | cryptopt | fiat | hand-C | donna |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.950 | 0.848 | 1.047 | 1.741 | 1.118 | 1.225 | 1.042 | 1.205 | 1.116 | 1.213 | 1.053 |
| gcc-11 -O2 | 0.957 | 0.841 | 1.007 | 1.705 | 1.105 | 1.204 | 1.032 | 1.229 | 1.158 | 1.244 | 1.146 |
| gcc-11 -Os | 0.737 | 0.645 | 0.853 | 1.404 | 0.847 | 0.939 | 0.807 | 0.951 | 0.875 | 0.966 | 0.882 |
| gcc-11 -O | 0.953 | 0.847 | 1.054 | 1.767 | 1.112 | 1.215 | 1.076 | 1.218 | 1.182 | 1.279 | 1.199 |
| gcc-12 -O3 | 0.950 | 0.850 | 1.007 | 1.717 | 1.116 | 1.223 | 1.040 | 1.200 | 1.106 | 1.202 | 1.063 |
| gcc-12 -O2 | 0.961 | 0.839 | 1.001 | 1.705 | 1.107 | 1.214 | 1.034 | 1.237 | 1.149 | 1.239 | 1.146 |
| gcc-12 -Os | 0.738 | 0.643 | 0.859 | 1.401 | 0.848 | 0.940 | 0.814 | 0.954 | 0.877 | 0.970 | 0.875 |
| gcc-12 -O | 0.953 | 0.845 | 1.055 | 1.763 | 1.110 | 1.209 | 1.070 | 1.216 | 1.179 | 1.266 | 1.172 |
| gcc-13 -O3 | 0.954 | 0.842 | 0.976 | 1.672 | 1.112 | 1.219 | 1.037 | 1.208 | 1.136 | 1.205 | 1.060 |
| gcc-13 -O2 | 0.963 | 0.841 | 0.972 | 1.679 | 1.111 | 1.218 | 1.037 | 1.236 | 1.152 | 1.223 | 1.105 |
| gcc-13 -Os | 0.742 | 0.643 | 0.823 | 1.374 | 0.847 | 0.939 | 0.813 | 0.965 | 0.879 | 0.979 | 0.842 |
| gcc-13 -O | 0.956 | 0.850 | 1.057 | 1.761 | 1.116 | 1.212 | 1.070 | 1.218 | 1.209 | 1.257 | 1.160 |
| clang-14 -O3 | 0.952 | 0.873 | 0.990 | 1.651 | 1.129 | 1.231 | 1.056 | 1.207 | 1.230 | 1.229 | 1.414 |
| clang-14 -O2 | 0.950 | 0.865 | 0.990 | 1.650 | 1.129 | 1.232 | 1.050 | 1.206 | 1.229 | 1.229 | 1.400 |
| clang-14 -Os | 0.959 | 0.859 | 0.991 | 1.649 | 1.123 | 1.230 | 1.039 | 1.227 | 1.288 | 1.301 | 1.406 |
| clang-14 -O | 0.967 | 0.862 | 0.993 | 1.667 | 1.132 | 1.225 | 1.060 | 1.222 | 1.258 | 1.271 | 1.439 |
| clang-17 -O3 | 0.952 | 0.872 | 0.969 | 1.639 | 1.129 | 1.231 | 1.050 | 1.208 | 1.227 | 1.205 | 1.374 |
| clang-17 -O2 | 0.953 | 0.865 | 0.970 | 1.633 | 1.129 | 1.228 | 1.044 | 1.207 | 1.228 | 1.206 | 1.358 |
| clang-17 -Os | 0.966 | 0.862 | 0.981 | 1.631 | 1.128 | 1.240 | 1.047 | 1.230 | 1.283 | 1.272 | 1.383 |
| clang-17 -O | 0.968 | 0.862 | 0.975 | 1.634 | 1.137 | 1.231 | 1.043 | 1.228 | 1.254 | 1.275 | 1.403 |
| clang-18 -O3 | 0.952 | 0.871 | 0.970 | 1.628 | 1.128 | 1.227 | 1.046 | 1.207 | 1.224 | 1.206 | 1.423 |
| clang-18 -O2 | 0.951 | 0.865 | 0.970 | 1.626 | 1.128 | 1.232 | 1.048 | 1.206 | 1.227 | 1.210 | 1.414 |
| clang-18 -Os | 0.963 | 0.859 | 0.973 | 1.627 | 1.126 | 1.231 | 1.045 | 1.229 | 1.259 | 1.268 | 1.433 |
| clang-18 -O | 0.970 | 0.868 | 0.975 | 1.639 | 1.140 | 1.235 | 1.046 | 1.237 | 1.269 | 1.279 | 1.428 |
| **geomean** | **0.927** | **0.827** | **0.976** | **1.637** | **1.083** | **1.184** | **1.015** | **1.182** | **1.159** | **1.204** | **1.216** |

### Does `a64/ucode` win?

_ratio = other ÷ a64/ucode (median cycles). **>1 ⇒ a64/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.545 | 0.487 | 0.601 | 0.642 | 0.703 | 0.598 | 0.574 | 0.692 | 0.641 | 0.697 | 0.605 |
| gcc-11 -O2 | 0.561 | 0.494 | 0.591 | 0.648 | 0.706 | 0.605 | 0.587 | 0.721 | 0.679 | 0.730 | 0.672 |
| gcc-11 -Os | 0.525 | 0.460 | 0.607 | 0.603 | 0.669 | 0.575 | 0.712 | 0.677 | 0.623 | 0.688 | 0.628 |
| gcc-11 -O | 0.539 | 0.479 | 0.597 | 0.629 | 0.688 | 0.609 | 0.566 | 0.689 | 0.669 | 0.724 | 0.679 |
| gcc-12 -O3 | 0.553 | 0.495 | 0.586 | 0.650 | 0.712 | 0.606 | 0.582 | 0.699 | 0.644 | 0.700 | 0.619 |
| gcc-12 -O2 | 0.564 | 0.492 | 0.587 | 0.649 | 0.712 | 0.606 | 0.587 | 0.726 | 0.674 | 0.727 | 0.672 |
| gcc-12 -Os | 0.526 | 0.459 | 0.613 | 0.605 | 0.671 | 0.581 | 0.714 | 0.681 | 0.626 | 0.692 | 0.624 |
| gcc-12 -O | 0.540 | 0.479 | 0.598 | 0.630 | 0.686 | 0.607 | 0.567 | 0.689 | 0.668 | 0.718 | 0.665 |
| gcc-13 -O3 | 0.571 | 0.504 | 0.584 | 0.666 | 0.729 | 0.621 | 0.598 | 0.723 | 0.680 | 0.721 | 0.634 |
| gcc-13 -O2 | 0.573 | 0.501 | 0.579 | 0.662 | 0.725 | 0.617 | 0.596 | 0.736 | 0.686 | 0.728 | 0.658 |
| gcc-13 -Os | 0.540 | 0.468 | 0.599 | 0.617 | 0.684 | 0.592 | 0.728 | 0.702 | 0.640 | 0.712 | 0.613 |
| gcc-13 -O | 0.543 | 0.483 | 0.600 | 0.634 | 0.688 | 0.607 | 0.568 | 0.692 | 0.687 | 0.714 | 0.658 |
| clang-14 -O3 | 0.576 | 0.529 | 0.599 | 0.684 | 0.746 | 0.640 | 0.606 | 0.731 | 0.745 | 0.744 | 0.856 |
| clang-14 -O2 | 0.576 | 0.525 | 0.600 | 0.684 | 0.747 | 0.637 | 0.606 | 0.731 | 0.745 | 0.745 | 0.848 |
| clang-14 -Os | 0.581 | 0.521 | 0.601 | 0.681 | 0.746 | 0.630 | 0.606 | 0.744 | 0.781 | 0.789 | 0.853 |
| clang-14 -O | 0.580 | 0.517 | 0.596 | 0.679 | 0.735 | 0.636 | 0.600 | 0.733 | 0.755 | 0.763 | 0.863 |
| clang-17 -O3 | 0.581 | 0.532 | 0.591 | 0.689 | 0.751 | 0.640 | 0.610 | 0.737 | 0.748 | 0.735 | 0.838 |
| clang-17 -O2 | 0.584 | 0.530 | 0.594 | 0.692 | 0.752 | 0.640 | 0.612 | 0.739 | 0.752 | 0.739 | 0.832 |
| clang-17 -Os | 0.592 | 0.528 | 0.601 | 0.691 | 0.760 | 0.642 | 0.613 | 0.754 | 0.787 | 0.780 | 0.848 |
| clang-17 -O | 0.592 | 0.527 | 0.597 | 0.696 | 0.753 | 0.638 | 0.612 | 0.751 | 0.768 | 0.780 | 0.859 |
| clang-18 -O3 | 0.585 | 0.535 | 0.596 | 0.693 | 0.754 | 0.643 | 0.614 | 0.741 | 0.752 | 0.741 | 0.874 |
| clang-18 -O2 | 0.585 | 0.532 | 0.597 | 0.694 | 0.758 | 0.645 | 0.615 | 0.742 | 0.754 | 0.744 | 0.870 |
| clang-18 -Os | 0.592 | 0.528 | 0.598 | 0.692 | 0.756 | 0.642 | 0.615 | 0.755 | 0.773 | 0.779 | 0.881 |
| clang-18 -O | 0.592 | 0.530 | 0.595 | 0.695 | 0.753 | 0.638 | 0.610 | 0.755 | 0.774 | 0.780 | 0.871 |
| **geomean** | **0.566** | **0.505** | **0.596** | **0.662** | **0.724** | **0.620** | **0.611** | **0.722** | **0.708** | **0.736** | **0.743** |
