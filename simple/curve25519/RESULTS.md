# X25519 Microcode Benchmark Results

**Generated:** Wed 09 Sep 2026 00:34:07 ACST
**Host:** redunlock-GB-BPCE-3350C
**CPU:** Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
**Pinned freq:** 1094400 kHz   (governor: `userspace`, no_turbo: `1`)
**Delivered core freq:** 1100 MHz · **TSC (RDTSC) rate:** 1094 MHz · **correction f_core/f_TSC:** 1.00548 (aperf/mperf under load, verified before the sweep; comparative **ratios are invariant** to this factor, multiply **absolute** cycle counts by it for true core cycles)
**Post-sweep frequency check:** stable (+0.000% over the sweep) · delivered 1100 MHz / TSC 1094 MHz after the sweep (the pre-sweep guard proves the machine was pinned when the sweep started; this proves it stayed pinned throughout)
**Core isolation:** core 0; 28 IRQs steered to core 1 (2 per-CPU/unmovable); SCHED_FIFO 99 · nohz_full/isolcpus: off — periodic timer tick still hits core 0
**Runs per config:** 3 (recorded median is the median of those runs; worst run-to-run spread 2.254% at amd64-51/ucode-Clad @ clang-18 -O2)
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
| **microcode 5×51 (this work)** | **308,335** | — | — | clang-14 -O3 |
| fiat-crypto (verified C) | 355,527 | 1.153 | 1.194 | gcc-12 -O3 |
| hand-written C (`__uint128_t`) | 381,245 | 1.236 | 1.240 | clang-17 -O3 |
| CryptOpt (superoptimized asm) | 381,285 | 1.237 | 1.217 | clang-14 -O2 |
| amd64-51 asm (Bernstein–Schwabe) | 383,781 | 1.245 | 1.220 | clang-14 -O3 |

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
| `a51/asm` | qhasm, monolithic | qhasm asm | 356,726 | — | baseline |
| `a51/asmCld` | C, per-op calls | qhasm asm | 388,007 | 0.919 | ladder: qhasm → C (fusion lost) |
| `a51/ucCld` | C, per-op calls | **microcode** | 327,622 | **1.184** | **field ops: asm → microcode** |
| `a51/ucode` | inline-asm, chained | microcode | 316,590 | 1.035 | ladder: C → register-chained asm |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** 1.184× faster (geomean 1.169×), with the ladder **and** the framework held constant.

**Consistency check.** The steps are multiplicative, so they must compose to the
measured end-to-end ratio:

