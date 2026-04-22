# Microcode Field Arithmetic — Implementation Summary

## What This Project Does

We implement **field multiplication and squaring** for elliptic curve cryptography by writing custom microcode patches for an Intel Goldmont (Celeron N3350) processor. Instead of running compiled x86 instructions, the CPU executes our hand-written micro-operations directly from the patch RAM, bypassing the instruction decoder entirely.

The hooked instruction is `vmwrite` (opcode 0x0cd8). When the program executes `vmwrite`, the CPU intercepts it and runs our microcode patch instead. The patch reads inputs from architectural registers, performs the field arithmetic using micro-operations (MUL, ADD, SETCC, ZEROEXT, SHR), and writes results back to architectural registers. The calling C code then retrieves the results.

## What Is Micro-coded

For every curve, the **core arithmetic kernel** is in microcode: the multiply-and-reduce operation that dominates elliptic curve scalar multiplication. Specifically:

- **The schoolbook multiplication** (computing a[i] * b[j] for all limb pairs)
- **The carry propagation** (combining partial products into full-width words)
- **The field reduction** (reducing the double-width product back to a field element)

What is NOT in microcode:
- Conditional final subtraction (for Montgomery curves) — done in native C
- Carry wrap-around (for Solinas curves) — done in native C
- Point arithmetic (point addition, doubling) — not implemented
- The test harness and benchmarking — native C

## Curve Implementations

### Montgomery Curves (Saturated 4x64-bit)

These use **word-by-word Montgomery multiplication** with 4 iterations per field operation. Each iteration has 4 phases, all in microcode:

```
Phase A  — Schoolbook multiply: a[i] * b[0..3] -> 5 product words
Phase A' — Accumulate: add product words to running accumulator with carry chain
Phase B  — Reduction: compute m = acc[0] * mu, then m * p[0..3] -> reduction words
Phase C  — Apply reduction + shift: add reduction to accumulator, discard lowest word
```

The micro-coded part is a single macro `MONT_ITER` that implements one complete iteration. The patch runs 2 iterations per `vmwrite` call (2 vmwrites total for 4 iterations).

| Curve | mu | p structure | Phase B MULs | Triads/iter | Total patch |
|-------|-----|-------------|-------------|-------------|-------------|
| **P-256** | 1 (implicit) | p[2]=0, can skip 1 MUL | 3 (p[0], p[1], p[3]) | 36 | 76 triads |
| **secp256k1 mont** | 0xD838...3531 | p[1]=p[2]=p[3]=all-ones | 3 (mu, p[0], one m*R8 reused) | 42 | 88 triads |
| **P-224** | -1 (= R8) | p[0]=1, p[3]=0xFFFFFFFF | 4 (mu=R8, p[1], p[2]=R8, p[3]) | 41 | 86 triads |

**Key optimizations applied to all Montgomery curves:**
- Phase A' w0 merged into Phase A MUL triad (free slots 1-2 during MUL)
- Triple-pack SETCC pattern: `{SETCC(c_prev), ADD(+cin), SETCC(c_new)}` — 3 carry ops in 1 triad
- Phase A b3 MUL merge: compact 4 triads into 3 by filling carry-chain NOP slots
- Phase A'/B boundary merge: start Phase B setup in Phase A' last triad
- Phase B/C boundary merge: start Phase C word 0 inside Phase B MUL triad
- Phase C w1 start in Phase B last triad, enabling w1 triple-pack

**P-256 specific:** `mu=1` means `m = acc[0]` with no MUL needed. `p[2]=0` skips one reduction MUL. This gives the smallest Phase B (3 MULs, 5 triads).

**secp256k1 specific:** `mu != 1` requires an extra MUL. But `p[1]=p[2]=p[3]` means one `m*R8` MUL covers all three — the hi/lo products are reused for red[2], red[3], red[4]. Uses RBP for mu constant (push/pop in inline asm). Saves red[2] to RDI (arch reg free after Phase A) to avoid clobbering TMP9 (mu) between iterations.

**P-224 specific:** `mu = -1 = R8`, so mu MUL reuses the same constant as p[2]. `p[0]=1` means `m*p[0] = m` (no MUL). `p[3] = 0xFFFFFFFF = SHR(R8, 32)`, precomputed in TMP9 like P-256. Phase C w0 is acc[0] + m = acc[0] + (-acc[0]) = 0, but carry is needed.

### Unsaturated Solinas/Dettman Curves

These use a **single-pass multiply-and-reduce** approach. The field element is represented with limbs smaller than 64 bits, so products don't overflow 128-bit accumulators. No carry chains between multiply-accumulate (MAC) operations.

Each output limb is computed as a sum of MAC products:
```
c[k] = sum of a[i] * b[j] for all (i,j) where (i+j) mod N == k
       (with reduction constant applied to wrapped products where i+j >= N)
```

| Curve | Limbs | Bits/limb | Reduction | MULs | Patch triads |
|-------|-------|-----------|-----------|------|-------------|
| **curve25519** | 5 | 51-bit | *19 (Dettman) | 25 | 88 triads |
| **poly1305** | 3 | 44/43-bit | Solinas | 9 | ~39 triads |
| **secp256k1 Dettman** | 5 | 52/48-bit | Dettman | 25 | ~119 triads |
| **P-521** | 9 | 58-bit | *1 (Mersenne!) | 5/limb | 19 triads × 9 vmwrites |

