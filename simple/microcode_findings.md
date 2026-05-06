# Microcode patching: what we've learned

Distilled from implementing field arithmetic for ten elliptic curves (curve25519,
P-224, P-256, P-384, P-448, P-521, secp256k1 mont/dettman, Poly1305) on Intel
Goldmont (Celeron N3350) via `vmwrite`/`vmread` hook patches.

---

## 1. Triad anatomy

A patch is a sequence of **triads**. Each triad is exactly:

```
{ μop_slot0, μop_slot1, μop_slot2, sequencing_word }
```

- **3 μops** (in slots 0, 1, 2) — the actual work
- **1 sequencing word** (typically `NOP_SEQWORD` or `END_SEQWORD`) — controls
  flow between triads

The three μops within a triad execute with **sequential semantics**: slot 0
fires first, then slot 1, then slot 2. Within a triad, later slots can read
values written by earlier slots (intra-triad RAW). The sequencing word at the
end either advances to the next triad (`NOP_SEQWORD`) or returns control to
the host CPU (`END_SEQWORD`).

Patch RAM holds **at most 128 triads** at addresses U7c00..U7dfc (each triad
takes 4 address units; addresses must be even).

---

## 2. Why we need NOPs

NOPs appear in slots when the structure of the algorithm prevents us from
filling all 3 slots with useful work. The reasons, in roughly decreasing
frequency:

### 2a. **MUL placement constraints**
`MUL_DSZ64_DRR` is verified working in **slot 0 or slot 1** of a triad.
Slot 2 has not been tested in production code; we treat it as risky. A
triad whose only useful op is a MUL therefore wastes 1 slot at minimum (the
slot before MUL if MUL is in slot 1, or slots 1 and 2 if MUL is in slot 0
with no follow-on work).

```
{ MUL_DSZ64_DRR(RCX, RDI, RDX),
  NOP, NOP, NOP_SEQWORD }    // ← 2 NOPs because MUL is the only useful op
```

In contrast, a packed MUL triad uses slot 0 for accumulating the previous
MUL's result, slot 1 for this MUL, and slot 2 for accumulating a previous
SETCC carry — no NOPs:

```
{ ADD_DSZ64_DRR(R8, R8, RCX),     // slot 0: hi += previous hi
  MUL_DSZ64_DRR(RCX, srcA, RDX),  // slot 1: this MAC
  ADD_DSZ64_DRR(TMP9, TMP9, TMP15), // slot 2: carry sum += previous carry
  NOP_SEQWORD }
```

The `p521_sq` and (now) `curve25519_mul` patches use this fully-packed pattern.

### 2b. **MUL's srcB destruction**
`MUL_DSZ64_DRR(dst, srcA, srcB)` puts the high half in `dst` and the low half
in `srcB`. **srcA is preserved; srcB is overwritten.** This means:

- If srcB is a value we still need (like a `b[j]` lane), we have to copy it
  to RDX first via `ZEROEXT(RDX, TMP_b)` — a separate op.