```
  0.919 (ladder) x 1.184 (field ops) x 1.035 (chaining)  =  1.12678
  measured  356726 / 316590                              =  1.12678
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
| `a64/asm` | qhasm, monolithic | qhasm asm | 271,655 | — | baseline |
| `a64/asmCld` | C, per-op calls | qhasm asm | 306,273 | 0.887 | ladder: qhasm → C (fusion lost) |
| `a64/ucode` | C, per-op calls | **microcode** | 513,941 | **0.596** | **field ops: asm → microcode** |

_`× vs row above` > 1 means that row is **faster** than the one above it._

**The field-op step is the paper's quantity:** microcode is 1.678× **slower** than the asm it replaces (geomean 1.679×), with the ladder held constant — against 1.892× if the ladder rewrite is wrongly charged to the field ops.

---

## Table 4 — End-to-end standing (orientation, not the claim)

**Held constant:** nothing — these are whole implementations differing in
representation, ladder, inversion and field ops at once. Useful for placing the
work against shipped code; useless for attributing the difference to microcode.
For that, see Table 1.

| implementation | cyc/X25519 | best config |
|---|---:|---|
| amd64-64 asm (Bernstein–Schwabe, 4×64) | 271,655 | gcc-12 -O3 |
| **microcode 5×51 + inline-asm ladder (this work)** | **300,266** | clang-14 -O3 |
| amd64-51 framework + microcode | 316,590 | clang-17 -O |
| donna c64 (portable C) | 337,834 | gcc-11 -O3 |
| fiat-crypto (verified C) | 355,527 | gcc-12 -O3 |
| amd64-51 asm (Bernstein–Schwabe, 5×51) | 356,726 | clang-18 -O3 |
| hand-written C (`__uint128_t`) | 381,245 | clang-17 -O3 |
| CryptOpt (superoptimized asm) | 381,285 | clang-14 -O2 |


### Dispersion at each contender's best config

_Median is the headline; min and the p10–p90 range show run-to-run spread at that config. A tight p90−p10 relative to the inter-contender gaps means the ranking is not noise._

| contender | median | min | p10 | p90 | p90−p10 | best config |
|---|---:|---:|---:|---:|---:|---|
| ucode | 300266 | 299801 | 299955 | 310914 | 10959 | clang-14 -O3 |
| a64/asm | 271655 | 271478 | 271563 | 279400 | 7837 | gcc-12 -O3 |
| a64/asmCld | 306273 | 306135 | — | — | — | clang-17 -O3 |
| a64/ucode | 513941 | 510535 | — | — | — | clang-18 -O |
| a51/asm | 356726 | 356541 | 356606 | 366997 | 10391 | clang-18 -O3 |
| a51/asmCld | 388007 | 387842 | 387909 | 399377 | 11468 | clang-17 -O3 |
| a51/ucCld | 327622 | 327370 | 327518 | 334438 | 6920 | clang-18 -O |
| a51/ucode | 316590 | 316156 | 316485 | 327770 | 11285 | clang-17 -O |
| cryptopt | 381285 | 380959 | 381204 | 391564 | 10360 | clang-14 -O2 |
| fiat | 355527 | 355332 | 355460 | 364416 | 8956 | gcc-12 -O3 |
| hand-C | 381245 | 380049 | 380979 | 391822 | 10843 | clang-17 -O3 |
| donna | 337834 | 337481 | 337637 | 347413 | 9776 | gcc-11 -O3 |
| s2n-bignum/asm | 243746 | 243557 | 243669 | 252749 | 9080 | gcc-13 -O |
| osslops/C-ladder | 263108 | 263035 | 263077 | 272285 | 9208 | clang-14 -O2 |
| openssl | 253885 | 253827 | 253855 | 262652 | 8797 | gcc-13 -O2 |

---

# Appendix A — full per-config sweep

The raw 24-config matrices behind the best-per-contender numbers above.
Present so the selection rule can be audited and so per-compiler behaviour is
visible; not intended to be read row by row.

### A.1 — X25519 end-to-end, every contender

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 305461 | 272392 | 336324 | 559687 | 358811 | 393355 | 334647 | 322380 | 387240 | 358585 | 390798 | **337834** | 243852 | 268063 | 254516 |
| gcc-11 -O2 | 310846 | 273069 | 326707 | 553258 | 358974 | 390414 | 332872 | 324216 | 399141 | 376111 | 404236 | 371164 | 243813 | 278433 | 253898 |
| gcc-11 -Os | 312820 | 273865 | 362184 | 596499 | 359059 | 398858 | 344240 | 424082 | 404459 | 371562 | 410562 | 374243 | 244099 | 281722 | 253896 |
| gcc-11 -O | 306939 | 272790 | 339441 | 568709 | 358192 | 391130 | 346299 | 320989 | 391916 | 378653 | 412305 | 386016 | 243795 | 273262 | 254191 |
| gcc-12 -O3 | 305862 | **271655** | 323769 | 553061 | 358779 | 393539 | 335614 | 324090 | 386049 | **355527** | 386562 | 341212 | 243767 | 266709 | 254014 |
| gcc-12 -O2 | 311448 | 271975 | 324193 | 552263 | 359204 | 391745 | 334224 | 325362 | 401135 | 372402 | 402220 | 371408 | 243909 | 277120 | 253941 |
| gcc-12 -Os | 312875 | 272364 | 363717 | 594999 | 358898 | 398443 | 340992 | 423787 | 406219 | 372493 | 411300 | 373322 | 244019 | 283847 | 253920 |
| gcc-12 -O | 307572 | 272464 | 340307 | 569352 | 358265 | 390063 | 344886 | 320959 | 393493 | 379948 | 408281 | 377843 | 243783 | 272950 | 254133 |
| gcc-13 -O3 | 308225 | 271723 | 314948 | 539505 | 358746 | 391777 | 333851 | 322054 | 390841 | 366789 | 389014 | 341595 | 243964 | 271269 | 254275 |
| gcc-13 -O2 | 311215 | 271943 | 314157 | 544641 | 359082 | 391875 | 333933 | 324155 | 399246 | 372155 | 395517 | 356425 | 243915 | 275643 | **253885** |
| gcc-13 -Os | 313046 | 272415 | 348569 | 581028 | 358790 | 398493 | 340992 | 423724 | 406229 | 373214 | 414164 | 357848 | 244017 | 284941 | 253981 |
| gcc-13 -O | 307976 | 273346 | 336331 | 566573 | 359326 | 389878 | 344408 | 321154 | 391889 | 388663 | 404370 | 373240 | **243746** | 271908 | 254178 |
| clang-14 -O3 | **300266** | 276106 | 312690 | 522987 | 356830 | 389245 | 331327 | 317454 | 381632 | 387903 | 388703 | 454669 | 250430 | 263163 | 254815 |
| clang-14 -O2 | 300331 | 273979 | 312804 | 521214 | 356865 | 389170 | 331125 | 316963 | **381285** | 390123 | 388227 | 454711 | 253076 | **263108** | 255421 |
| clang-14 -Os | 304264 | 273056 | 314911 | 523142 | 356788 | 390600 | 329924 | 317008 | 389037 | 411072 | 411836 | 447760 | 243801 | 267456 | 254157 |
| clang-14 -O | 305394 | 273372 | 314412 | 528152 | 358264 | 388164 | 334352 | 316930 | 387019 | 399263 | 402999 | 465837 | 252695 | 268416 | 255151 |
| clang-17 -O3 | 301109 | 275695 | **306273** | 516897 | 356728 | **388007** | 330649 | 317199 | 381863 | 387041 | **381245** | 442296 | 250737 | 263375 | 254513 |
| clang-17 -O2 | 301225 | 274097 | 306349 | 515872 | 356959 | 389178 | 329904 | 317739 | 381643 | 387042 | 381252 | 441133 | 253155 | 263678 | 255470 |
| clang-17 -Os | 305675 | 272502 | 310228 | 515860 | 356847 | 389511 | 329590 | 316768 | 390084 | 405557 | 401781 | 438358 | 243821 | 268724 | 254165 |
| clang-17 -O | 305486 | 271921 | 307877 | 515023 | 358733 | 388636 | 330329 | **316590** | 387248 | 396496 | 402432 | 451569 | 252714 | 268421 | 255451 |
| clang-18 -O3 | 301108 | 275558 | 306788 | 516888 | **356726** | 390513 | 330999 | 317368 | 381929 | 387000 | 381424 | 458479 | 250906 | 263232 | 254457 |
| clang-18 -O2 | 301137 | 274927 | 306837 | 514387 | 356915 | 389725 | 329873 | 317698 | 381464 | 388164 | 382651 | 459112 | 252999 | 263642 | 255478 |
| clang-18 -Os | 305404 | 272387 | 308451 | 515594 | 356802 | 389632 | 329568 | 316599 | 389988 | 399439 | 403403 | 454520 | 243881 | 268063 | 254133 |
| clang-18 -O | 305400 | 273390 | 306870 | **513941** | 358625 | 388667 | **327622** | 317336 | 389145 | 399770 | 402574 | 460562 | 253123 | 268344 | 255380 |

### A.2 — Same C ladder, only the field op differs

_median cycles. **bold** = best (lowest-median) config in that column._

| Config | uc/Clad | a51op/Clad | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 313284 | 388237 | 387240 | 358585 | 390798 |
| gcc-11 -O2 | 337303 | 395560 | 399141 | 376111 | 404236 |
| gcc-11 -Os | 341177 | 401415 | 404459 | 371562 | 410562 |
| gcc-11 -O | 331688 | 393388 | 391916 | 378653 | 412305 |
| gcc-12 -O3 | 314333 | 385253 | 386049 | **355527** | 386562 |
| gcc-12 -O2 | 337151 | 398178 | 401135 | 372402 | 402220 |
| gcc-12 -Os | 342625 | 403599 | 406219 | 372493 | 411300 |
| gcc-12 -O | 333415 | 394987 | 393493 | 379948 | 408281 |
| gcc-13 -O3 | 315048 | 393634 | 390841 | 366789 | 389014 |
| gcc-13 -O2 | 335482 | 396148 | 399246 | 372155 | 395517 |
| gcc-13 -Os | 341911 | 404906 | 406229 | 373214 | 414164 |
| gcc-13 -O | 330697 | 393726 | 391889 | 388663 | 404370 |
| clang-14 -O3 | **308335** | **383781** | 381632 | 387903 | 388703 |
| clang-14 -O2 | 308390 | 383927 | **381285** | 390123 | 388227 |
| clang-14 -Os | 314654 | 390929 | 389037 | 411072 | 411836 |
| clang-14 -O | 312767 | 389464 | 387019 | 399263 | 402999 |
| clang-17 -O3 | 309431 | 385772 | 381863 | 387041 | **381245** |
| clang-17 -O2 | 309333 | 386096 | 381643 | 387042 | 381252 |
| clang-17 -Os | 315896 | 390401 | 390084 | 405557 | 401781 |
| clang-17 -O | 312549 | 388050 | 387248 | 396496 | 402432 |
| clang-18 -O3 | 310359 | 385937 | 381929 | 387000 | 381424 |
| clang-18 -O2 | 310086 | 385821 | 381464 | 388164 | 382651 |
| clang-18 -Os | 315565 | 390322 | 389988 | 399439 | 403403 |
| clang-18 -O | 310593 | 389957 | 389145 | 399770 | 402574 |

### A.3 — Per-config ratios


### Does `uc/Clad` win?

_ratio = other ÷ uc/Clad (median cycles). **>1 ⇒ uc/Clad is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a51op/Clad | cryptopt | fiat | hand-C |
|---|---:|---:|---:|---:|
| gcc-11 -O3 | 1.239 | 1.236 | 1.145 | 1.247 |
| gcc-11 -O2 | 1.173 | 1.183 | 1.115 | 1.198 |
| gcc-11 -Os | 1.177 | 1.185 | 1.089 | 1.203 |
| gcc-11 -O | 1.186 | 1.182 | 1.142 | 1.243 |
| gcc-12 -O3 | 1.226 | 1.228 | 1.131 | 1.230 |
| gcc-12 -O2 | 1.181 | 1.190 | 1.105 | 1.193 |
| gcc-12 -Os | 1.178 | 1.186 | 1.087 | 1.200 |
| gcc-12 -O | 1.185 | 1.180 | 1.140 | 1.225 |
| gcc-13 -O3 | 1.249 | 1.241 | 1.164 | 1.235 |
| gcc-13 -O2 | 1.181 | 1.190 | 1.109 | 1.179 |
| gcc-13 -Os | 1.184 | 1.188 | 1.092 | 1.211 |
| gcc-13 -O | 1.191 | 1.185 | 1.175 | 1.223 |
| clang-14 -O3 | 1.245 | 1.238 | 1.258 | 1.261 |
| clang-14 -O2 | 1.245 | 1.236 | 1.265 | 1.259 |
| clang-14 -Os | 1.242 | 1.236 | 1.306 | 1.309 |
| clang-14 -O | 1.245 | 1.237 | 1.277 | 1.288 |
| clang-17 -O3 | 1.247 | 1.234 | 1.251 | 1.232 |
| clang-17 -O2 | 1.248 | 1.234 | 1.251 | 1.232 |
| clang-17 -Os | 1.236 | 1.235 | 1.284 | 1.272 |
| clang-17 -O | 1.242 | 1.239 | 1.269 | 1.288 |
| clang-18 -O3 | 1.244 | 1.231 | 1.247 | 1.229 |
| clang-18 -O2 | 1.244 | 1.230 | 1.252 | 1.234 |
| clang-18 -Os | 1.237 | 1.236 | 1.266 | 1.278 |
| clang-18 -O | 1.256 | 1.253 | 1.287 | 1.296 |
| **geomean** | **1.220** | **1.217** | **1.194** | **1.240** |

### Does `ucode` win?

_ratio = other ÷ ucode (median cycles). **>1 ⇒ ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | a64/asm | a51/asm | a51/ucode | donna | fiat | cryptopt | hand-C |
|---|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.892 | 1.175 | 1.055 | 1.106 | 1.174 | 1.268 | 1.279 |
| gcc-11 -O2 | 0.878 | 1.155 | 1.043 | 1.194 | 1.210 | 1.284 | 1.300 |
| gcc-11 -Os | 0.875 | 1.148 | 1.356 | 1.196 | 1.188 | 1.293 | 1.312 |
| gcc-11 -O | 0.889 | 1.167 | 1.046 | 1.258 | 1.234 | 1.277 | 1.343 |
| gcc-12 -O3 | 0.888 | 1.173 | 1.060 | 1.116 | 1.162 | 1.262 | 1.264 |
| gcc-12 -O2 | 0.873 | 1.153 | 1.045 | 1.193 | 1.196 | 1.288 | 1.291 |
| gcc-12 -Os | 0.871 | 1.147 | 1.354 | 1.193 | 1.191 | 1.298 | 1.315 |
| gcc-12 -O | 0.886 | 1.165 | 1.044 | 1.228 | 1.235 | 1.279 | 1.327 |
| gcc-13 -O3 | 0.882 | 1.164 | 1.045 | 1.108 | 1.190 | 1.268 | 1.262 |
| gcc-13 -O2 | 0.874 | 1.154 | 1.042 | 1.145 | 1.196 | 1.283 | 1.271 |
| gcc-13 -Os | 0.870 | 1.146 | 1.354 | 1.143 | 1.192 | 1.298 | 1.323 |
| gcc-13 -O | 0.888 | 1.167 | 1.043 | 1.212 | 1.262 | 1.272 | 1.313 |
| clang-14 -O3 | 0.920 | 1.188 | 1.057 | 1.514 | 1.292 | 1.271 | 1.295 |
| clang-14 -O2 | 0.912 | 1.188 | 1.055 | 1.514 | 1.299 | 1.270 | 1.293 |
| clang-14 -Os | 0.897 | 1.173 | 1.042 | 1.472 | 1.351 | 1.279 | 1.354 |
| clang-14 -O | 0.895 | 1.173 | 1.038 | 1.525 | 1.307 | 1.267 | 1.320 |
| clang-17 -O3 | 0.916 | 1.185 | 1.053 | 1.469 | 1.285 | 1.268 | 1.266 |
| clang-17 -O2 | 0.910 | 1.185 | 1.055 | 1.464 | 1.285 | 1.267 | 1.266 |
| clang-17 -Os | 0.891 | 1.167 | 1.036 | 1.434 | 1.327 | 1.276 | 1.314 |
| clang-17 -O | 0.890 | 1.174 | 1.036 | 1.478 | 1.298 | 1.268 | 1.317 |
| clang-18 -O3 | 0.915 | 1.185 | 1.054 | 1.523 | 1.285 | 1.268 | 1.267 |
| clang-18 -O2 | 0.913 | 1.185 | 1.055 | 1.525 | 1.289 | 1.267 | 1.271 |
| clang-18 -Os | 0.892 | 1.168 | 1.037 | 1.488 | 1.308 | 1.277 | 1.321 |
| clang-18 -O | 0.895 | 1.174 | 1.039 | 1.508 | 1.309 | 1.274 | 1.318 |
| **geomean** | **0.892** | **1.169** | **1.081** | **1.323** | **1.251** | **1.276** | **1.300** |

### A.4 — Uncontrolled ratios (superseded)

These compare a microcode hybrid against its asm baseline **without** holding the
ladder constant, so they attribute the ladder rewrite to the field ops. Retained
for auditability only — Tables 2 and 3 are the correct form of these comparisons.

### Does `a51/ucode` win?

_ratio = other ÷ a51/ucode (median cycles). **>1 ⇒ a51/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a64/ucode | a51/asm | a51/asmCld | a51/ucCld | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.948 | 0.845 | 1.043 | 1.736 | 1.113 | 1.220 | 1.038 | 1.201 | 1.112 | 1.212 | 1.048 | 0.756 | 0.832 | 0.789 |
| gcc-11 -O2 | 0.959 | 0.842 | 1.008 | 1.706 | 1.107 | 1.204 | 1.027 | 1.231 | 1.160 | 1.247 | 1.145 | 0.752 | 0.859 | 0.783 |
| gcc-11 -Os | 0.738 | 0.646 | 0.854 | 1.407 | 0.847 | 0.941 | 0.812 | 0.954 | 0.876 | 0.968 | 0.882 | 0.576 | 0.664 | 0.599 |
| gcc-11 -O | 0.956 | 0.850 | 1.057 | 1.772 | 1.116 | 1.219 | 1.079 | 1.221 | 1.180 | 1.284 | 1.203 | 0.760 | 0.851 | 0.792 |
| gcc-12 -O3 | 0.944 | 0.838 | 0.999 | 1.707 | 1.107 | 1.214 | 1.036 | 1.191 | 1.097 | 1.193 | 1.053 | 0.752 | 0.823 | 0.784 |
| gcc-12 -O2 | 0.957 | 0.836 | 0.996 | 1.697 | 1.104 | 1.204 | 1.027 | 1.233 | 1.145 | 1.236 | 1.142 | 0.750 | 0.852 | 0.780 |
| gcc-12 -Os | 0.738 | 0.643 | 0.858 | 1.404 | 0.847 | 0.940 | 0.805 | 0.959 | 0.879 | 0.971 | 0.881 | 0.576 | 0.670 | 0.599 |
| gcc-12 -O | 0.958 | 0.849 | 1.060 | 1.774 | 1.116 | 1.215 | 1.075 | 1.226 | 1.184 | 1.272 | 1.177 | 0.760 | 0.850 | 0.792 |
| gcc-13 -O3 | 0.957 | 0.844 | 0.978 | 1.675 | 1.114 | 1.216 | 1.037 | 1.214 | 1.139 | 1.208 | 1.061 | 0.758 | 0.842 | 0.790 |
| gcc-13 -O2 | 0.960 | 0.839 | 0.969 | 1.680 | 1.108 | 1.209 | 1.030 | 1.232 | 1.148 | 1.220 | 1.100 | 0.752 | 0.850 | 0.783 |
| gcc-13 -Os | 0.739 | 0.643 | 0.823 | 1.371 | 0.847 | 0.940 | 0.805 | 0.959 | 0.881 | 0.977 | 0.845 | 0.576 | 0.672 | 0.599 |
| gcc-13 -O | 0.959 | 0.851 | 1.047 | 1.764 | 1.119 | 1.214 | 1.072 | 1.220 | 1.210 | 1.259 | 1.162 | 0.759 | 0.847 | 0.791 |
| clang-14 -O3 | 0.946 | 0.870 | 0.985 | 1.647 | 1.124 | 1.226 | 1.044 | 1.202 | 1.222 | 1.224 | 1.432 | 0.789 | 0.829 | 0.803 |
| clang-14 -O2 | 0.948 | 0.864 | 0.987 | 1.644 | 1.126 | 1.228 | 1.045 | 1.203 | 1.231 | 1.225 | 1.435 | 0.798 | 0.830 | 0.806 |
| clang-14 -Os | 0.960 | 0.861 | 0.993 | 1.650 | 1.125 | 1.232 | 1.041 | 1.227 | 1.297 | 1.299 | 1.412 | 0.769 | 0.844 | 0.802 |
| clang-14 -O | 0.964 | 0.863 | 0.992 | 1.666 | 1.130 | 1.225 | 1.055 | 1.221 | 1.260 | 1.272 | 1.470 | 0.797 | 0.847 | 0.805 |
| clang-17 -O3 | 0.949 | 0.869 | 0.966 | 1.630 | 1.125 | 1.223 | 1.042 | 1.204 | 1.220 | 1.202 | 1.394 | 0.790 | 0.830 | 0.802 |
| clang-17 -O2 | 0.948 | 0.863 | 0.964 | 1.624 | 1.123 | 1.225 | 1.038 | 1.201 | 1.218 | 1.200 | 1.388 | 0.797 | 0.830 | 0.804 |
| clang-17 -Os | 0.965 | 0.860 | 0.979 | 1.629 | 1.127 | 1.230 | 1.040 | 1.231 | 1.280 | 1.268 | 1.384 | 0.770 | 0.848 | 0.802 |
| clang-17 -O | 0.965 | 0.859 | 0.972 | 1.627 | 1.133 | 1.228 | 1.043 | 1.223 | 1.252 | 1.271 | 1.426 | 0.798 | 0.848 | 0.807 |
| clang-18 -O3 | 0.949 | 0.868 | 0.967 | 1.629 | 1.124 | 1.230 | 1.043 | 1.203 | 1.219 | 1.202 | 1.445 | 0.791 | 0.829 | 0.802 |
| clang-18 -O2 | 0.948 | 0.865 | 0.966 | 1.619 | 1.123 | 1.227 | 1.038 | 1.201 | 1.222 | 1.204 | 1.445 | 0.796 | 0.830 | 0.804 |
| clang-18 -Os | 0.965 | 0.860 | 0.974 | 1.629 | 1.127 | 1.231 | 1.041 | 1.232 | 1.262 | 1.274 | 1.436 | 0.770 | 0.847 | 0.803 |
| clang-18 -O | 0.962 | 0.862 | 0.967 | 1.620 | 1.130 | 1.225 | 1.032 | 1.226 | 1.260 | 1.269 | 1.451 | 0.798 | 0.846 | 0.805 |
| **geomean** | **0.925** | **0.825** | **0.973** | **1.634** | **1.082** | **1.182** | **1.011** | **1.180** | **1.158** | **1.203** | **1.224** | **0.746** | **0.817** | **0.769** |

### Does `a64/ucode` win?

_ratio = other ÷ a64/ucode (median cycles). **>1 ⇒ a64/ucode is faster** (wins); <1 ⇒ slower. **bold** = geomean._

| Config | ucode | a64/asm | a64/asmCld | a51/asm | a51/asmCld | a51/ucCld | a51/ucode | cryptopt | fiat | hand-C | donna | s2n-bignum/asm | osslops/C-ladder | openssl |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| gcc-11 -O3 | 0.546 | 0.487 | 0.601 | 0.641 | 0.703 | 0.598 | 0.576 | 0.692 | 0.641 | 0.698 | 0.604 | 0.436 | 0.479 | 0.455 |
| gcc-11 -O2 | 0.562 | 0.494 | 0.591 | 0.649 | 0.706 | 0.602 | 0.586 | 0.721 | 0.680 | 0.731 | 0.671 | 0.441 | 0.503 | 0.459 |
| gcc-11 -Os | 0.524 | 0.459 | 0.607 | 0.602 | 0.669 | 0.577 | 0.711 | 0.678 | 0.623 | 0.688 | 0.627 | 0.409 | 0.472 | 0.426 |
| gcc-11 -O | 0.540 | 0.480 | 0.597 | 0.630 | 0.688 | 0.609 | 0.564 | 0.689 | 0.666 | 0.725 | 0.679 | 0.429 | 0.480 | 0.447 |
| gcc-12 -O3 | 0.553 | 0.491 | 0.585 | 0.649 | 0.712 | 0.607 | 0.586 | 0.698 | 0.643 | 0.699 | 0.617 | 0.441 | 0.482 | 0.459 |
| gcc-12 -O2 | 0.564 | 0.492 | 0.587 | 0.650 | 0.709 | 0.605 | 0.589 | 0.726 | 0.674 | 0.728 | 0.673 | 0.442 | 0.502 | 0.460 |
| gcc-12 -Os | 0.526 | 0.458 | 0.611 | 0.603 | 0.670 | 0.573 | 0.712 | 0.683 | 0.626 | 0.691 | 0.627 | 0.410 | 0.477 | 0.427 |
| gcc-12 -O | 0.540 | 0.479 | 0.598 | 0.629 | 0.685 | 0.606 | 0.564 | 0.691 | 0.667 | 0.717 | 0.664 | 0.428 | 0.479 | 0.446 |
| gcc-13 -O3 | 0.571 | 0.504 | 0.584 | 0.665 | 0.726 | 0.619 | 0.597 | 0.724 | 0.680 | 0.721 | 0.633 | 0.452 | 0.503 | 0.471 |
| gcc-13 -O2 | 0.571 | 0.499 | 0.577 | 0.659 | 0.720 | 0.613 | 0.595 | 0.733 | 0.683 | 0.726 | 0.654 | 0.448 | 0.506 | 0.466 |
| gcc-13 -Os | 0.539 | 0.469 | 0.600 | 0.618 | 0.686 | 0.587 | 0.729 | 0.699 | 0.642 | 0.713 | 0.616 | 0.420 | 0.490 | 0.437 |
| gcc-13 -O | 0.544 | 0.482 | 0.594 | 0.634 | 0.688 | 0.608 | 0.567 | 0.692 | 0.686 | 0.714 | 0.659 | 0.430 | 0.480 | 0.449 |
| clang-14 -O3 | 0.574 | 0.528 | 0.598 | 0.682 | 0.744 | 0.634 | 0.607 | 0.730 | 0.742 | 0.743 | 0.869 | 0.479 | 0.503 | 0.487 |
| clang-14 -O2 | 0.576 | 0.526 | 0.600 | 0.685 | 0.747 | 0.635 | 0.608 | 0.732 | 0.748 | 0.745 | 0.872 | 0.486 | 0.505 | 0.490 |
| clang-14 -Os | 0.582 | 0.522 | 0.602 | 0.682 | 0.747 | 0.631 | 0.606 | 0.744 | 0.786 | 0.787 | 0.856 | 0.466 | 0.511 | 0.486 |
| clang-14 -O | 0.578 | 0.518 | 0.595 | 0.678 | 0.735 | 0.633 | 0.600 | 0.733 | 0.756 | 0.763 | 0.882 | 0.478 | 0.508 | 0.483 |
| clang-17 -O3 | 0.583 | 0.533 | 0.593 | 0.690 | 0.751 | 0.640 | 0.614 | 0.739 | 0.749 | 0.738 | 0.856 | 0.485 | 0.510 | 0.492 |
| clang-17 -O2 | 0.584 | 0.531 | 0.594 | 0.692 | 0.754 | 0.640 | 0.616 | 0.740 | 0.750 | 0.739 | 0.855 | 0.491 | 0.511 | 0.495 |
| clang-17 -Os | 0.593 | 0.528 | 0.601 | 0.692 | 0.755 | 0.639 | 0.614 | 0.756 | 0.786 | 0.779 | 0.850 | 0.473 | 0.521 | 0.493 |
| clang-17 -O | 0.593 | 0.528 | 0.598 | 0.697 | 0.755 | 0.641 | 0.615 | 0.752 | 0.770 | 0.781 | 0.877 | 0.491 | 0.521 | 0.496 |
| clang-18 -O3 | 0.583 | 0.533 | 0.594 | 0.690 | 0.756 | 0.640 | 0.614 | 0.739 | 0.749 | 0.738 | 0.887 | 0.485 | 0.509 | 0.492 |
| clang-18 -O2 | 0.585 | 0.534 | 0.597 | 0.694 | 0.758 | 0.641 | 0.618 | 0.742 | 0.755 | 0.744 | 0.893 | 0.492 | 0.513 | 0.497 |
| clang-18 -Os | 0.592 | 0.528 | 0.598 | 0.692 | 0.756 | 0.639 | 0.614 | 0.756 | 0.775 | 0.782 | 0.882 | 0.473 | 0.520 | 0.493 |
| clang-18 -O | 0.594 | 0.532 | 0.597 | 0.698 | 0.756 | 0.637 | 0.617 | 0.757 | 0.778 | 0.783 | 0.896 | 0.493 | 0.522 | 0.497 |
| **geomean** | **0.566** | **0.505** | **0.596** | **0.662** | **0.723** | **0.619** | **0.612** | **0.722** | **0.708** | **0.736** | **0.749** | **0.456** | **0.500** | **0.470** |
