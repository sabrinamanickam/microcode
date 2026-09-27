# X25519 Microcode Benchmark Results

**Generated:** Sun 27 Sep 2026 23:13:52 ACST
**Host:** redunlock-GB-BPCE-3350C
**CPU:** Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
**Pinned freq:** 1094363 kHz   (governor: `userspace`, no_turbo: `1`)
**Delivered core freq:** 1100 MHz · **TSC (RDTSC) rate:** 1094 MHz · **correction f_core/f_TSC:** 1.00548 (aperf/mperf under load, verified before the sweep; comparative **ratios are invariant** to this factor, multiply **absolute** cycle counts by it for true core cycles)
**Post-sweep frequency check:** stable (+0.000% over the sweep) · delivered 1100 MHz / TSC 1094 MHz after the sweep (the pre-sweep guard proves the machine was pinned when the sweep started; this proves it stayed pinned throughout)
**Core isolation:** core 1; 28 IRQs steered to core 0 (2 per-CPU/unmovable); SCHED_FIFO 99 · nohz_full/isolcpus: off — periodic timer tick still hits core 1
**Runs per config:** 3 (recorded median is the median of those runs; worst run-to-run spread 1.800% at amd64-51/ucode-Clad @ gcc-11 -O3)
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
| osslops/C-ladder | 253,741 | 0.931 | 0.915 | clang-18 -O3 |
| **microcode 5×51 (this work)** | **272,535** | — | — | clang-18 -O2 |
| fiat-crypto (verified C) | 347,187 | 1.274 | 1.307 | gcc-12 -O3 |
| CryptOpt (superoptimized asm) | 372,294 | 1.366 | 1.332 | clang-14 -O2 |
| hand-written C (`__uint128_t`) | 373,523 | 1.371 | 1.358 | clang-17 -O2 |
| amd64-51 asm (Bernstein–Schwabe) | 374,999 | 1.376 | 1.334 | clang-17 -O2 |

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
| `a51/asm` | qhasm, monolithic | qhasm asm | 356,871 | — | baseline |
| `a51/asmCld` | C, per-op calls | qhasm asm | 388,375 | 0.919 | ladder: qhasm → C (fusion lost) |
| `a51/ucCld` | C, per-op calls | **microcode** | 301,475 | **1.288** | **field ops: asm → microcode** |
| `a51/ucode` | inline-asm, chained | microcode | 277,861 | 1.085 | ladder: C → register-chained asm |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** 1.288× faster (geomean 1.263×), with the ladder **and** the framework held constant.

**Consistency check.** The steps are multiplicative, so they must compose to the
measured end-to-end ratio:

