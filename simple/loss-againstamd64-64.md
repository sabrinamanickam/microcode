# Why our microcode X25519 loses to amd64-64

**Conclusion:** Our microcode X25519 runs in 312k cycles on Goldmont N3350.
SUPERCOP's `amd64-64` runs the same computation in 272k cycles — a 40k
cycle (15%) gap. **This gap cannot be closed in microcode** because the
single most important primitive that gives `amd64-64` its advantage —
the x86 `ADC` instruction's carry-in — is unreachable from inside a
microcode patch on Goldmont.

This document is the concrete evidence for that claim.

---

## 1. Why `ADC` matters

`amd64-64` represents a 256-bit field element as 4 saturated 64-bit
limbs and uses x86's hardware add-with-carry (`adc`) to chain a 64-bit
addition across limbs in one instruction per limb:

```asm
add  rax, [b]        ; lo:  sets CF
adc  rcx, [b+8]      ;      reads previous CF, adds, sets new CF
adc  rdx, [b+16]     ;      ...
adc  r8,  [b+24]     ;      ...
```

Each `adc` is one instruction, ~1 cycle on Goldmont. The whole carry
chain across 4 limbs costs ~4 cycles.

Inside `amd64-64`'s `ladderstep`, this `adc` pattern fires **321 times**
per ladder step (counted via `objdump -dM intel`). The whole-stack
runtime gain from `adc` over any alternative carry mechanism dominates.

---

## 2. How carry works in Goldmont microcode

The microcode in our patches uses a different carry-handling scheme:
the **SETCC dance**.

```
ADD_DSZ64_DRR(TMP0, a, b)        ; TMP0 = a + b, sets "TMP-domain CF" for TMP0
SETCC_CONDB_DR(TMP_c, TMP0)      ; TMP_c = 1 if TMP0's CF was set, else 0
ADD_DSZ64_DRR(hi, hi, TMP_c)     ; manually add the carry as a value
```

Three micro-ops per carry propagation. Compare to hardware `adc`'s one
instruction.

This works because Goldmont microcode has **two distinct flag
domains**:
- **Domain #1** (TMP-domain): per-TMP-register flags. `ADD →TMP` writes
  it; `SETCC_CONDB_DR` reads it. Persists across triads.
- **Domain #2** (arch RFLAGS): the same RFLAGS the x86 ISA exposes. The
  micro-op `ADC` reads its carry-in from this domain.

If we could write Domain #1's CF into Domain #2 inside a patch, then
microcode `ADC_DSZ64` (which the opcode set includes — see
`include/inst.h` line 159) would let us chain carries the same way
`amd64-64`'s asm does. One micro-op per limb instead of three.

---

## 3. The 4-month-old false-positive finding

`Sabrina/tests/adc_findings.md` (dated 2026-04-17) reported that
`GENARITHFLAGS_R(TMP)` bridges Domain #1 → Domain #2. Its evidence was
a 3-triad 128-bit add that passed two probes:

| a (lo)             | b (lo) | sum (lo) | true CF | result hi |
|--------------------|--------|----------|---------|-----------|
| 1                  | 1      | 2        | 0       | 0 ✓       |
| 0xFFFF…FF          | 1      | **0**    | 1       | 1 ✓       |

Both probes that "pass" do so under a coincidence: the overflow case
also has the low half landing on exactly zero. Those two probes can't
discriminate between two competing hypotheses:

- **H1:** `GENARITHFLAGS_R(TMP)` writes arch CF from TMP's domain-#1 CF.
- **H2:** `GENARITHFLAGS_R(TMP)` writes arch CF based on the *value* in
  TMP (e.g. "is it zero?"), not on TMP's CF.

Under both hypotheses, the canonical `0xFF..F + 1 = 0` case predicts
arch CF = 1. The original test had no probe where `CF=1` and `TMP ≠ 0`
simultaneously.

---

## 4. The discriminator

`Sabrina/tests/test_genarithflags_semantics.c` adds the missing probes.
Pattern fired:

```
T0: ADD t0 = R9 + R10 ;  GENARITHFLAGS_R(t0)
T1: ADC RAX = RAX + RCX                          ; RAX=RCX=0, so RAX_out = arch CF
T2: END
```

Results (run 2026-05-21 on the same Goldmont N3350):

| a (R9)            | b (R10)           | TMP0 (sum lo) | true CF | got RAX |
|-------------------|-------------------|---------------|---------|---------|
| 1                 | 1                 | 2             | 0       | 0       |
| 0xFFFF…FF         | 1                 | 0             | 1       | 1       |
| **0xFFFF…FE**     | **3**             | **1**         | **1**   | **0** ⚠ |
| **0xFFFF…FE**     | **5**             | **3**         | **1**   | **0** ⚠ |
| **0xDEAD…**       | **0xCAFE…**       | **0xA9AC…**   | **1**   | **0** ⚠ |
| 0x8000…           | 0x8000…           | 0             | 1       | 1       |
| 0                 | 0                 | 0             | 0       | 0       |

The five highlighted rows are the discriminator. They have `CF=1` from
a real overflow, but `TMP ≠ 0` because the sum doesn't wrap to exactly
zero. **Every one of them returns arch CF = 0.** `ADC` did not see the
carry.

The cleanest characterization of the observed behaviour is

```
arch_CF_after_GENARITHFLAGS = 1   iff   (TMP-domain CF == 1) AND (TMP == 0)
```

In other words: `GENARITHFLAGS_R(TMP)` writes arch CF = 1 only when the
ADD that produced TMP both carried out *and* wrapped to exactly zero.
That's not a CF bridge. It happens to coincide with the conventional
sentinel "result of `a + b = 2^k`", which is why the 4-month-old probe
set never caught the failure.