- The post-MUL slot can save the lo (it's now in srcB / RDX) and hi (RCX) to
  TMPs — fine, but those saves take real op slots.

When the algorithm doesn't need both lo and hi immediately, post-MUL slots
become NOPs.

### 2c. **RAW dependency forcing**
Some operations require their input to be produced earlier with a specific
register flow. For example, `SETCC_CONDB_DR(TMP15, TMP0)` needs TMP0's
domain-#1 flag, which is set by the most recent ADD/SUB *to* TMP0. If you
need to insert other ops between the ADD-to-TMP0 and the SETCC, those ops
must not also do an ADD/SUB to TMP0 (or they'll overwrite the flag). When
the natural placement of operations conflicts with this rule, you may have
to leave a slot NOP rather than place a flag-disturbing op there.

### 2d. **Idle slots at limb boundaries**
The first triad of the first limb (no incoming carry to fold) and the last
triad of the last limb (no outgoing carry to prep) often have less work
than middle limbs. Example from `asm_op_curve25519.c` c0 T0:

```
{ ZEROEXT_DSZ64_DR(TMP0, RAX),     // init acc from carry-in
  MUL_DSZ64_DRR(RCX, RDI, RDI),    // first MAC
  NOP, NOP_SEQWORD }                // ← no link/prep needed in slot 2
```

### 2e. **Op-count mismatch with the 3-slot budget**
Sometimes the algorithm requires 4 or 5 dependent operations that would all
race for the same triad. You must split into two triads. The second triad
sometimes only has 2 useful ops, leaving 1 slot NOP. This is "structural"
NOP and unavoidable without restructuring the algorithm.

---

## 3. Triad dependencies (RAW rules)

Confirmed via `test_slot02_raw.c`, `test_slot_raw_extended.c`, and
day-to-day production-code experience:

| Dependency | Status | Notes |
|---|---|---|
| **Intra-triad slot 0 → slot 1** (value) | ✓ confirmed | Most common pattern |
| **Intra-triad slot 0 → slot 1** (flag) | ✓ confirmed | SETCC after ADD in adjacent slot reads new flag |
| **Intra-triad slot 0 → slot 2** (value) | ✓ confirmed | Important for tight packing |
| **Intra-triad slot 1 → slot 2** (value) | ✓ confirmed | Often used for SHL/SHR + SHR mask |
| **Intra-triad slot 0 + slot 2 WAW** | ✓ slot 2 wins | When two slots write the same dst, slot 2's value persists |
| **Cross-triad TMP value persistence** | ✓ confirmed | TMPs persist between triads within one vmwrite |
| **Cross-triad TMP flag persistence** | ✓ confirmed | A TMP's flag from triad K's ADD persists until next ADD/SUB to that TMP |
| **MUL `srcB` RAW slot 0 → 1** | ✓ confirmed | Lets us write `srcB` in slot 0 then read in slot 1's MUL |
| **TMP persistence across `vmwrite`** | ✗ does NOT persist | Critical — between calls, only arch regs survive |
| **Arch reg persistence across `vmwrite`** | ✓ persists | Used for inter-call state (e.g., 5 acc regs in P-384 Mont) |

In short: **within a triad, every slot can read everything earlier slots wrote.
Across triads (within the same vmwrite), TMPs and arch regs both persist.
Across vmwrites, only arch regs persist.**

---

## 4. The two flag domains

This is one of the most non-obvious aspects of patch design.

### Domain #1: Internal per-TMP flags
- Set by: ADD/SUB whose destination is a TMP register
- Read by: `SETCC_CONDB_DR(dst, src)` where `src` is the TMP whose flag we want
- Persists: until the next ADD/SUB to that same TMP overwrites it
- Each TMP has its own independent flag bit
- **Does not exist for arch registers.** ADD/SUB to RAX/R8/etc. doesn't set a
  flag readable by SETCC. We've burned several debugging sessions on this:
  the arithmetic result is correct, but SETCC silently returns 0.

### Domain #2: Architectural RFLAGS
- Set by: the host CPU's `vmwrite` instruction (frozen at patch entry)
- Read by: `READAFLAGS`, `ADC` (`add-with-carry` reads the CF bit), conditional
  branches, etc.
- **Frozen during the patch** — domain-#1 ADD/SUB doesn't update domain-#2
- Restored to caller's view at `END_SEQWORD`

### The bridge that almost works
There is a `GENARITHFLAGS_R` op that copies a TMP's domain-#1 flag into
domain-#2 RFLAGS, intended to enable ADC patterns. **In practice it
fails unreliably in large patches** — `tests/adc_findings.md` documents this
in detail. Production code does not use it.

---

## 5. ADC without TMP registers — the answer

**Short answer: no, we cannot reliably use ADC for carry tracking inside a
patch. Use ADD-to-TMP + SETCC_CONDB instead.**

Why ADC is broken in this environment:

1. **ADC reads RFLAGS.CF (domain #2).** RFLAGS is frozen at patch entry.
2. **ADD instructions inside the patch don't update domain #2** unless they
   write to an arch register *and* something explicit propagates the flag —
   neither of which happens by default.
3. **`GENARITHFLAGS_R` to push domain-#1 → domain-#2 is unreliable** in
   patches longer than a handful of triads. We get wrong carries
   non-deterministically.
4. ADC therefore reads a **stale or invalid CF** — sometimes 0, sometimes
   whatever the host CPU happened to have at vmwrite entry.

Why the ADD-to-TMP + SETCC pattern works:

1. **ADD-to-TMP sets domain #1's per-TMP flag** (reliable).
2. **`SETCC_CONDB_DR(TMP15, TMP_x)` reads that per-TMP flag and materializes
   it as a 0/1 value in TMP15** (reliable).
3. **Subsequent ops can ADD TMP15 into a carry sum, propagate it to the next
   limb, etc.** — pure data-flow, no hidden flag dependencies.

This means **every carry chain in production microcode goes through TMPs**.
You cannot accumulate a carry directly into an arch register and use ADC to
chain — the flag plumbing isn't there.

The relevant CLAUDE.md rule:

> SETCC_CONDB_DR only works on TMP registers, NOT arch registers

is a direct consequence of this two-domain structure.

### Practical implication for register pressure
Every carry-chain stage costs at least one TMP slot for the SETCC
destination, plus an ADD-target TMP whose flag can be observed. On
operations with deep carry chains (Montgomery reductions, multi-limb
schoolbook), this pressure is real — Montgomery P-384 runs the TMP file
near saturation, with `R8` repurposed as an arch-side hi accumulator
specifically because no TMPs are free to hold it.

---

## 6. Practical patterns we've settled on

### "Progressive accumulation" (the high-density pattern)
Used in `asm_op_p521_sq.c`, `asm_op_curve25519_mul.c` (the optimized
version), `asm_op_curve25519.c`. Each MAC triad does three ops:

```
slot 0:  ADD R8 += RCX        // hi from previous MAC into running hi sum
slot 1:  MUL_DSZ64_DRR         // this MAC
slot 2:  ADD TMP9 += TMP15    // SETCC carry from previous ACC into running carry sum
```

Pairs with an ACC triad:

```
slot 0:  ADD TMP_acc += RDX    // lo from this MAC into running accumulator
slot 1:  SETCC_CONDB(TMP15, TMP_acc)  // capture overflow
slot 2:  ZEROEXT(RDX, TMP_b_next)     // prep next MAC's srcB
```

Two triads per MAC after the first, fully packed, no NOPs in middle limbs.

### "Triple pack" for carry chain accumulation
Used in `asm_op_p384_sq.c`'s Phase A'. When you need to add a carry-in to
a running sum and capture the new overflow:

```
slot 0:  SETCC_CONDB(TMP_prev_carry, TMP_acc)   // capture carry from previous ADD
slot 1:  ADD TMP_acc, TMP_acc, TMP_cin           // add the chain carry
slot 2:  SETCC_CONDB(TMP_new_carry, TMP_acc)    // capture new carry
```

3 ops, 1 triad, handles a full carry-chain step.

### "Mask via SHL+SHR pair"
We don't have an `AND_DSZ64_DRI` with arbitrary 64-bit immediates, so to
mask to k bits we use:

```
SHL TMP_x, TMP_acc, (64-k)    // shift left to discard high (64-k) bits
SHR output_reg, TMP_x, (64-k) // shift right to land masked value at bit 0
```

Two ops, fits in adjacent slots of one triad via slot 0→1 RAW.

---

## 7. Aggregate findings (the structural rules)

From the cross-curve experience:

1. **Microcode wins iff the entire computation fits in one vmwrite.** Each
   `vmwrite` costs ~20–25 cycles fixed tax. Below the patch-RAM ceiling
   (~128 triads), this is paid once and amortized over all the work.
   Crossing the ceiling forces fragmentation, paying the tax per fragment,
   which compounds disastrously (P-521 mul: 18 fragments → 422 cyc vs
   fiat's 157).

2. **Squaring fits where multiplication doesn't,** because of the
   `a[i]·a[j] = a[j]·a[i]` symmetry that halves the MAC count. Same prime,
   same hardware, sq wins / mul loses for any curve where the MAC count
   straddles ~45.

3. **Operand storage is a separate ceiling.** Mul needs both `a[]` and
   `b[]` accessible, doubling TMP demand vs sq. With only 16 TMPs, large
   muls hit this wall before they hit the triad-count wall.

4. **NIST P-curves cluster at the boundary.** P-224 and P-256 sq fit
   comfortably; P-384 sq is on the line; P-384 mul and P-521 mul are over.
   This is why the "convert to unsaturated Solinas" trick worked for P-224
   (sq drops from 32 to ~10 effective MULs) and would help P-256/P-384.

5. **The carry-chain shape determines viability.** Algorithms with long
   linear carry chains (Montgomery reduction, multi-limb schoolbook) consume
   TMP register pressure and triad budget at high rates. Algorithms with
   independent carry chains (Solinas reduction column-wise) parallelize
   better and run lighter.

6. **Hashing-style primitives don't benefit.** Keccak, SHA-* — operations
   are 1-cycle XOR/AND/ROT with no expensive primitive. The vmwrite tax
   exceeds the savings. Microcode is for *expensive* operations
   (multiplication, iterative carries) that exist in *high count* per
   patch invocation.

---

## 8. Things we did *not* discover (open questions)

- **Whether MUL works in slot 2** — we never needed it badly enough to risk
  a debugging session on it. Treating slot 2 as MUL-prohibited has cost us
  ~2-3 triads per multiplication-heavy patch but stays safe.
- **The full SSE/AVX micro-op vocabulary** — Goldmont implements SSE
  internally with micro-ops, but we don't have a documented opcode list
  for them. All our patches are integer-domain.
- **Whether the patch RAM has loop/branch ops** — we've never used backward
  jumps. Production patches are straight-line. Loops might enable Keccak-style
  iterative algorithms but would be a significant research project to verify.
- **Why `GENARITHFLAGS_R` fails sporadically** — empirically reliable for
  small patches, unreliable for large ones, no clear failure mode. We
  routed around it rather than diagnose.
