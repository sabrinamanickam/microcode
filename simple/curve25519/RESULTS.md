# X25519 Microcode Benchmark Results

**Generated:** Wed 02 Sep 2026 23:24:48 ACST
**Host:** redunlock-GB-BPCE-3350C
**CPU:** Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
**Pinned freq:** 1094400 kHz   (governor: `userspace`, no_turbo: `1`)
**Delivered core freq:** 1100 MHz · **TSC (RDTSC) rate:** 1094 MHz · **correction f_core/f_TSC:** 1.00548 (aperf/mperf under load, verified before the sweep; comparative **ratios are invariant** to this factor, multiply **absolute** cycle counts by it for true core cycles)
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
| **microcode 5×51 (this work)** | **304,946** | — | — | clang-18 -O3 |
| fiat-crypto (verified C) | 354,868 | 1.164 | 1.196 | gcc-12 -O3 |
| CryptOpt (superoptimized asm) | 379,832 | 1.246 | 1.220 | clang-17 -O3 |
| hand-written C (`__uint128_t`) | 381,186 | 1.250 | 1.242 | clang-18 -O2 |
| amd64-51 asm (Bernstein–Schwabe) | 383,343 | 1.257 | 1.223 | clang-14 -O3 |

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
| `a51/asm` | qhasm, monolithic | qhasm asm | 356,042 | — | baseline |
| `a51/asmCld` | C, per-op calls | qhasm asm | 386,845 | 0.920 | ladder: qhasm → C (fusion lost) |
| `a51/ucCld` | C, per-op calls | **microcode** | 328,807 | **1.177** | **field ops: asm → microcode** |
| `a51/ucode` | inline-asm, chained | microcode | 315,202 | 1.043 | ladder: C → register-chained asm |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** 1.177× faster (geomean 1.166×), with the ladder **and** the framework held constant.

**Consistency check.** The steps are multiplicative, so they must compose to the
measured end-to-end ratio:

