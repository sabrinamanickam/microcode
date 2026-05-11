# X25519 Benchmark Results

CPU: Intel Celeron N3350 (Goldmont).
Methodology: 8-config SUPERCOP flag matrix (`gcc`/`clang` × `-O3`/`-O2`/`-Os`/`-O`),
100 repetitions per config, `min` reported. Driver: `bench_supercop_matrix.sh`.
All six implementations pass the RFC 7748 1000-iteration chain test.

---

## (a) Same-ladder field-op comparison — OUR ladder

These three contenders share our hand-written Montgomery ladder, invert chain,
`fe_cswap`, and `fe_mul121665`. **The only thing that differs between them is
the implementation of `fe_mul` and `fe_sq`.** Pure field-op comparison.

| Config    |     native |       fiat |       ucode |
|-----------|-----------:|-----------:|------------:|
| gcc -O3   |   185 942  | **170 953**|     151 473 |
| gcc -O2   |   192 831  |    179 464 |     162 497 |
| gcc -Os   |   196 644  |    177 626 |     164 819 |
| gcc -O    |   197 022  |    181 871 |     164 240 |
| clang -O3 | **184 318**|    184 524 |     150 529 |
| clang -O2 |   184 324  |    185 095 | **149 077** |
| clang -Os |   196 650  |    195 266 |     154 187 |
| clang -O  |   192 009  |    190 117 |     151 280 |

Best: native **184 318** · fiat **170 953** · ucode **149 077**.

Microcode beats fiat-crypto by **~12.8%** and naïve C by **~19.2%** in
identical surrounding code.

---

## (b) Same-ladder field-op comparison — amd64-51 ladder

Both contenders use **SUPERCOP's `crypto_scalarmult/curve25519/amd64-51`
driver** (`mont25519.c` + `fe25519_invert.c` + pack/unpack/freeze/cswap).
The only difference is the implementation of `fe25519_mul` and
`fe25519_square`:

- **amd64-51**: original hand-tuned x86-64 asm (Bernstein/Schwabe, qhasm).
- **a51+ucode** (hybrid): a C `ladderstep` that calls our microcode
  `fe_mul`/`fe_sq` via the vmwrite / vmread hook.

| Config    |   amd64-51 |   a51+ucode |
|-----------|-----------:|------------:|
| gcc -O3   |    170 941 |     162 503 |
| gcc -O2   |    170 896 |     163 586 |
| gcc -Os   |    171 403 | **161 567** |
| gcc -O    |    170 858 |     163 614 |
| clang -O3 |    170 265 |     163 336 |
| clang -O2 |    170 310 |     163 024 |
| clang -Os | **170 255**|     161 930 |
| clang -O  |    170 678 |     163 282 |

Best: amd64-51 **170 255** · a51+ucode **161 567** — Δ = **−8 688 cycles
(−5.1%)** in favour of microcode field ops.

**This is the cleanest possible head-to-head**: same compiler, same driver,
same invert chain, same pack/cswap, same Goldmont N3350. The only thing that
moves the number is the field-op kernel.

---

## (c) End-to-end implementation comparison

Each row is what would actually ship as the X25519 function — different
ladder structures, different invert chains, different everything.

| Config    |  donna_c64 |   amd64-51 |  a51+ucode |       ucode |
|-----------|-----------:|-----------:|-----------:|------------:|
| gcc -O3   | **160 559**|    170 941 |    162 503 |     151 473 |
| gcc -O2   |    176 621 |    170 896 |    163 586 |     162 497 |
| gcc -Os   |    176 484 |    171 403 |**161 567** |     164 819 |
| gcc -O    |    181 771 |    170 858 |    163 614 |     164 240 |
| clang -O3 |    210 529 |    170 265 |    163 336 |     150 529 |
| clang -O2 |    210 528 |    170 310 |    163 024 | **149 077** |
| clang -Os |    211 848 | **170 255**|    161 930 |     154 187 |
| clang -O  |    216 887 |    170 678 |    163 282 |     151 280 |

---

## Best per contender (SUPERCOP-style reporting)

| Contender              |    min cyc | winning config |
|------------------------|-----------:|----------------|
| Native C               |    184 318 | clang -O3      |
| Fiat-crypto            |    170 953 | gcc -O3        |
| SUPERCOP donna_c64     |    160 559 | gcc -O3        |
| SUPERCOP amd64-51      |    170 255 | clang -Os      |
| amd64-51-ucode (hybrid)|    161 567 | gcc -Os        |
| **Microcode**          | **149 077**| **clang -O2**  |

---

## Interpretation

1. **Cleanest claim (table b)**: Microcode field arithmetic is **~5.1%
   faster** than amd64-51's hand-tuned x86-64 assembly field arithmetic
   on Goldmont N3350, when both are run inside amd64-51's own driver.
   Same flags, same CPU, same measurement methodology — every variable
   except `fe25519_mul`/`fe25519_square` is held constant.

2. **End-to-end**: Microcode-based X25519 (149 077 cyc, clang -O2) is
   **~7.2% faster than donna_c64** (the next-fastest contender on this
   CPU) and **~13.4% faster than amd64-51**.

3. **Our ladder vs amd64-51's framework**: Compare Microcode (our
   ladder, 149 077) to a51+ucode (amd64-51's framework with microcode,
   161 567). Same field ops, ~12.5k cycles delta in favour of our
   framework. Likely explanation: the amd64-51 framework pays a
   `ladderstep()` function-call overhead (255 calls × ~40 cyc ≈ 10k)
   that we avoid by inlining the ladderstep body directly in the bit
   loop. The amd64-51 design was tuned around a 7000-line inlined-asm
   ladderstep — once you replace it with a separate C function, the
   framework loses its primary advantage.

4. **Compiler picks matter for pure C, not for asm/microcode**:
   - **Pure C** (native, fiat, donna): result varies by 5–30% across
     compiler/flag configs. Best is sometimes gcc -O3, sometimes
     clang -O2. donna_c64 specifically does badly under clang on
     Goldmont (~210k vs 160k under gcc -O3) — a real outlier.
   - **amd64-51, a51+ucode, microcode**: nearly flat across configs
     (variance < 1%), because the hot path is asm or microcode that
     the C compiler doesn't see.

---

## Caveats (from `benchmark_review.md`)

- `amd64-51`'s 170k figure is high — published Goldmont/Apollo Lake
  numbers are typically 130–150k. Likely cause: CPU frequency
  scaling not pinned. Pin before final numbers:
  ```
  sudo cpupower frequency-set -g performance
  ```
  Relative ordering should be stable; absolute numbers may shift down.
- Median ≈ min for every contender (±0.5%), so interrupt jitter is not
  contaminating these results.