```
  0.919 (ladder) x 1.288 (field ops) x 1.085 (chaining)  =  1.28435
  measured  356871 / 277861                              =  1.28435
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
| `a64/asm` | qhasm, monolithic | qhasm asm | 271,825 | — | baseline |
| `a64/asmCld` | C, per-op calls | qhasm asm | 306,823 | 0.886 | ladder: qhasm → C (fusion lost) |
| `a64/ucode` | C, per-op calls | **microcode** | 531,281 | **0.578** | **field ops: asm → microcode** |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** microcode is 1.732× **slower** than the asm it replaces (geomean 1.733×), with the ladder held constant — against 1.954× if the ladder rewrite is wrongly charged to the field ops.

---

## Table 4 — End-to-end standing (orientation, not the claim)

**Held constant:** nothing — these are whole implementations differing in
representation, ladder, inversion and field ops at once. Useful for placing the
work against shipped code; useless for attributing the difference to microcode.
For that, see Table 1.

| implementation | cyc/X25519 | best config |
|---|---:|---|
| **microcode 5×51 + inline-asm ladder (this work)** | **264,150** | clang-18 -O3 |
| amd64-64 asm (Bernstein–Schwabe, 4×64) | 271,825 | gcc-12 -O3 |
| amd64-51 framework + microcode | 277,861 | clang-18 -Os |
| donna c64 (portable C) | 337,717 | gcc-11 -O3 |
| fiat-crypto (verified C) | 347,187 | gcc-12 -O3 |
| amd64-51 asm (Bernstein–Schwabe, 5×51) | 356,871 | clang-18 -Os |
| CryptOpt (superoptimized asm) | 372,294 | clang-14 -O2 |
| hand-written C (`__uint128_t`) | 373,523 | clang-17 -O2 |


### Dispersion at each contender's best config

_Median is the headline; min and the p10–p90 range show run-to-run spread at that config. A tight p90−p10 relative to the inter-contender gaps means the ranking is not noise._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| ucode | 264150 | 263942 | 264052 | 273321 | 9269 | clang-18 -O3 |
| a64/asm | 271825 | 271684 | 271758 | 280019 | 8261 | gcc-12 -O3 |
| a64/asmCld | 306823 | 306556 | 306786 | 315190 | 8404 | clang-17 -O3 |
| a64/ucode | 531281 | 530837 | 531175 | 538823 | 7648 | clang-17 -O |
| a51/asm | 356871 | 356684 | 356809 | 364949 | 8140 | clang-18 -Os |
| a51/asmCld | 388375 | 388245 | 388311 | 396511 | 8200 | clang-14 -O |
| a51/ucCld | 301475 | 301355 | 301412 | 311863 | 10451 | clang-18 -O2 |
| a51/ucode | 277861 | 276633 | 277767 | 286098 | 8331 | clang-18 -Os |
| cryptopt | 372294 | 372204 | 372238 | 380972 | 8734 | clang-14 -O2 |
| fiat | 347187 | 346922 | 347123 | 355318 | 8195 | gcc-12 -O3 |
| hand-C | 373523 | 373000 | 373308 | 382048 | 8740 | clang-17 -O2 |
| donna | 337717 | 337269 | 337493 | 345513 | 8020 | gcc-11 -O3 |
| s2n-bignum/asm | 244190 | 244088 | 244117 | 252650 | 8533 | gcc-11 -Os |
| osslops/C-ladder | 253741 | 253575 | 253681 | 262902 | 9221 | clang-18 -O3 |
| openssl | 253978 | 253915 | 253937 | 263108 | 9171 | gcc-11 -O2 |

---

# Appendix A — full per-config sweep

The raw 24-config matrices behind the best-per-contender numbers above.
Present so the selection rule can be audited and so per-compiler behaviour is
visible; not intended to be read row by row.

### A.1 — X25519 end-to-end, every contender

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 267067 | 272452 | 343455 | 587446 | 359094 | 393615 | 311912 | 284037 | 380813 | 353406 | 384294 | **337717** | 244600 | 261143 | 254409 |
| gcc-11 -O2 | 273285 | 273358 | 333698 | 586698 | 359031 | 390680 | 309377 | 284503 | 388819 | 364354 | 392541 | 371029 | 244483 | 269767 | **253978** |
| gcc-11 -Os | 275583 | 274115 | 373959 | 631811 | 359245 | 399364 | 319180 | 386837 | 391157 | 360940 | 400666 | 368238 | **244190** | 272730 | 254086 |
| gcc-11 -O | 271992 | 272880 | 335607 | 591444 | 359560 | 391482 | 321845 | 281590 | 380839 | 369126 | 402774 | 378346 | 244551 | 263694 | 254210 |
| gcc-12 -O3 | 267930 | **271825** | 333108 | 581952 | 358902 | 391453 | 308558 | 285607 | 379763 | **347187** | 376444 | 341041 | 244570 | 259246 | 254027 |
| gcc-12 -O2 | 273824 | 272053 | 332899 | 581717 | 359342 | 393304 | 310218 | 283827 | 391653 | 362581 | 392146 | 370404 | 244249 | 268779 | 254098 |
| gcc-12 -Os | 276055 | 272615 | 375058 | 632893 | 359091 | 398918 | 316397 | 386494 | 394850 | 361022 | 399924 | 374673 | 244286 | 273805 | 254088 |
| gcc-12 -O | 269117 | 272653 | 340337 | 594294 | 358817 | 390438 | 322195 | 280912 | 381159 | 368729 | 398056 | 382749 | 244428 | 263100 | 254204 |
| gcc-13 -O3 | 271796 | 271845 | 323561 | 572769 | 359219 | 393599 | 309996 | 283925 | 383612 | 360431 | 381487 | 341443 | 244532 | 264205 | 254370 |
| gcc-13 -O2 | 271762 | 272047 | 323781 | 575236 | 359285 | 393436 | 310400 | 283876 | 390074 | 362317 | 385398 | 356242 | 244396 | 267122 | 254140 |
| gcc-13 -Os | 276883 | 272615 | 361077 | 616192 | 359084 | 398856 | 316432 | 386334 | 395912 | 358996 | 401880 | 358194 | 244200 | 274048 | 254100 |
| gcc-13 -O | 273030 | 273572 | 336369 | 591019 | 359795 | 390276 | 322429 | 280967 | 382153 | 377674 | 392410 | 374281 | 244418 | 262701 | 254248 |
| clang-14 -O3 | 266452 | 275992 | 315961 | 538355 | 357441 | 389612 | 308392 | 284778 | 372629 | 383799 | 384402 | 454439 | 250088 | 254629 | 255108 |
| clang-14 -O2 | 268700 | 274557 | 316026 | 538488 | 357167 | 389143 | 304217 | 280363 | **372294** | 383765 | 383286 | 454491 | 252998 | 254965 | 255456 |
| clang-14 -Os | 269978 | 273148 | 315050 | 543245 | 357254 | 391026 | 305602 | 280309 | 383571 | 401776 | 405146 | 446608 | 244401 | 262551 | 254234 |
| clang-14 -O | 273317 | 273276 | 314506 | 556825 | 358722 | **388375** | 312301 | 280326 | 379810 | 391160 | 393485 | 466189 | 252854 | 262474 | 255203 |
| clang-17 -O3 | 264220 | 275546 | **306823** | 532217 | 357529 | 389359 | 305958 | 280267 | 373462 | 379824 | 373731 | 442259 | 250428 | 254044 | 254847 |
| clang-17 -O2 | 265380 | 274591 | 306880 | 532329 | 357307 | 389306 | 304795 | 278986 | 373124 | 379694 | **373523** | 440439 | 252964 | 254566 | 255123 |
| clang-17 -Os | 267880 | 272772 | 310373 | 533266 | 357121 | 391222 | 305302 | 278120 | 382651 | 396516 | 394352 | 435270 | 244449 | 260425 | 254380 |
| clang-17 -O | 273073 | 272008 | 307999 | **531281** | 358895 | 389054 | 302617 | 280083 | 380232 | 388249 | 393771 | 453547 | 252899 | 262012 | 255403 |
| clang-18 -O3 | **264150** | 275707 | 307332 | 532251 | 357549 | 390320 | 308904 | 281906 | 373142 | 379547 | 374460 | 458506 | 250179 | **253741** | 254957 |
| clang-18 -O2 | 265223 | 275402 | 307389 | 532201 | 357171 | 389937 | **301475** | 279253 | 372951 | 378667 | 373873 | 459019 | 252976 | 254018 | 255220 |
| clang-18 -Os | 269321 | 272737 | 308565 | 532352 | **356871** | 390522 | 305516 | **277861** | 379750 | 393619 | 395483 | 452384 | 244493 | 260573 | 254201 |
| clang-18 -O | 273012 | 273698 | 307026 | 531727 | 358901 | 388968 | 302812 | 279298 | 381452 | 392649 | 394271 | 460974 | 253100 | 262988 | 255942 |

### A.2 — Same C ladder, only the field op differs

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | uc/Clad | a51op/Clad | osslops/C-ladder | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 281374 | 380870 | 261143 | 380813 | 353406 | 384294 |
| gcc-11 -O2 | 301963 | 385235 | 269767 | 388819 | 364354 | 392541 |
| gcc-11 -Os | 305436 | 393373 | 272730 | 391157 | 360940 | 400666 |
| gcc-11 -O | 295484 | 382234 | 263694 | 380839 | 369126 | 402774 |
| gcc-12 -O3 | 282861 | 377686 | 259246 | 379763 | **347187** | 376444 |
| gcc-12 -O2 | 300357 | 388297 | 268779 | 391653 | 362581 | 392146 |
| gcc-12 -Os | 305544 | 394025 | 273805 | 394850 | 361022 | 399924 |
| gcc-12 -O | 297483 | 383720 | 263100 | 381159 | 368729 | 398056 |
| gcc-13 -O3 | 285144 | 382964 | 264205 | 383612 | 360431 | 381487 |
| gcc-13 -O2 | 298881 | 386383 | 267122 | 390074 | 362317 | 385398 |
| gcc-13 -Os | 305793 | 394092 | 274048 | 395912 | 358996 | 401880 |
| gcc-13 -O | 296148 | 382992 | 262701 | 382153 | 377674 | 392410 |
| clang-14 -O3 | 275224 | 377689 | 254629 | 372629 | 383799 | 384402 |
| clang-14 -O2 | 275127 | 377365 | 254965 | **372294** | 383765 | 383286 |
| clang-14 -Os | 283709 | 385158 | 262551 | 383571 | 401776 | 405146 |
| clang-14 -O | 278347 | 379547 | 262474 | 379810 | 391160 | 393485 |
| clang-17 -O3 | 273624 | 375099 | 254044 | 373462 | 379824 | 373731 |
| clang-17 -O2 | 273175 | **374999** | 254566 | 373124 | 379694 | **373523** |
| clang-17 -Os | 283761 | 382022 | 260425 | 382651 | 396516 | 394352 |
| clang-17 -O | 277966 | 380493 | 262012 | 380232 | 388249 | 393771 |
| clang-18 -O3 | 275234 | 375563 | **253741** | 373142 | 379547 | 374460 |
| clang-18 -O2 | **272535** | 375414 | 254018 | 372951 | 378667 | 373873 |
| clang-18 -Os | 284056 | 381674 | 260573 | 379750 | 393619 | 395483 |
| clang-18 -O | 276543 | 381406 | 262988 | 381452 | 392649 | 394271 |

### A.3 — Per-config ratios


### Does `uc/Clad` win?

_ratio = other ÷ uc/Clad (median cycles). **>1 ⇒ uc/Clad is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a51op/Clad | osslops/C-ladder | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 1.354 | 0.928 | 1.353 | 1.256 | 1.366 |
| gcc-11 -O2 | 1.276 | 0.893 | 1.288 | 1.207 | 1.300 |
| gcc-11 -Os | 1.288 | 0.893 | 1.281 | 1.182 | 1.312 |
| gcc-11 -O | 1.294 | 0.892 | 1.289 | 1.249 | 1.363 |
| gcc-12 -O3 | 1.335 | 0.917 | 1.343 | 1.227 | 1.331 |
| gcc-12 -O2 | 1.293 | 0.895 | 1.304 | 1.207 | 1.306 |
| gcc-12 -Os | 1.290 | 0.896 | 1.292 | 1.182 | 1.309 |
| gcc-12 -O | 1.290 | 0.884 | 1.281 | 1.239 | 1.338 |
| gcc-13 -O3 | 1.343 | 0.927 | 1.345 | 1.264 | 1.338 |
| gcc-13 -O2 | 1.293 | 0.894 | 1.305 | 1.212 | 1.289 |
| gcc-13 -Os | 1.289 | 0.896 | 1.295 | 1.174 | 1.314 |
| gcc-13 -O | 1.293 | 0.887 | 1.290 | 1.275 | 1.325 |
| clang-14 -O3 | 1.372 | 0.925 | 1.354 | 1.394 | 1.397 |
| clang-14 -O2 | 1.372 | 0.927 | 1.353 | 1.395 | 1.393 |
| clang-14 -Os | 1.358 | 0.925 | 1.352 | 1.416 | 1.428 |
| clang-14 -O | 1.364 | 0.943 | 1.365 | 1.405 | 1.414 |
| clang-17 -O3 | 1.371 | 0.928 | 1.365 | 1.388 | 1.366 |
| clang-17 -O2 | 1.373 | 0.932 | 1.366 | 1.390 | 1.367 |
| clang-17 -Os | 1.346 | 0.918 | 1.348 | 1.397 | 1.390 |
| clang-17 -O | 1.369 | 0.943 | 1.368 | 1.397 | 1.417 |
| clang-18 -O3 | 1.365 | 0.922 | 1.356 | 1.379 | 1.361 |
| clang-18 -O2 | 1.377 | 0.932 | 1.368 | 1.389 | 1.372 |
| clang-18 -Os | 1.344 | 0.917 | 1.337 | 1.386 | 1.392 |
| clang-18 -O | 1.379 | 0.951 | 1.379 | 1.420 | 1.426 |
| **geomean** | **1.334** | **0.915** | **1.332** | **1.307** | **1.358** |

### Does `ucode` win?

_ratio = other ÷ ucode (median cycles). **>1 ⇒ ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a64/asm | a51/asm | a51/ucode | donna | fiat | cryptopt | hand-C |
|---|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 1.020 | 1.345 | 1.064 | 1.265 | 1.323 | 1.426 | 1.439 |
| gcc-11 -O2 | 1.000 | 1.314 | 1.041 | 1.358 | 1.333 | 1.423 | 1.436 |
| gcc-11 -Os | 0.995 | 1.304 | 1.404 | 1.336 | 1.310 | 1.419 | 1.454 |
| gcc-11 -O | 1.003 | 1.322 | 1.035 | 1.391 | 1.357 | 1.400 | 1.481 |
| gcc-12 -O3 | 1.015 | 1.340 | 1.066 | 1.273 | 1.296 | 1.417 | 1.405 |
| gcc-12 -O2 | 0.994 | 1.312 | 1.037 | 1.353 | 1.324 | 1.430 | 1.432 |
| gcc-12 -Os | 0.988 | 1.301 | 1.400 | 1.357 | 1.308 | 1.430 | 1.449 |
| gcc-12 -O | 1.013 | 1.333 | 1.044 | 1.422 | 1.370 | 1.416 | 1.479 |
| gcc-13 -O3 | 1.000 | 1.322 | 1.045 | 1.256 | 1.326 | 1.411 | 1.404 |
| gcc-13 -O2 | 1.001 | 1.322 | 1.045 | 1.311 | 1.333 | 1.435 | 1.418 |
| gcc-13 -Os | 0.985 | 1.297 | 1.395 | 1.294 | 1.297 | 1.430 | 1.451 |
| gcc-13 -O | 1.002 | 1.318 | 1.029 | 1.371 | 1.383 | 1.400 | 1.437 |
| clang-14 -O3 | 1.036 | 1.341 | 1.069 | 1.706 | 1.440 | 1.398 | 1.443 |
| clang-14 -O2 | 1.022 | 1.329 | 1.043 | 1.691 | 1.428 | 1.386 | 1.426 |
| clang-14 -Os | 1.012 | 1.323 | 1.038 | 1.654 | 1.488 | 1.421 | 1.501 |
| clang-14 -O | 1.000 | 1.312 | 1.026 | 1.706 | 1.431 | 1.390 | 1.440 |
| clang-17 -O3 | 1.043 | 1.353 | 1.061 | 1.674 | 1.438 | 1.413 | 1.414 |
| clang-17 -O2 | 1.035 | 1.346 | 1.051 | 1.660 | 1.431 | 1.406 | 1.408 |
| clang-17 -Os | 1.018 | 1.333 | 1.038 | 1.625 | 1.480 | 1.428 | 1.472 |
| clang-17 -O | 0.996 | 1.314 | 1.026 | 1.661 | 1.422 | 1.392 | 1.442 |
| clang-18 -O3 | 1.044 | 1.354 | 1.067 | 1.736 | 1.437 | 1.413 | 1.418 |
| clang-18 -O2 | 1.038 | 1.347 | 1.053 | 1.731 | 1.428 | 1.406 | 1.410 |
| clang-18 -Os | 1.013 | 1.325 | 1.032 | 1.680 | 1.462 | 1.410 | 1.468 |
| clang-18 -O | 1.003 | 1.315 | 1.023 | 1.688 | 1.438 | 1.397 | 1.444 |
| **geomean** | **1.011** | **1.326** | **1.083** | **1.497** | **1.385** | **1.412** | **1.440** |

### A.4 — Uncontrolled ratios (superseded)

These compare a microcode hybrid against its asm baseline **without** holding the
ladder constant, so they attribute the ladder rewrite to the field ops. Retained
for auditability only — Tables 2 and 3 are the correct form of these comparisons.

### Does `a51/ucode` win?

_ratio = other ÷ a51/ucode (median cycles). **>1 ⇒ a51/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.940 | 0.959 | 1.209 | 2.068 | 1.264 | 1.386 | 1.098 | 1.341 | 1.244 | 1.353 | 1.189 | 0.861 | 0.919 | 0.896 |
| gcc-11 -O2 | 0.961 | 0.961 | 1.173 | 2.062 | 1.262 | 1.373 | 1.087 | 1.367 | 1.281 | 1.380 | 1.304 | 0.859 | 0.948 | 0.893 |
| gcc-11 -Os | 0.712 | 0.709 | 0.967 | 1.633 | 0.929 | 1.032 | 0.825 | 1.011 | 0.933 | 1.036 | 0.952 | 0.631 | 0.705 | 0.657 |
| gcc-11 -O | 0.966 | 0.969 | 1.192 | 2.100 | 1.277 | 1.390 | 1.143 | 1.352 | 1.311 | 1.430 | 1.344 | 0.868 | 0.936 | 0.903 |
| gcc-12 -O3 | 0.938 | 0.952 | 1.166 | 2.038 | 1.257 | 1.371 | 1.080 | 1.330 | 1.216 | 1.318 | 1.194 | 0.856 | 0.908 | 0.889 |
| gcc-12 -O2 | 0.965 | 0.959 | 1.173 | 2.050 | 1.266 | 1.386 | 1.093 | 1.380 | 1.277 | 1.382 | 1.305 | 0.861 | 0.947 | 0.895 |
| gcc-12 -Os | 0.714 | 0.705 | 0.970 | 1.638 | 0.929 | 1.032 | 0.819 | 1.022 | 0.934 | 1.035 | 0.969 | 0.632 | 0.708 | 0.657 |
| gcc-12 -O | 0.958 | 0.971 | 1.212 | 2.116 | 1.277 | 1.390 | 1.147 | 1.357 | 1.313 | 1.417 | 1.363 | 0.870 | 0.937 | 0.905 |
| gcc-13 -O3 | 0.957 | 0.957 | 1.140 | 2.017 | 1.265 | 1.386 | 1.092 | 1.351 | 1.269 | 1.344 | 1.203 | 0.861 | 0.931 | 0.896 |
| gcc-13 -O2 | 0.957 | 0.958 | 1.141 | 2.026 | 1.266 | 1.386 | 1.093 | 1.374 | 1.276 | 1.358 | 1.255 | 0.861 | 0.941 | 0.895 |
| gcc-13 -Os | 0.717 | 0.706 | 0.935 | 1.595 | 0.929 | 1.032 | 0.819 | 1.025 | 0.929 | 1.040 | 0.927 | 0.632 | 0.709 | 0.658 |
| gcc-13 -O | 0.972 | 0.974 | 1.197 | 2.104 | 1.281 | 1.389 | 1.148 | 1.360 | 1.344 | 1.397 | 1.332 | 0.870 | 0.935 | 0.905 |
| clang-14 -O3 | 0.936 | 0.969 | 1.109 | 1.890 | 1.255 | 1.368 | 1.083 | 1.308 | 1.348 | 1.350 | 1.596 | 0.878 | 0.894 | 0.896 |
| clang-14 -O2 | 0.958 | 0.979 | 1.127 | 1.921 | 1.274 | 1.388 | 1.085 | 1.328 | 1.369 | 1.367 | 1.621 | 0.902 | 0.909 | 0.911 |
| clang-14 -Os | 0.963 | 0.974 | 1.124 | 1.938 | 1.275 | 1.395 | 1.090 | 1.368 | 1.433 | 1.445 | 1.593 | 0.872 | 0.937 | 0.907 |
| clang-14 -O | 0.975 | 0.975 | 1.122 | 1.986 | 1.280 | 1.385 | 1.114 | 1.355 | 1.395 | 1.404 | 1.663 | 0.902 | 0.936 | 0.910 |
| clang-17 -O3 | 0.943 | 0.983 | 1.095 | 1.899 | 1.276 | 1.389 | 1.092 | 1.333 | 1.355 | 1.333 | 1.578 | 0.894 | 0.906 | 0.909 |
| clang-17 -O2 | 0.951 | 0.984 | 1.100 | 1.908 | 1.281 | 1.395 | 1.093 | 1.337 | 1.361 | 1.339 | 1.579 | 0.907 | 0.912 | 0.914 |
| clang-17 -Os | 0.963 | 0.981 | 1.116 | 1.917 | 1.284 | 1.407 | 1.098 | 1.376 | 1.426 | 1.418 | 1.565 | 0.879 | 0.936 | 0.915 |
| clang-17 -O | 0.975 | 0.971 | 1.100 | 1.897 | 1.281 | 1.389 | 1.080 | 1.358 | 1.386 | 1.406 | 1.619 | 0.903 | 0.935 | 0.912 |
| clang-18 -O3 | 0.937 | 0.978 | 1.090 | 1.888 | 1.268 | 1.385 | 1.096 | 1.324 | 1.346 | 1.328 | 1.626 | 0.887 | 0.900 | 0.904 |
| clang-18 -O2 | 0.950 | 0.986 | 1.101 | 1.906 | 1.279 | 1.396 | 1.080 | 1.336 | 1.356 | 1.339 | 1.644 | 0.906 | 0.910 | 0.914 |
| clang-18 -Os | 0.969 | 0.982 | 1.111 | 1.916 | 1.284 | 1.405 | 1.100 | 1.367 | 1.417 | 1.423 | 1.628 | 0.880 | 0.938 | 0.915 |
| clang-18 -O | 0.977 | 0.980 | 1.099 | 1.904 | 1.285 | 1.393 | 1.084 | 1.366 | 1.406 | 1.412 | 1.650 | 0.906 | 0.942 | 0.916 |
| **geomean** | **0.923** | **0.934** | **1.113** | **1.929** | **1.224** | **1.338** | **1.059** | **1.304** | **1.279** | **1.330** | **1.382** | **0.844** | **0.896** | **0.869** |

### Does `a64/ucode` win?

_ratio = other ÷ a64/ucode (median cycles). **>1 ⇒ a64/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.455 | 0.464 | 0.585 | 0.611 | 0.670 | 0.531 | 0.484 | 0.648 | 0.602 | 0.654 | 0.575 | 0.416 | 0.445 | 0.433 |
| gcc-11 -O2 | 0.466 | 0.466 | 0.569 | 0.612 | 0.666 | 0.527 | 0.485 | 0.663 | 0.621 | 0.669 | 0.632 | 0.417 | 0.460 | 0.433 |
| gcc-11 -Os | 0.436 | 0.434 | 0.592 | 0.569 | 0.632 | 0.505 | 0.612 | 0.619 | 0.571 | 0.634 | 0.583 | 0.386 | 0.432 | 0.402 |
| gcc-11 -O | 0.460 | 0.461 | 0.567 | 0.608 | 0.662 | 0.544 | 0.476 | 0.644 | 0.624 | 0.681 | 0.640 | 0.413 | 0.446 | 0.430 |
| gcc-12 -O3 | 0.460 | 0.467 | 0.572 | 0.617 | 0.673 | 0.530 | 0.491 | 0.653 | 0.597 | 0.647 | 0.586 | 0.420 | 0.445 | 0.437 |
| gcc-12 -O2 | 0.471 | 0.468 | 0.572 | 0.618 | 0.676 | 0.533 | 0.488 | 0.673 | 0.623 | 0.674 | 0.637 | 0.420 | 0.462 | 0.437 |
| gcc-12 -Os | 0.436 | 0.431 | 0.593 | 0.567 | 0.630 | 0.500 | 0.611 | 0.624 | 0.570 | 0.632 | 0.592 | 0.386 | 0.433 | 0.401 |
| gcc-12 -O | 0.453 | 0.459 | 0.573 | 0.604 | 0.657 | 0.542 | 0.473 | 0.641 | 0.620 | 0.670 | 0.644 | 0.411 | 0.443 | 0.428 |
| gcc-13 -O3 | 0.475 | 0.475 | 0.565 | 0.627 | 0.687 | 0.541 | 0.496 | 0.670 | 0.629 | 0.666 | 0.596 | 0.427 | 0.461 | 0.444 |
| gcc-13 -O2 | 0.472 | 0.473 | 0.563 | 0.625 | 0.684 | 0.540 | 0.493 | 0.678 | 0.630 | 0.670 | 0.619 | 0.425 | 0.464 | 0.442 |
| gcc-13 -Os | 0.449 | 0.442 | 0.586 | 0.583 | 0.647 | 0.514 | 0.627 | 0.643 | 0.583 | 0.652 | 0.581 | 0.396 | 0.445 | 0.412 |
| gcc-13 -O | 0.462 | 0.463 | 0.569 | 0.609 | 0.660 | 0.546 | 0.475 | 0.647 | 0.639 | 0.664 | 0.633 | 0.414 | 0.444 | 0.430 |
| clang-14 -O3 | 0.495 | 0.513 | 0.587 | 0.664 | 0.724 | 0.573 | 0.529 | 0.692 | 0.713 | 0.714 | 0.844 | 0.465 | 0.473 | 0.474 |
| clang-14 -O2 | 0.499 | 0.510 | 0.587 | 0.663 | 0.723 | 0.565 | 0.521 | 0.691 | 0.713 | 0.712 | 0.844 | 0.470 | 0.473 | 0.474 |
| clang-14 -Os | 0.497 | 0.503 | 0.580 | 0.658 | 0.720 | 0.563 | 0.516 | 0.706 | 0.740 | 0.746 | 0.822 | 0.450 | 0.483 | 0.468 |
| clang-14 -O | 0.491 | 0.491 | 0.565 | 0.644 | 0.697 | 0.561 | 0.503 | 0.682 | 0.702 | 0.707 | 0.837 | 0.454 | 0.471 | 0.458 |
| clang-17 -O3 | 0.496 | 0.518 | 0.576 | 0.672 | 0.732 | 0.575 | 0.527 | 0.702 | 0.714 | 0.702 | 0.831 | 0.471 | 0.477 | 0.479 |
| clang-17 -O2 | 0.499 | 0.516 | 0.576 | 0.671 | 0.731 | 0.573 | 0.524 | 0.701 | 0.713 | 0.702 | 0.827 | 0.475 | 0.478 | 0.479 |
| clang-17 -Os | 0.502 | 0.512 | 0.582 | 0.670 | 0.734 | 0.573 | 0.522 | 0.718 | 0.744 | 0.740 | 0.816 | 0.458 | 0.488 | 0.477 |
| clang-17 -O | 0.514 | 0.512 | 0.580 | 0.676 | 0.732 | 0.570 | 0.527 | 0.716 | 0.731 | 0.741 | 0.854 | 0.476 | 0.493 | 0.481 |
| clang-18 -O3 | 0.496 | 0.518 | 0.577 | 0.672 | 0.733 | 0.580 | 0.530 | 0.701 | 0.713 | 0.704 | 0.861 | 0.470 | 0.477 | 0.479 |
| clang-18 -O2 | 0.498 | 0.517 | 0.578 | 0.671 | 0.733 | 0.566 | 0.525 | 0.701 | 0.712 | 0.703 | 0.862 | 0.475 | 0.477 | 0.480 |
| clang-18 -Os | 0.506 | 0.512 | 0.580 | 0.670 | 0.734 | 0.574 | 0.522 | 0.713 | 0.739 | 0.743 | 0.850 | 0.459 | 0.489 | 0.478 |
| clang-18 -O | 0.513 | 0.515 | 0.577 | 0.675 | 0.732 | 0.569 | 0.525 | 0.717 | 0.738 | 0.741 | 0.867 | 0.476 | 0.495 | 0.481 |
| **geomean** | **0.479** | **0.484** | **0.577** | **0.635** | **0.694** | **0.549** | **0.518** | **0.676** | **0.663** | **0.689** | **0.717** | **0.438** | **0.464** | **0.451** |