```
  0.920 (ladder) x 1.177 (field ops) x 1.043 (chaining)  =  1.12957
  measured  356042 / 315202                              =  1.12957
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
| `a64/asm` | qhasm, monolithic | qhasm asm | 271,328 | — | baseline |
| `a64/asmCld` | C, per-op calls | qhasm asm | 306,387 | 0.886 | ladder: qhasm → C (fusion lost) |
| `a64/ucode` | C, per-op calls | **microcode** | 513,782 | **0.596** | **field ops: asm → microcode** |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** microcode is 1.677× **slower** than the asm it replaces (geomean 1.679×), with the ladder held constant — against 1.894× if the ladder rewrite is wrongly charged to the field ops.

---

## Table 4 — End-to-end standing (orientation, not the claim)

**Held constant:** nothing — these are whole implementations differing in
representation, ladder, inversion and field ops at once. Useful for placing the
work against shipped code; useless for attributing the difference to microcode.
For that, see Table 1.

| implementation | cyc/X25519 | best config |
|---|---:|---|
| amd64-64 asm (Bernstein–Schwabe, 4×64) | 271,328 | gcc-12 -O3 |
| **microcode 5×51 + inline-asm ladder (this work)** | **300,482** | clang-18 -O2 |
| amd64-51 framework + microcode | 315,202 | clang-18 -O |
| donna c64 (portable C) | 335,805 | gcc-11 -O3 |
| fiat-crypto (verified C) | 354,868 | gcc-12 -O3 |
| amd64-51 asm (Bernstein–Schwabe, 5×51) | 356,042 | clang-17 -O3 |
| CryptOpt (superoptimized asm) | 379,832 | clang-17 -O3 |
| hand-written C (`__uint128_t`) | 381,186 | clang-18 -O2 |


### Dispersion at each contender's best config

_Median is the headline; min and the p10–p90 range show run-to-run spread at that config. A tight p90−p10 relative to the inter-contender gaps means the ranking is not noise._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| ucode | 300482 | 300461 | 300471 | 307216 | 6745 | clang-18 -O2 |
| a64/asm | 271328 | 271318 | 271324 | 278086 | 6762 | gcc-12 -O3 |
| a64/asmCld | 306387 | 306274 | — | — | — | clang-17 -O2 |
| a64/ucode | 513782 | 510748 | — | — | — | clang-18 -O |
| a51/asm | 356042 | 355988 | 355997 | 362905 | 6908 | clang-17 -O3 |
| a51/asmCld | 386845 | 386811 | 386818 | 395839 | 9021 | clang-18 -O3 |
| a51/ucCld | 328807 | 328785 | 328790 | 334476 | 5686 | clang-18 -O |
| a51/ucode | 315202 | 313131 | 313789 | 321083 | 7294 | clang-18 -O |
| cryptopt | 379832 | 379820 | 379825 | 386669 | 6844 | clang-17 -O3 |
| fiat | 354868 | 354504 | 354837 | 361299 | 6462 | gcc-12 -O3 |
| hand-C | 381186 | 379950 | 381114 | 395600 | 14486 | clang-18 -O2 |
| donna | 335805 | 335633 | 335738 | 345481 | 9743 | gcc-11 -O3 |

---

# Appendix A — full per-config sweep

The raw 24-config matrices behind the best-per-contender numbers above.
Present so the selection rule can be audited and so per-compiler behaviour is
visible; not intended to be read row by row.

### A.1 — X25519 end-to-end, every contender

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 304467 | 272059 | 337961 | 563246 | 358231 | 391726 | 334881 | 318701 | 385501 | 357930 | 389137 | **335805** |
| gcc-11 -O2 | 309830 | 272061 | 327374 | 554061 | 358213 | 388763 | 332860 | 324365 | 398939 | 375355 | 403334 | 369405 |
| gcc-11 -Os | 311811 | 273655 | 370453 | 596636 | 359411 | 398184 | 341854 | 423838 | 404085 | 370024 | 410268 | 368729 |
| gcc-11 -O | 305890 | 272250 | 335596 | 569609 | 357517 | 390297 | 346039 | 321578 | 391444 | 380619 | 412454 | 378726 |
| gcc-12 -O3 | 304948 | **271328** | 333790 | 558036 | 358201 | 393093 | 333167 | 324216 | 384516 | **354868** | 386275 | 339122 |
| gcc-12 -O2 | 309885 | 271332 | 324133 | 556375 | 358224 | 392813 | 334409 | 324528 | 400482 | 371587 | 401066 | 369036 |
| gcc-12 -Os | 311804 | 271987 | 367884 | 595848 | 358280 | 397709 | 344240 | 423840 | 406005 | 375168 | 411012 | 372499 |
| gcc-12 -O | 306587 | 272083 | 340482 | 569668 | 359191 | 389326 | 344793 | 321393 | 392176 | 379343 | 409494 | 377590 |
| gcc-13 -O3 | 307264 | 271503 | 314968 | 546132 | 358230 | 393029 | 334050 | 321417 | 390840 | 367067 | 388697 | 339568 |
| gcc-13 -O2 | 310426 | 271408 | 314406 | 542524 | 358255 | 392842 | 334401 | 323915 | 398762 | 371490 | 394397 | 354713 |
| gcc-13 -Os | 312423 | 271975 | 350249 | 582503 | 358215 | 397687 | 344248 | 423797 | 405832 | 372289 | 413623 | 358728 |
| gcc-13 -O | 307034 | 272971 | 336458 | 567671 | 358426 | 389100 | 343739 | 321073 | 391840 | 387378 | 403733 | 369362 |
| clang-14 -O3 | 301999 | 272435 | 312784 | 522940 | 356262 | 387826 | 331516 | 315343 | 381364 | 388585 | 386670 | 440217 |
| clang-14 -O2 | 302021 | 272570 | 312971 | 529746 | 356159 | 388269 | 331520 | 315595 | 380642 | 388699 | 386743 | 440221 |
| clang-14 -Os | 304101 | 272476 | 314972 | 530704 | 356767 | 389939 | 330669 | 316973 | 389735 | 407837 | 410408 | 446751 |
| clang-14 -O | 305761 | 272780 | 314477 | 527700 | 359545 | 387468 | 334611 | 315615 | 386534 | 397544 | 401070 | 453812 |
| clang-17 -O3 | 300484 | 272322 | 306402 | 516050 | **356042** | 388432 | 331139 | 315438 | **379832** | 386487 | 382262 | 428100 |
| clang-17 -O2 | 300493 | 272124 | **306387** | 515659 | 356293 | 388470 | 331178 | 315492 | 380426 | 386336 | 382252 | 427138 |
| clang-17 -Os | 304661 | 271933 | 310282 | 516045 | 356297 | 389674 | 332295 | 318076 | 389658 | 405247 | 401335 | 435634 |
| clang-17 -O | 304602 | 271579 | 307878 | 514019 | 357875 | 387958 | 328833 | 315491 | 387604 | 395014 | 400765 | 440114 |
| clang-18 -O3 | 300488 | 272063 | 306982 | 514797 | 356930 | **386845** | 329558 | 315428 | 380387 | 386892 | 381202 | 443108 |
| clang-18 -O2 | **300482** | 272295 | 306913 | 520200 | 356091 | 389135 | 331249 | 315925 | 380346 | 385516 | **381186** | 444276 |
| clang-18 -Os | 304669 | 272089 | 308472 | 515960 | 356138 | 388992 | 329839 | 315629 | 390157 | 398630 | 401955 | 452158 |
| clang-18 -O | 304871 | 272130 | 306986 | **513782** | 357917 | 387995 | **328807** | **315202** | 388363 | 399267 | 401127 | 447381 |

### A.2 — Same C ladder, only the field op differs

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | uc/Clad | a51op/Clad | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 312499 | 387842 | 385501 | 357930 | 389137 |
| gcc-11 -O2 | 337631 | 394856 | 398939 | 375355 | 403334 |
| gcc-11 -Os | 341334 | 401105 | 404085 | 370024 | 410268 |
| gcc-11 -O | 334427 | 392765 | 391444 | 380619 | 412454 |
| gcc-12 -O3 | 315764 | 384864 | 384516 | **354868** | 386275 |
| gcc-12 -O2 | 333203 | 397618 | 400482 | 371587 | 401066 |
| gcc-12 -Os | 343026 | 403233 | 406005 | 375168 | 411012 |
| gcc-12 -O | 331315 | 394273 | 392176 | 379343 | 409494 |
| gcc-13 -O3 | 315019 | 392997 | 390840 | 367067 | 388697 |
| gcc-13 -O2 | 332077 | 395561 | 398762 | 371490 | 394397 |
| gcc-13 -Os | 342893 | 405129 | 405832 | 372289 | 413623 |
| gcc-13 -O | 332322 | 393019 | 391840 | 387378 | 403733 |
| clang-14 -O3 | 306786 | **383343** | 381364 | 388585 | 386670 |
| clang-14 -O2 | 306886 | 384129 | 380642 | 388699 | 386743 |
| clang-14 -Os | 315552 | 390397 | 389735 | 407837 | 410408 |
| clang-14 -O | 310049 | 387537 | 386534 | 397544 | 401070 |
| clang-17 -O3 | 304981 | 385531 | **379832** | 386487 | 382262 |
| clang-17 -O2 | 305003 | 385491 | 380426 | 386336 | 382252 |
| clang-17 -Os | 313876 | 389924 | 389658 | 405247 | 401335 |
| clang-17 -O | 311297 | 388264 | 387604 | 395014 | 400765 |
| clang-18 -O3 | **304946** | 385351 | 380387 | 386892 | 381202 |
| clang-18 -O2 | 305011 | 385493 | 380346 | 385516 | **381186** |
| clang-18 -Os | 315632 | 389904 | 390157 | 398630 | 401955 |
| clang-18 -O | 311216 | 389564 | 388363 | 399267 | 401127 |

### A.3 — Per-config ratios


### Does `uc/Clad` win?

_ratio = other ÷ uc/Clad (median cycles). **>1 ⇒ uc/Clad is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a51op/Clad | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|
| gcc-11 -O3 | 1.241 | 1.234 | 1.145 | 1.245 |
| gcc-11 -O2 | 1.169 | 1.182 | 1.112 | 1.195 |
| gcc-11 -Os | 1.175 | 1.184 | 1.084 | 1.202 |
| gcc-11 -O | 1.174 | 1.170 | 1.138 | 1.233 |
| gcc-12 -O3 | 1.219 | 1.218 | 1.124 | 1.223 |
| gcc-12 -O2 | 1.193 | 1.202 | 1.115 | 1.204 |
| gcc-12 -Os | 1.176 | 1.184 | 1.094 | 1.198 |
| gcc-12 -O | 1.190 | 1.184 | 1.145 | 1.236 |
| gcc-13 -O3 | 1.248 | 1.241 | 1.165 | 1.234 |
| gcc-13 -O2 | 1.191 | 1.201 | 1.119 | 1.188 |
| gcc-13 -Os | 1.182 | 1.184 | 1.086 | 1.206 |
| gcc-13 -O | 1.183 | 1.179 | 1.166 | 1.215 |
| clang-14 -O3 | 1.250 | 1.243 | 1.267 | 1.260 |
| clang-14 -O2 | 1.252 | 1.240 | 1.267 | 1.260 |
| clang-14 -Os | 1.237 | 1.235 | 1.292 | 1.301 |
| clang-14 -O | 1.250 | 1.247 | 1.282 | 1.294 |
| clang-17 -O3 | 1.264 | 1.245 | 1.267 | 1.253 |
| clang-17 -O2 | 1.264 | 1.247 | 1.267 | 1.253 |
| clang-17 -Os | 1.242 | 1.241 | 1.291 | 1.279 |
| clang-17 -O | 1.247 | 1.245 | 1.269 | 1.287 |
| clang-18 -O3 | 1.264 | 1.247 | 1.269 | 1.250 |
| clang-18 -O2 | 1.264 | 1.247 | 1.264 | 1.250 |
| clang-18 -Os | 1.235 | 1.236 | 1.263 | 1.273 |
| clang-18 -O | 1.252 | 1.248 | 1.283 | 1.289 |
| **geomean** | **1.223** | **1.220** | **1.196** | **1.242** |

### Does `ucode` win?

_ratio = other ÷ ucode (median cycles). **>1 ⇒ ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a64/asm | a51/asm | a51/ucode | donna | fiat | cryptopt | hand-C |
|---|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.894 | 1.177 | 1.047 | 1.103 | 1.176 | 1.266 | 1.278 |
| gcc-11 -O2 | 0.878 | 1.156 | 1.047 | 1.192 | 1.211 | 1.288 | 1.302 |
| gcc-11 -Os | 0.878 | 1.153 | 1.359 | 1.183 | 1.187 | 1.296 | 1.316 |
| gcc-11 -O | 0.890 | 1.169 | 1.051 | 1.238 | 1.244 | 1.280 | 1.348 |
| gcc-12 -O3 | 0.890 | 1.175 | 1.063 | 1.112 | 1.164 | 1.261 | 1.267 |
| gcc-12 -O2 | 0.876 | 1.156 | 1.047 | 1.191 | 1.199 | 1.292 | 1.294 |
| gcc-12 -Os | 0.872 | 1.149 | 1.359 | 1.195 | 1.203 | 1.302 | 1.318 |
| gcc-12 -O | 0.887 | 1.172 | 1.048 | 1.232 | 1.237 | 1.279 | 1.336 |
| gcc-13 -O3 | 0.884 | 1.166 | 1.046 | 1.105 | 1.195 | 1.272 | 1.265 |
| gcc-13 -O2 | 0.874 | 1.154 | 1.043 | 1.143 | 1.197 | 1.285 | 1.271 |
| gcc-13 -Os | 0.871 | 1.147 | 1.356 | 1.148 | 1.192 | 1.299 | 1.324 |
| gcc-13 -O | 0.889 | 1.167 | 1.046 | 1.203 | 1.262 | 1.276 | 1.315 |
| clang-14 -O3 | 0.902 | 1.180 | 1.044 | 1.458 | 1.287 | 1.263 | 1.280 |
| clang-14 -O2 | 0.902 | 1.179 | 1.045 | 1.458 | 1.287 | 1.260 | 1.281 |
| clang-14 -Os | 0.896 | 1.173 | 1.042 | 1.469 | 1.341 | 1.282 | 1.350 |
| clang-14 -O | 0.892 | 1.176 | 1.032 | 1.484 | 1.300 | 1.264 | 1.312 |
| clang-17 -O3 | 0.906 | 1.185 | 1.050 | 1.425 | 1.286 | 1.264 | 1.272 |
| clang-17 -O2 | 0.906 | 1.186 | 1.050 | 1.421 | 1.286 | 1.266 | 1.272 |
| clang-17 -Os | 0.893 | 1.169 | 1.044 | 1.430 | 1.330 | 1.279 | 1.317 |
| clang-17 -O | 0.892 | 1.175 | 1.036 | 1.445 | 1.297 | 1.272 | 1.316 |
| clang-18 -O3 | 0.905 | 1.188 | 1.050 | 1.475 | 1.288 | 1.266 | 1.269 |
| clang-18 -O2 | 0.906 | 1.185 | 1.051 | 1.479 | 1.283 | 1.266 | 1.269 |
| clang-18 -Os | 0.893 | 1.169 | 1.036 | 1.484 | 1.308 | 1.281 | 1.319 |
| clang-18 -O | 0.893 | 1.174 | 1.034 | 1.467 | 1.310 | 1.274 | 1.316 |
| **geomean** | **0.890** | **1.170** | **1.080** | **1.306** | **1.252** | **1.276** | **1.300** |

### A.4 — Uncontrolled ratios (superseded)

These compare a microcode hybrid against its asm baseline **without** holding the
ladder constant, so they attribute the ladder rewrite to the field ops. Retained
for auditability only — Tables 2 and 3 are the correct form of these comparisons.

### Does `a51/ucode` win?

_ratio = other ÷ a51/ucode (median cycles). **>1 ⇒ a51/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | cryptopt | fiat | hand-C | donna |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.955 | 0.854 | 1.060 | 1.767 | 1.124 | 1.229 | 1.051 | 1.210 | 1.123 | 1.221 | 1.054 |
| gcc-11 -O2 | 0.955 | 0.839 | 1.009 | 1.708 | 1.104 | 1.199 | 1.026 | 1.230 | 1.157 | 1.243 | 1.139 |
| gcc-11 -Os | 0.736 | 0.646 | 0.874 | 1.408 | 0.848 | 0.939 | 0.807 | 0.953 | 0.873 | 0.968 | 0.870 |
| gcc-11 -O | 0.951 | 0.847 | 1.044 | 1.771 | 1.112 | 1.214 | 1.076 | 1.217 | 1.184 | 1.283 | 1.178 |
| gcc-12 -O3 | 0.941 | 0.837 | 1.030 | 1.721 | 1.105 | 1.212 | 1.028 | 1.186 | 1.095 | 1.191 | 1.046 |
| gcc-12 -O2 | 0.955 | 0.836 | 0.999 | 1.714 | 1.104 | 1.210 | 1.030 | 1.234 | 1.145 | 1.236 | 1.137 |
| gcc-12 -Os | 0.736 | 0.642 | 0.868 | 1.406 | 0.845 | 0.938 | 0.812 | 0.958 | 0.885 | 0.970 | 0.879 |
| gcc-12 -O | 0.954 | 0.847 | 1.059 | 1.772 | 1.118 | 1.211 | 1.073 | 1.220 | 1.180 | 1.274 | 1.175 |
| gcc-13 -O3 | 0.956 | 0.845 | 0.980 | 1.699 | 1.115 | 1.223 | 1.039 | 1.216 | 1.142 | 1.209 | 1.056 |
| gcc-13 -O2 | 0.958 | 0.838 | 0.971 | 1.675 | 1.106 | 1.213 | 1.032 | 1.231 | 1.147 | 1.218 | 1.095 |
| gcc-13 -Os | 0.737 | 0.642 | 0.826 | 1.374 | 0.845 | 0.938 | 0.812 | 0.958 | 0.878 | 0.976 | 0.846 |
| gcc-13 -O | 0.956 | 0.850 | 1.048 | 1.768 | 1.116 | 1.212 | 1.071 | 1.220 | 1.207 | 1.257 | 1.150 |
| clang-14 -O3 | 0.958 | 0.864 | 0.992 | 1.658 | 1.130 | 1.230 | 1.051 | 1.209 | 1.232 | 1.226 | 1.396 |
| clang-14 -O2 | 0.957 | 0.864 | 0.992 | 1.679 | 1.129 | 1.230 | 1.050 | 1.206 | 1.232 | 1.225 | 1.395 |
| clang-14 -Os | 0.959 | 0.860 | 0.994 | 1.674 | 1.126 | 1.230 | 1.043 | 1.230 | 1.287 | 1.295 | 1.409 |
| clang-14 -O | 0.969 | 0.864 | 0.996 | 1.672 | 1.139 | 1.228 | 1.060 | 1.225 | 1.260 | 1.271 | 1.438 |
| clang-17 -O3 | 0.953 | 0.863 | 0.971 | 1.636 | 1.129 | 1.231 | 1.050 | 1.204 | 1.225 | 1.212 | 1.357 |
| clang-17 -O2 | 0.952 | 0.863 | 0.971 | 1.634 | 1.129 | 1.231 | 1.050 | 1.206 | 1.225 | 1.212 | 1.354 |
| clang-17 -Os | 0.958 | 0.855 | 0.975 | 1.622 | 1.120 | 1.225 | 1.045 | 1.225 | 1.274 | 1.262 | 1.370 |
| clang-17 -O | 0.965 | 0.861 | 0.976 | 1.629 | 1.134 | 1.230 | 1.042 | 1.229 | 1.252 | 1.270 | 1.395 |
| clang-18 -O3 | 0.953 | 0.863 | 0.973 | 1.632 | 1.132 | 1.226 | 1.045 | 1.206 | 1.227 | 1.209 | 1.405 |
| clang-18 -O2 | 0.951 | 0.862 | 0.971 | 1.647 | 1.127 | 1.232 | 1.049 | 1.204 | 1.220 | 1.207 | 1.406 |
| clang-18 -Os | 0.965 | 0.862 | 0.977 | 1.635 | 1.128 | 1.232 | 1.045 | 1.236 | 1.263 | 1.274 | 1.433 |
| clang-18 -O | 0.967 | 0.863 | 0.974 | 1.630 | 1.136 | 1.231 | 1.043 | 1.232 | 1.267 | 1.273 | 1.419 |
| **geomean** | **0.926** | **0.824** | **0.979** | **1.644** | **1.083** | **1.183** | **1.014** | **1.182** | **1.159** | **1.203** | **1.209** |

### Does `a64/ucode` win?

_ratio = other ÷ a64/ucode (median cycles). **>1 ⇒ a64/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.541 | 0.483 | 0.600 | 0.636 | 0.695 | 0.595 | 0.566 | 0.684 | 0.635 | 0.691 | 0.596 |
| gcc-11 -O2 | 0.559 | 0.491 | 0.591 | 0.647 | 0.702 | 0.601 | 0.585 | 0.720 | 0.677 | 0.728 | 0.667 |
| gcc-11 -Os | 0.523 | 0.459 | 0.621 | 0.602 | 0.667 | 0.573 | 0.710 | 0.677 | 0.620 | 0.688 | 0.618 |
| gcc-11 -O | 0.537 | 0.478 | 0.589 | 0.628 | 0.685 | 0.608 | 0.565 | 0.687 | 0.668 | 0.724 | 0.665 |
| gcc-12 -O3 | 0.546 | 0.486 | 0.598 | 0.642 | 0.704 | 0.597 | 0.581 | 0.689 | 0.636 | 0.692 | 0.608 |
| gcc-12 -O2 | 0.557 | 0.488 | 0.583 | 0.644 | 0.706 | 0.601 | 0.583 | 0.720 | 0.668 | 0.721 | 0.663 |
| gcc-12 -Os | 0.523 | 0.456 | 0.617 | 0.601 | 0.667 | 0.578 | 0.711 | 0.681 | 0.630 | 0.690 | 0.625 |
| gcc-12 -O | 0.538 | 0.478 | 0.598 | 0.631 | 0.683 | 0.605 | 0.564 | 0.688 | 0.666 | 0.719 | 0.663 |
| gcc-13 -O3 | 0.563 | 0.497 | 0.577 | 0.656 | 0.720 | 0.612 | 0.589 | 0.716 | 0.672 | 0.712 | 0.622 |
| gcc-13 -O2 | 0.572 | 0.500 | 0.580 | 0.660 | 0.724 | 0.616 | 0.597 | 0.735 | 0.685 | 0.727 | 0.654 |
| gcc-13 -Os | 0.536 | 0.467 | 0.601 | 0.615 | 0.683 | 0.591 | 0.728 | 0.697 | 0.639 | 0.710 | 0.616 |
| gcc-13 -O | 0.541 | 0.481 | 0.593 | 0.631 | 0.685 | 0.606 | 0.566 | 0.690 | 0.682 | 0.711 | 0.651 |
| clang-14 -O3 | 0.578 | 0.521 | 0.598 | 0.681 | 0.742 | 0.634 | 0.603 | 0.729 | 0.743 | 0.739 | 0.842 |
| clang-14 -O2 | 0.570 | 0.515 | 0.591 | 0.672 | 0.733 | 0.626 | 0.596 | 0.719 | 0.734 | 0.730 | 0.831 |
| clang-14 -Os | 0.573 | 0.513 | 0.593 | 0.672 | 0.735 | 0.623 | 0.597 | 0.734 | 0.768 | 0.773 | 0.842 |
| clang-14 -O | 0.579 | 0.517 | 0.596 | 0.681 | 0.734 | 0.634 | 0.598 | 0.732 | 0.753 | 0.760 | 0.860 |
| clang-17 -O3 | 0.582 | 0.528 | 0.594 | 0.690 | 0.753 | 0.642 | 0.611 | 0.736 | 0.749 | 0.741 | 0.830 |
| clang-17 -O2 | 0.583 | 0.528 | 0.594 | 0.691 | 0.753 | 0.642 | 0.612 | 0.738 | 0.749 | 0.741 | 0.828 |
| clang-17 -Os | 0.590 | 0.527 | 0.601 | 0.690 | 0.755 | 0.644 | 0.616 | 0.755 | 0.785 | 0.778 | 0.844 |
| clang-17 -O | 0.593 | 0.528 | 0.599 | 0.696 | 0.755 | 0.640 | 0.614 | 0.754 | 0.768 | 0.780 | 0.856 |
| clang-18 -O3 | 0.584 | 0.528 | 0.596 | 0.693 | 0.751 | 0.640 | 0.613 | 0.739 | 0.752 | 0.740 | 0.861 |
| clang-18 -O2 | 0.578 | 0.523 | 0.590 | 0.685 | 0.748 | 0.637 | 0.607 | 0.731 | 0.741 | 0.733 | 0.854 |
| clang-18 -Os | 0.590 | 0.527 | 0.598 | 0.690 | 0.754 | 0.639 | 0.612 | 0.756 | 0.773 | 0.779 | 0.876 |
| clang-18 -O | 0.593 | 0.530 | 0.598 | 0.697 | 0.755 | 0.640 | 0.613 | 0.756 | 0.777 | 0.781 | 0.871 |
| **geomean** | **0.563** | **0.501** | **0.596** | **0.659** | **0.720** | **0.617** | **0.608** | **0.719** | **0.705** | **0.732** | **0.735** |