**curve25519 Dettman (5x51-bit):** The reduction constant 19 is folded into the multiplication: products that wrap around (i+j >= 5) multiply by 19 instead. Pre-computes `19*b[1..4]` in PREP. Each of 5 output limbs accumulates 5 MACs. Carry extraction uses SHR/SHL for 51-bit masking. All 25 MACs + carry propagation in one 88-triad patch.

**poly1305 (3x44/43-bit):** Smallest curve. Only 9 MULs for 3x3 schoolbook. Reduction folds the upper limbs back using the Solinas constant. Fits in ~39 triads.

**secp256k1 Dettman (5x52/48-bit):** Similar to curve25519 Dettman but with a different reduction constant derived from the secp256k1 prime. Uses 5 limbs with alternating 52/48-bit widths (Dettman's optimization for this prime).

**P-521 (9x58-bit, Mersenne prime):** The prime 2^521 - 1 is Mersenne, so `2^521 ≡ 1 mod p`. The reduction constant is just 2 (since 9*58 = 522, and `2^522 = 2 * 2^521 ≡ 2 mod p`). Uses a **19-triad generic MAC patch** called 9 times via vmwrite (one per output limb). Each call computes 5 MAC products for one limb. For squaring, symmetry reduces unique products from 9 to 5 per limb. The inline asm precomputes doubled/quadrupled operands and rotates register assignments between vmwrites. Wrap-around carry handled in native C.

### Saturated Solinas (curve25519_solinas, 4x64-bit)

This was implemented but **does not beat GCC** on Goldmont. The representation uses full 64-bit saturated limbs, requiring native ADC-style carry chains. In microcode, each carry propagation costs 2 triads (ADD+SETCC, then ADD+cin+SETCC) versus 1 native ADC instruction. With ~33 carry chains in the squaring, this adds ~33 extra triads of overhead that microcode cannot eliminate.

**Lesson learned:** Microcode beats GCC only when carry chains are avoided — i.e., with unsaturated representations where limb products don't overflow the 128-bit accumulator.

## The MAC Template (used by all unsaturated curves)

The multiply-accumulate pattern is the workhorse of unsaturated arithmetic:

```
// Prepare b value in RDX
ZEROEXT(RDX, TMP_bj)              // or ADD(RDX, TMP_bj, TMP_bj) for doubled

// Multiply
MUL(RCX, a_reg, RDX)              // hi:lo = a[i] * b[j]

// Accumulate lo into running sum
ADD(TMP_lo, TMP_lo, RDX)          // lo_acc += lo_product
SETCC(TMP_carry, TMP_lo)          // capture carry

// Accumulate hi + carry
ADD(TMP_hi_part, RCX, TMP_carry)  // hi_product + carry_from_lo
ADD(R8_hi, R8_hi, TMP_hi_part)    // hi_acc += hi_product + carry
```

Each MAC takes ~3 triads. The hi accumulator (R8) collects the upper bits. After all MACs for one limb, a SHR extracts the carry for the next limb.

## The Triple-Pack Carry Pattern (used by all Montgomery curves)

The addcarryx (add-with-carry) operation costs 5 micro-ops: two ADDs and three SETCCs. The triple-pack compresses the carry-in addition into a single triad:

```
// Previous triad's slot 2 started: ADD(TMP0, acc_word, product_word)
// This triad does the carry-in from the previous word:
{ SETCC(TMP1, TMP0),                    // capture carry from previous ADD
  ADD(TMP0, TMP0, TMP3_carry_in),       // add carry-in from word below
  SETCC(TMP8, TMP0) }                   // capture new carry

// Next triad combines and starts the next word:
{ ADD(TMP3, TMP1, TMP8),                // combined carry = c1 + c2
  ZEROEXT(R_out, TMP0),                 // write result to output register
  ADD(TMP0, next_acc, next_product) }   // start next word in slot 2
```

This achieves one addcarryx per 2 triads (the triple-pack triad + the combine/start triad), compared to 3 triads without the optimization.

## Hardware Constraints

- **128 triads** of patch RAM (U7c00-U7dfc), minus 5 triads for staging area (U7de0-U7df0) = **120 usable triads**
- **MUL must be in slot 0** of a triad
- **3 micro-ops per triad** (slots 0, 1, 2) + seqword
- **SETCC only works on TMP registers** (not architectural registers)
- **TMP registers don't persist across vmwrite calls** — all inter-call state must be in architectural registers
- **Intra-triad RAW:** slot 0 → slot 1 confirmed (value + flags). Slot 1 → slot 2 confirmed for flags (SETCC). Slot 0 → slot 2 value unconfirmed.
- **2 hook slots** available (vmwrite = hook 0, vmread = hook 1)
- MUL_DSZ64_DRR(hi, srcA, srcB): srcA preserved, srcB overwritten with lo, hi gets hi product

## File Naming Convention

```
asm_op_{curve}_{operation}.c        → microcode implementation + native C + tests
asm_op_{curve}_{operation}_static   → compiled binary (run with sudo on core 0)
```

All files in `/home/redunlock/code/lib-micro/Sabrina/simple/`.
Reference C implementations in `/home/redunlock/code/lib-micro/Sabrina/curvesC/`.