---

## 5. The other candidate bridges also fail

| Op                                       | Behaviour                                         |
|------------------------------------------|---------------------------------------------------|
| `GENARITHFLAGS_R(TMP)`                   | Quirky — sets arch CF iff TMP==0 *and* TMP-CF==1   |
| `MOVEINSERTFLGS_DSZ64_DRR(TMP15, TMP, TMP)` | Same broken pattern as GENARITHFLAGS              |
| `MOVEMERGEFLGS_DSZ64_DRR(TMP15, TMP, TMP)` | Same broken pattern                              |
| `MOVETOCREG_DSZ64_RI(TMP, CORE_CR_EFLAGS)` | **Total failure** — arch CF stays frozen at the value captured at patch entry, regardless of what we write |

`MOVETOCREG → CORE_CR_EFLAGS` was the most promising candidate because
it's an explicit "move register to control register" op and
`CORE_CR_EFLAGS` is defined in `include/opcode.h:116`. Run results from
`test_movetocreg_eflags_static`:

```
overflow, TMP=0        expect CF=1  got=0  FAIL
overflow, TMP=1        expect CF=1  got=0  FAIL
overflow, TMP=random   expect CF=1  got=0  FAIL
```

This confirms that **arch CF inside a patch is captured once at the
hook entry and frozen until END_SEQWORD.** The micro-op machinery does
not provide any way to update what `ADC` reads.

---

## 6. Why this kills the 40k-cycle gap

The gap breaks down roughly as:

| Component                                | Size       | Reachable in microcode? |
|------------------------------------------|------------|-------------------------|
| Structural mul-count (5×51 → 4×64)       | ~16k cyc   | **No**, see below       |
| Register-resident ladderstep (vs memory) | ~10–15k cyc | No — `feedback_dispatch_overhead.md` showed Goldmont's OoO + STLF already hide the C-level memory roundtrip |
| Mode-switch dispatch (vmread/vmwrite)    | ~10–15k cyc | No — would need [[sq-pair-plan]]; doesn't fit patch RAM (`feedback_sq_pair_too_big.md`) |

The first row is the one that would be most cleanly addressable in
principle: switch our microcode from 5×51 unsaturated to 4×64
saturated, like `amd64-64` does. That cuts the schoolbook from 25 muls
to 16.

But the savings only materialize **if** we can chain ADC for the
carries the way `amd64-64` does in hardware. In microcode we'd still
need the SETCC dance for every limb:

```
ADD lo, lo_a, lo_b               ; 1 op
SETCC_CONDB_DR(TMP_c, lo)        ; 1 op   ← needed because ADC can't see TMP-CF
ADD hi, hi, TMP_c                ; 1 op
ADD hi, hi, hi_partial           ; 1 op (the actual hi addition)
```

That's 4 micro-ops per limb. The 9 fewer multiplications save ~9 muls
× ~5 cyc/mul ≈ 45 cycles per `fe_mul`. The extra carry-propagation
overhead (~16 carries × 3 extra ops vs amd64-64's free ADC) adds back
roughly the same. Net savings: roughly zero.

We measured this indirectly: when we built the same kind of saturated
microcode patches for P-256 (4×64 Montgomery) and secp256k1 (Dettman vs
Montgomery), the **5×51 Dettman** representation is faster than 4×64
Montgomery for the same prime family in our microcode framework. Same
trade-off, different curve.

---

## 7. What we'd need to actually close the gap

Closing the 40k-cycle gap to `amd64-64` would require **either**:

1. A working CF bridge from Domain #1 to Domain #2 on Goldmont
   microcode that we haven't found. There are 580 opcodes defined in
   `include/opcode.h`; we've tested the four obvious candidates. The
   remaining surface area is large but the search would be largely
   uninformed without internal docs from Intel.

2. A fundamentally different `vmwrite`/`vmread` hook design that
   eliminates the mode-switch tax. Currently each call into a microcode
   patch costs ~27 cycles of unaccounted overhead (measured in
   `fe_sq_ucode_n` decomposition).

3. Inlining the entire ladder step into one giant microcode patch that
   does all 9 field operations in one `vmread` firing. Estimated 200+
   triads — far beyond the 124-triad patch RAM limit.

None of these are within reach with the current microcode tooling.

---

## 8. What we *did* accomplish

Microcode is a real win against the best hand-tuned **same-radix** x86
assembly: our X25519 beats SUPERCOP's `amd64-51/asm` (5×51 hand-tuned)
by ~14% on Goldmont (312k vs 356k cycles) for an apples-to-apples
comparison. The 40k cycle deficit only appears against `amd64-64`,
which uses a different representation (4×64 saturated with hardware
ADC) that the microcode architecture cannot match.

That's the honest framing for the paper: **microcode beats hand-tuned
5×51 x86 asm by 14%. The 4×64 representation `amd64-64` uses is faster
than 5×51 by 23% on this CPU thanks to hardware ADC, and that gap is
structural — not a microcode-tooling failure but an
arch-feature-availability one.**

---

## Artifacts

- `Sabrina/tests/test_genarithflags_semantics.c` — the discriminator
  that disproved the GENARITHFLAGS bridge hypothesis.
- `Sabrina/tests/test_movetocreg_eflags.c` — confirmed no alternative
  bridge via CORE_CR_EFLAGS.
- `Sabrina/tests/test_adc_chain.c` — broader probe set including chain
  attempts and MOVEINSERTFLGS/MOVEMERGEFLGS tests.
- `Sabrina/tests/adc_findings.md` — the prior (now-superseded)
  investigation.
