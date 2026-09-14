# Plan: get microcode fe_mul and fe_sq below OpenSSL's fe51 assembly

Handoff document. Self-contained: a fresh session should be able to execute this
without reading the conversation that produced it. Written 2026-09-09.

---

## 1. Objective and exact targets

OpenSSL's hand-written base-2^51 assembly (`crypto/ec/asm/x25519-x86_64.pl`,
`x25519_fe51_mul` / `x25519_fe51_sqr`) is the fastest field arithmetic on this
core and currently beats us on both operations. Measured in `bench_kernel`,
dependent memory-to-memory chain, median cycles per operation:

| backend | fe_mul | fe_sq |
|---|---|---|
| microcode (current) | 122.9 | 81.8 |
| **OpenSSL fe51 asm** | **97.1** | **73.2** |
| s2n-bignum `_alt` (4x64) | 110.4 | 105.9 |
| fiat-crypto | 132.2 | 117.5 |
| CryptOpt | 141.4 | 121.0 |
| amd64-51 asm | 145.9 | 118.8 |
| hand-written `__uint128_t` C | 144.2 | 114.8 |

Cost model (section 2, corrected by probe_issue):

```
cycles = invocation_floor + n_mul * 0.91 + n_other * r
```

with `r` between 0.153 (fully independent ops) and 0.48 (what the current serial
schedule achieves). Budgets to beat OpenSSL, at plausible values of `r`:

| | now | n_mul | n_other | r=0.48 | r=0.30 | r=0.20 |
|---|---|---|---|---|---|---|
| fe_mul (target < 97.1) | 122.9 | 30 | 120 | 101 | **79** | **67** |
| fe_sq (target < 73.2) | 81.8 | 16 | 85 | **71** | **56** | **49** |

fe_sq clears OpenSSL even at today's `r`, purely on op-count reduction. fe_mul
does not: **it needs the dependency work, not just fewer operations.** That is
the single most important line in this document.

---

## 2. Measured hardware facts

Everything here was measured on this machine in this project. Do not re-derive.

### Cost model - CORRECTED 2026-09-09 by probe_issue

An earlier version of this plan asserted that patch bodies cost a flat
1.61 cyc/triad and were issue bound. **That was wrong.** It came from fitting
only two patches whose dependency structure is nearly identical, which cannot
separate issue rate from dependency stalls. `probe_issue` (triad-count sweep,
mutually independent ops, slope taken over the 20->40 span so the floor cancels)
gives the real independent rates:

| arm | cyc/triad | cyc/op | vs ALU |
|---|---|---|---|
| alu3 (3 ALU, no MUL) | 0.46 | 0.153 | 1x |
| setcc3 (3 SETCC) | 0.685 | 0.228 | 1.5x |
| mix (fe_mul's op ratio) | 0.58 | 0.193 | 1.3x |
| **mul3 (3 MUL)** | **2.74** | **0.913** | **6.0x** |

**MUL throughput is 0.91 cycles, not the 2.5 recorded in project memory.**
Correct that note.

**The production patches run at 1.62 cyc/triad while a synthetic patch with the
same op mix runs at 0.58. That 2.80x gap is dependency stalls.** The patches are
dependency bound, not issue bound.

```
fe_mul body            107.1 cyc          fe_sq body             67.6 cyc
  30 MUL @ 0.91         27.4                16 MUL @ 0.91        14.6
  166 non-MUL ops       79.7  (0.480/op)    107 non-MUL ops      53.0  (0.495/op)
  same, independent     25.5  (0.153/op)    same, independent    16.4  (0.153/op)
```

**Use this cost model instead:**
```
cycles = floor + n_mul * 0.91 + n_other * r      r in [0.153 .. 0.48]
```
`r` is set by how much parallelism the schedule exposes. 0.48 is what the
current serial single-accumulator design achieves; 0.153 is the fully
independent limit. Driving `r` down is the primary lever; reducing `n_other` is
the secondary one.

CAVEAT: alu3's 0.153 cyc/op is 6.5 ops/cycle, which exceeds 3-wide retirement,
so some cross-firing overlap survives the serialisation in probe_issue and
0.153 is a **lower bound** - the true headroom is smaller. Treat r = 0.25-0.30
as an optimistic-but-defensible target rather than 0.153.
- **Invocation floor is a THROUGHPUT quantity, not a latency.** `floor_mul_lat`
  and `floor_mul_tput` were identical to three digits (15.74 / 15.74).
- The floor scales with the caller's instruction count at roughly **0.3 cyc per
  instruction**: 13 instructions -> 14.2, 18 -> 15.7, 19 -> 16.2. It is not a
  constant of the patching mechanism.
- **Bare redirection alone is ~7 cycles** (`probe_vmwrite_cost`: back-to-back
  triggers, no operand traffic). The rest of the floor is operand marshalling.

### Micro-op latencies, dependent chain (`probe_oplat`)
| op | latency |
|---|---|
| ADD / OR / AND / SHL / SHR | ~1.0 |
| ZEROEXT | ~0.7 |
| SETCC | ~1.9 |
| **MUL** | **~5.9** |

(These were reported by subtracting the full floor, which overlaps in a
latency-bound patch, so they are slight underestimates. Ordering is reliable.)

### Carry handling
- **`GENARITHFLAGS_RR(TMP, TMP)` bridges a computed carry into a following
  `ADC`. CONFIRMED with a matched control** (`probe_carry` [6] vs [7]):
  `{ADD(TMP0,R15,R15), GFL_RR(TMP0,TMP0), ADC(R15,RDI,RSI)}` gave
  `0000000000000000` when the ADD carried and `ffffffffffffffff` when it did
  not. Same triad, only the input differs.
- A carrying `ADD` does **not** reach a following `ADC` unaided ([4]/[5]).
- `STC`, `CLC`, `SAHF` execute without faulting but do **not** affect `ADC`
  ([2],[3],[10],[11],[12]). Do not use them.
- Arch CF is a single global, so only **one bridged carry can be in flight at a
  time**. This suits a serial accumulator and fights OpenSSL's five-accumulator
  layout. Keep the serial design.
- The intra-triad form `{ADC, GFL_RR(TMP,TMP), ADC}` is recorded as working
  (2026-05-22). `GFL_RR(arch, arch)` leaks CF - route chains through TMPs only.
  `ADC`'s destination may be an arch register (R15 worked in the probe).

### Opcodes verified by `probe_opsem`
- **`IMUL64L_DSZ64` = 0x264 is a non-destructive 64-bit low multiply.** Both
  forms verified exactly:
  `DRR(R15,RDI,RSI)` -> `lo(RDI*RSI)`, **both sources intact**;
  `DRI(R15,RDI,19)` -> `RDI*19`, RDI intact.
  It has no generated macro; construct it as
  `((0x264UL << 32) | INSTR_DRR(dst, srcA, srcB))`.
- `CONCAT_DSZ32` concatenates the low halves of two registers into 64 bits,
  non-destructively. There is **no** `_CONCAT_DSZ64` in opcode.h (the
  `CONCAT_DSZ64_*` macros in inst.h reference a missing opcode and will not
  compile).
- `MUL_DSZ64_DRR(hi, srcA, srcB)`: srcA preserved, **srcB receives the low
  half**, hi receives the high half. The MUL destination is free to be any
  register, including a TMP.
- `LAHF` returns a plausible flag word but did not track CF reliably; not useful.

### Size-field encoding
Across 28 opcode pairs in opcode.h the size field is bits [7:6]:
`DSZ64 = DSZ32 + 0x40` (27 of 28), `DSZ16 = +0x80`, `DSZ8 = +0xC0`.

---

## 3. SAFETY - read before firing anything new

**Opcode 0x957 in form `DRI(R15,RDI,13)` (uop `09570d02f227`) hard-crashed the
machine** on 2026-09-09. It was the predicted `SHRD` DSZ64 slot. The
fsync-before-fire logging in `probe_shrd.c` identified it precisely; keep that
pattern for any probe of unverified encodings.

**The fe_sq restructure hard-killed the machine on 2026-09-12.** The box died
instantly at 20:55 with NOTHING in the kernel log -- its last message was
from 10:03 that morning, and there is no oops, no MCE, no trace. That is the
signature of a microcode-level kill, not something the OS ever saw. Two
things were novel in that run, and both are now removed:

1. **`mov rcx, <mask>` immediately before the fe_sq trigger.** The trigger is
   `.byte 0f 78 ca`, which is **`vmread rdx, rcx`**: RCX is the VMCS
   field-encoding SOURCE operand and RDX the destination. Nothing in this
   tree had ever loaded a chosen value into RCX before that instruction --
   it had always held whatever the compiler left. A 2^51-1 field encoding
   has bits set above 32, which the SDM makes an unsupported component, and
   whatever the ucode does with that before reaching our hook at U0618 is
   not something we control. **The equivalent change on the fe_mul side is
   safe and shipped** -- but that trigger is `vmwrite rcx, rdx` at a
   different hook (U0cd8), so it proves nothing about this one.
   **The fe_sq mask now lives in R8**, which the old wrapper already wrote
   (as zero) before the same trigger.
2. **A triad containing three multiplies**, and another containing two. No
   validated patch in the tree contains either. The generator now refuses to
   put two multiplies in one triad, which probe_sched says is faster anyway.

Which of the two actually did it is UNKNOWN. `test_sq_fire.c` exists to find
out without spending another boot: it fires fe_sq five times, not millions,
and fsyncs a stage line before and after every step, so the last line in
test_sq_fire.log names the step that died. That is the same protocol that
localised opcode 0x957. **Run it before bench_kernel, every time a patch or
a wrapper's register convention changes.**

RULE ADDED: **the two trigger instructions have operands, and they are
registers we otherwise treat as free.** `vmwrite rcx, rdx` and
`vmread rdx, rcx` both read RCX and touch RDX. Loading either with a chosen
value before firing is a change to the hardware interface, not to our patch,
and must be treated as an unverified encoding would be.

### fe_sq five-accumulator patch: THREE hard resets, cause not found

Localised by the fsync-before-fire protocol across three boots:

| tested | result |
|---|---|
| patch install (both patches) | safe |
| `fe_sq_ucode` standalone, 5 firings, inputs to 2^53-1 | **correct**, matches fiat |
| `FE_SQ` macro, memory to memory | **correct** |
| `FE_SQ_FROM_REGS` | **correct** |
| fe_mul and fe_sq fired in ONE asm block | **correct** |
| `fe_sq_ucode_n` looped, n = 1,2,3,4,5 | **correct** |
| `fe_sq_ucode_n` looped, n = 10, 20, 50, 100 | survived |
| **`fe_invert_ucode`** | **KILLS THE MACHINE** |

**The patch is not wrong.** It has matched fiat-crypto on every path where
a result could be checked, and ~195 consecutive firings preceded the fatal
one in the same process. fe_invert_ucode is nothing but the calls above in
sequence, every one of them individually cleared.

**CRASH #4, 2026-09-13: THE FAILURE IS NOT DETERMINISTIC.** `test_sq_why`
died inside its FIRST `fe_sq_ucode_n(o, a, 100)` -- the identical call,
shape and n that survived one boot earlier in `test_sq_paths`, there after
~95 prior firings. Same patch, same wrapper, same operand shape, opposite
outcome.

That single fact invalidates the entire bisect programme:

- **"Survived" is not evidence of safety**, it is one sample of a
  nondeterministic failure. Every "cleared" row in the table above is
  therefore worthless as a safety argument, including the ones that made
  this patch look shippable.
- **No bisect can converge.** Splitting call shapes only works if a pass
  means something.
- The patch remains CORRECT -- every result it ever produced matched
  fiat-crypto. It is correctness that is established and *stability* that
  is not.

**What this does NOT indict: the five-accumulator technique.** The fe_mul
patch is the same structure -- five independent 128-bit accumulators, five
SETCC chains, the same two-pass reduction -- and it has been stable across
millions of firings and every bench_kernel run. So the method is sound and
something specific to the fe_sq patch is not.

The one op form the fe_sq patch uses that the proven fe_mul patch never
does is **`MUL_DSZ64_DRR(dst, X, X)` with srcA == srcB**, for a0*a0, a1*a1
and a2*a2. The old serial fe_sq contained exactly one such multiply and was
stable for months; the new one has three. That is the only concrete
structural difference on the table. It can be removed for 3 ops (~1 cyc) by
staging a copy first, giving a patch whose every op form already appears in
something proven -- but validating it would need a long soak, not a single
pass, and this has cost four resets already.

Two hypotheses were live before crash #4; `test_invert_bisect.c` was built
to separate them and is now pointless:

- **A, a specific call shape not yet covered.** Three shapes appear in
  fe_invert and nowhere in the tests: `fe_sq_ucode(t, t)` ALIASED,
  `fe_sq_ucode_n(t3, t3, n)` ALIASED, and **`fe_mul_ucode`, whose C-wrapper
  form has never been fired in any of these tests** -- only its `FE_MUL`
  macro form has.
- **B, cumulative.** fe_invert is ~260 firings on top of ~195 already done.
  Nothing has run that many in one process. If the death tracks a firing
  COUNT rather than a call, that is the finding, and it would be the first
  evidence of state accumulating across firings.

`test_invert_bisect.c` reproduces fe_invert call for call with a fsynced
stage line and a running firing count around every one. It will probably
crash; it is a bisect, not a fix.

**Theories already killed by evidence, recorded so they are not re-run:**
- *RCX at the vmread trigger.* `FE_MUL` loads 2^51-1 into RCX and the
  ladder then fires `vmread rdx, rcx` with it still there -- that ran
  through every bench_kernel measurement this week with the old sq_patch.
  Removing it did not prevent crash #2. **Not the cause.**
- *Multiply clustering.* The first version had a 3-MUL triad; the
  de-clustered version still crashed. Not the cause (still worth keeping
  for speed, per O6).
- *The two patches coexisting in one asm block.* Explicitly tested, safe.
- *Looped firing with a stack-held counter across a firing.* Tested to
  n=100, safe.

**STATUS 2026-09-12: REVERTED.** Production carries the serial fe_sq again,
byte-identical to the version that measured 81.8. The five-accumulator
version is parked in the same file as `sq_patch_5acc` (simulate with
`--mask-r8`), its generator is `lib/gen_sq_patch.py`, and
`test_invert_bisect.c` is built and ready if the cause is ever worth more
boots. The fe_sq wrapper contract reverted with it: all five firing sites
zero R8 again.

fe_mul's 102.8 is unaffected and measured -- it is the validated win, and
the shipped pair is now exactly the combination that has run clean all
week.

Other known-dangerous ground:
- Mass-packing two memory operations per triad hard-crashed the box previously.
  Production stays at one memory op per triad.
- Backward `UJMPCC` crashes; use `SEQ_GOTO0`.

**Do not resume the funnel-shift hunt.** It is finished and it cost a machine.
`SHRD` 0x997 was fully decoded: the count is masked to 5 bits (imm 51 == imm 19,
imm 63 == imm 31) and every result equals `(src_lo32 << (32-count)) & 0xFFFFFFFF`
with the low half hardwired to zero. It is a 32-bit `SHL`. 0x917 returns zero.
0x957 kills the machine. Untested slots (0x9d7, and SHLD 0x916/0x956/0x996/0x9d6)
are not worth the risk: even a working 64-bit funnel would need three inputs
(high, low, count) and `INSTR_DRI`/`INSTR_DRR` carry only two, which the 32-bit
behaviour already demonstrated.

---

## 4. Current state

### Files
- Patches: `full_curve25519_inline2.c`, `install_field_patches()`, arrays
  `mul_patch[]` (66 triads, from line ~111) and `sq_patch[]` (42 triads, ~252).
- Wrappers: `FE_MUL` / `FE_SQ` macros (~line 373 / ~404), `FE_MUL_FROM_REGS_A`,
  `FE_SQ_FROM_REGS`.
- Patch RAM layout: fe_mul at U7c00 (66 triads -> U7d04), fe_sq at U7d08
  (42 triads -> U7dac). 128 triads total available at U7c00 via the bootstrap
  reclaim trick (call lib helpers BEFORE `patch_ucode`). Budget after the
  rewrite: 49 + 31 = 80 triads, comfortable.
- Benchmarks: `bench_kernel.c` (kernel table, gated on verification against
  fiat-crypto), `bench_supercop_matrix.sh` (end-to-end sweep).

### Op census of the current patches
```
fe_mul  66 triads  196 ops   ADD=72 ZEROEXT=40 MUL=30 SETCC=25 SHR=12 SHL=11 OR=5 NOTAND=1
fe_sq   42 triads  123 ops   ADD=49 MUL=16 SETCC=15 SHR=12 ZEROEXT=11 SHL=11 OR=5 NOTAND=4
```
Two overheads dominate and native code pays neither:
- **carry emulation ~23% / ~20%** (SETCC plus the TMP9 folds)
- **operand staging ~20%** (the 40 ZEROEXTs, forced by MUL destroying srcB)

### What OpenSSL does differently
Five independent 128-bit accumulators, all 25 products added in with no
inter-limb carry propagation, then one lazy reduction written as a balanced tree
(`h2->h3` and `h0->h1` are independent, then `h3->h4` and `h1->g2`). Its
accumulate is `addq` + `adcq` = 2 instructions where ours costs 4 (or 3 with the
ADC bridge), so their advantage in *operation count* is `adcq` and a
non-destructive `mulq`, not the algorithm.

**Their advantage in dependency structure IS the algorithm, and that is the one
that matters for us** - five independent accumulation chains and a balanced
reduction tree, against our single serial accumulator with a 0->1->2->3->4 carry
chain. We are dependency bound at ~2.8x our independent issue rate (section 2),
so copy the structure. See 5.0.

---

## 5. The plan

### 5.0 PRIMARY LEVER: expose parallelism (this is the reversal)

An earlier version of this plan said to keep the serial single-accumulator
design and rejected OpenSSL's five-independent-accumulator layout on the grounds
that it saves no operations in microcode - each accumulate still costs 4 ops
(ADD, SETCC, fold, fold) either way. **That reasoning was right about op count
and wrong about what matters.** We are dependency bound with roughly 2.8x of
stall headroom, so five independent accumulation chains instead of one serial
chain is exactly the restructure to make, even at identical op count.

The objection I raised against it does not apply. I worried the single global
arch CF would stop parallel carry chains - but that constrains **ADC** only.
**SETCC reads domain #1, which is per-TMP-register, so five independent SETCC
carry chains coexist happily.** So:

- **five accumulators + SETCC chains**: 4 ops per accumulate, five-way parallel
- **one accumulator + ADC bridge**: 3 ops per accumulate, strictly serial
  (only one bridged carry in flight)

These are alternatives, not partners. Given the measurements, **build the
parallel SETCC version first** and treat the ADC bridge as a fallback if the
parallel version turns out to be register-starved.

Structure to copy (from OpenSSL's `fe51_mul`, section 4):
- five 128-bit accumulators `(lo_j, hi_j)`, j = 0..4, ten registers
- all 25 products accumulated with **no** inter-limb carry propagation
- 19-folding by scaling the g values in place
- **one lazy reduction at the end, written as a balanced tree** - `h2->h3` and
  `h0->h1` are independent, then `h3->h4` and `h1->g2`. Do not serialise this
  into a 0->1->2->3->4 chain; the tree is half the depth and that is the point.

Register budget: 10 accumulators + 5 g values + f_i + mask + scratch is roughly
18-20 of the 32 available. Tight but feasible. If it does not fit, fall back to
three accumulators in parallel rather than one.

### 5.0 IMPLEMENTED 2026-09-09 - offline numbers (hardware result in the next section)

`mul_patch` in `full_curve25519_inline2.c` is now the five-accumulator
version. The previous serial patch is kept in the same file as
`mul_patch_serial[]` inside `#if 0`, so both tools still read it and the A/B
is reproducible.

| | serial (was) | 5-accumulator (now) |
|---|---|---|
| triads | 66 | **58** |
| ops | 196 | **174** |
| n_mul / n_other | 30 / 166 | **29 / 145** |
| depth, MUL=5.9 | 74.9 | **25.4** |
| independent-issue floor | 52.7 | **48.6** |
| verdict | depth 1.42x the floor: **dependency bound** | depth 0.52x the floor: **issue bound** |

That is the whole point of 5.0: depth is now *below* the independent-issue
floor, so the cost model's `r` should fall from 0.48 toward the 0.153-0.25
band. Predicted total at `r = 0.30` is ~85 cyc and at `r = 0.25` ~78 cyc,
against OpenSSL's 97.1. **At `r >= 0.38` it loses**, so the measurement is
still the decision.

What differs from a literal transcription of `fe51_mul`:

- **The reduction is two fully parallel passes, not OpenSSL's tree.**
  OpenSSL propagates each carry into the *128-bit* accumulator (`addq` +
  `adcq`); in microcode that is ADD + SETCC + ADD. Splitting every
  accumulator into `r_j = lo_j & MASK` and `q_j = (lo_j>>51)|(hi_j<<13)`
  first makes every carry add a plain 64-bit ADD, and all five splits issue
  in parallel. Pass 1 lands `t_j < 2^63.6`; pass 2 lands every limb at
  `< 2^51 + 2^17`. Costs 6 ops more than the tree and about 14 less depth.
- **The carry folds into the product's high half, not into `hi_j`.**
  `ADD(hi_p, hi_p, c)` then `ADD(hi_j, hi_j, hi_p)` keeps the `hi_j` chain
  4 deep instead of 8, at identical op count.
- **Row 0 initialises all five accumulators for free** (5.1B): MUL writes
  its low half into srcB, so staging `b_j` into `lo_j` *is* the
  initialisation - no ADD and no SETCC for the first product of each limb.
- **5.1C and 5.1E came along for free.** `IMUL64L` does the four `19*b_j`
  scalings non-destructively (4 ops, no staging), and the wrapper now
  passes `2^51-1` in RCX so masking is one AND instead of SHL13/SHR13. O4
  is answered by inspection: `tests/ultimate_bench.c` already runs
  `AND_DSZ64_DRR(TMP1, TMP3, RCX)` against an arch-register mask.
- **Scratch is rotated, never reused back-to-back.** A false dependency on
  a shared staging register would re-serialise exactly what this
  restructure exists to remove, so registers are marked busy until the
  product that owns them is fully accumulated, and each `a_i` / `b_j` joins
  the scratch pool as its last product issues.

Offline verification done (section 7 steps 1 and 2):

```
python3 lib/ucode_sim.py      full_curve25519_inline2.c mul_patch mul   # 0/3000 PASS
python3 lib/ucode_critpath.py full_curve25519_inline2.c mul_patch       # depth 25.4
python3 lib/ucode_audit.py    full_curve25519_inline2.c mul_patch mul   # PASS
```

`lib/ucode_audit.py` is new and checks what the other two cannot see:
read-before-write against the wrapper's entry set (the simulator zero-fills,
hardware does not), SETCC sources being TMP, domain-#1 flag freshness, one
memory op per triad, and END_SEQWORD placement. Both pre-existing patches
pass it, which is the control.

An extended sweep (80k random trials at limb widths 51-54, plus
all-limbs-maximal and structured edge cases) is clean, and confirms the
**input bound is < 2^54**: `2^54-1` inputs are correct, `2^55-1` are wrong.
The binding constraint is `19*q_4 < 2^64`. The ladder feeds at most
`2^53.1`, so the margin is a factor of 2.

**Not yet measured on hardware.** Nothing here is a performance claim.

### 5.0 MEASURED 2026-09-09 - the hypothesis in section 2 is REFUTED

Clean `bench_kernel` run, pinned (`no_turbo=1`, userspace), drift check
+0.04%, RFC 7748 PASS over 1000 chained iterations.

| | serial (was) | 5-accumulator (now) | OpenSSL |
|---|---|---|---|
| fe_mul, dependent chain | 122.9 | **102.8** | 97.1 |
| patch body (full - 16.18 floor) | 106.7 | **86.6** | |
| triads / ops | 66 / 196 | 58 / 174 | |
| dependency depth | 74.9 | 25.4 | |
| **achieved `r`** | 0.478 | **0.415** | |

**fe_mul improved 16.4% and still loses to OpenSSL by 5.9%.** Depth fell
**2.95x** and `r` fell **13%**. Section 2 predicted `r` would collapse toward
0.25-0.30 for ~78-85 cyc. It did not. The cost model's shape held - it
predicts 102 at r=0.415 and measured 102.8 - but **`r` is not primarily set
by exposed parallelism**, which was the premise of the whole restructure.

What the three measured points actually say:

| patch | triads | depth | body | cyc/op | **cyc/triad** |
|---|---|---|---|---|---|
| fe_mul 5-acc | 58 | 25.4 | 86.6 | 0.498 | **1.493** |
| fe_mul serial | 66 | 74.9 | 106.7 | 0.544 | **1.617** |
| fe_sq | 42 | 59.3 | 65.3 | 0.531 | **1.555** |

A two-term fit on the two fe_mul points gives

```
body = 1.415 * triads + 0.178 * depth        (fe_sq holdout: 70.0 vs 65.3, 7% error)
```

**A triad costs 8x what a unit of dependency depth costs.** At depth 25.4,
the entire depth term is 4.5 of 86.6 cycles. The earlier version of this
plan asserted a flat ~1.61 cyc/triad and section 2 threw it out as an
artifact of fitting two patches with similar dependency structure; the
five-accumulator patch is the third point with a *completely* different
structure, and it lands at 1.49 cyc/triad. **The triad model was closer to
right than the model that replaced it.** Depth is worth chasing only when it
is free in triads - and this rewrite twice paid triads to buy depth, which
the numbers now say was backwards (see 5.0-next).

Still unexplained, and the biggest prize on the table: `probe_issue` measures
a synthetic patch with this op mix at **0.58 cyc/triad** against production's
**1.49**. Depth accounts for 0.08 of that gap. So **2.4x remains
unaccounted for** - not dependency stalls, not op mix. If a patch could be
made to run at the synthetic rate, fe_mul's body would be 34 cyc and the
total 50. Nobody has explained this gap; it deserves one probe before more
op-shaving. See O5.

### 5.0-next: what is left, after two refuted models

**Read 5.0-lever-1 first.** Levers 1 and 2 below were ranked by a
cost-per-triad fit that lever 1 then refuted out of sample. Lever 1 was
built and it LOST. Treat every projection in this subsection as a
hypothesis, and measure one change at a time - that discipline is the only
reason the lever-1 regression was attributable at all.

What survives:

1. ~~ADC bridge on the accumulates~~ **DEAD. Measured 104.0 vs 102.8.**
2. **Undo this rewrite's two depth-for-triads trades. -16 ops.** Still
   plausible, magnitude now unknown. Both were chosen against the `r` model
   and neither buys much depth:
   - the two x19 chains as shifts+adds cost 6 ops to save ~6 depth. Revert
     to `IMUL64L`, which trades 6 ALU ops for 2 multiplies.
   - the two-pass parallel reduction costs 48 ops against OpenSSL's tree at
     38. **But the tree needs 128-bit carry propagation, and lever 1 just
     showed the cheap way to do that (ADC) is the expensive way.** With
     SETCC the tree's carries cost 3 ops each, so the tree saves less than
     10 ops and lengthens the critical path. Re-cost it before building it.
   Unlike lever 1 this only removes ops of kinds already in the patch, so
   it does not repeat lever 1's mistake of introducing an op class whose
   cost was never measured.
3. **fe_sq is untouched and is the wider gap: 81.4 vs 73.2, 11.2% behind.**
   Still the serial design at 42 triads, depth 59.3. The five-accumulator
   restructure bought fe_mul 16.4%; the same treatment should help fe_sq at
   least as much, since its depth-to-triad ratio is worse. **This is the
   best-understood remaining win and it repeats a change already validated
   on hardware.** Do this next.
4. **Wrapper: 16.18 cyc, 15.6% of the op.** `xor eax,eax` / `xor r8d,r8d`
   survive in FE_MUL only to keep ucode_sim's zero-fill faithful, and
   `lib/ucode_audit.py` proves the patch never reads either before writing
   it. Dropping both is ~0.6 cyc; stop seeding them in the simulator
   instead. The 10 loads and 5 stores are irreducible.
5. **O5 is now the largest unknown by far, and O6 joins it.** Production
   runs at 1.49 cyc/triad against `probe_issue`'s 0.58 for the same op mix,
   and neither model that tried to explain the gap survived. **O6: two
   schedules with an identical census differ by 6%** (5.0-lever-1), which
   means the residual is at least partly schedule and register assignment,
   not a per-op constant at all. A probe that sweeps op ORDER and scratch
   register count at fixed census would attack both at once, and would have
   caught the 108.5 regression before it reached hardware. **Given that
   census-based projection has now failed three times, this probe is worth
   more than any further op-shaving.**

Patch RAM: 58 + 42 = 100 of 128 triads.

### 5.0-lever-1 MEASURED AND REVERTED - and an accident worth more than it

Four schedules of the same fe_mul, all measured on hardware, all with
identical semantics and all verified by ucode_sim:

| schedule | ops | triads | depth | total |
|---|---|---|---|---|
| serial single accumulator | 196 | 66 | 74.9 | 122.9 |
| **5-acc, SETCC, 1-deep pipeline** | 174 | 58 | 25.4 | **102.8 SHIPPED** |
| 5-acc, ADC carry bridge | 154 | 52 | 23.5 | 104.0 |
| 5-acc, SETCC, 2-product batches | 174 | 58 | 25.4 | 108.5 |

**Rows 2 and 4 are the finding. Same op count, same triad count, same
dependency depth, 5.7 cyc apart - 6.6% of the body.** The only difference is
the order ops are issued in and which scratch registers the allocator handed
out. Row 4 happened by accident: reverting the ADC experiment through a
reparameterised generator produced a different schedule with the same
census, and it cost more than the ADC bridge ever did.

So: **on this core, instruction schedule and register assignment are worth
~6% at fixed op count, and no model in this document can see that.** Every
projection in sections 2 and 5 is computed from a census - ops, triads,
depth - and two schedules with an identical census differ by 5.7 cyc. That
is larger than lever 1's effect and comparable to a substantial op-count
change.

Practical consequences:

- **A patch's census does not determine its speed.** Any change must be
  measured, not projected. Both refuted models (r-as-parallelism,
  cost-per-triad) were census models, which is probably why they failed.
- **`lib/gen_mul_patch.py` is now load-bearing** and must reproduce the
  shipped array byte-for-byte; the header carries all four measurements and
  the warning. The ADC/batched variant lives in
  `lib/gen_mul_patch_adc.py` so both negative results stay reproducible.
- **The 1-deep interleave is what to keep**: product k+1's multiply issued
  between product k's multiply and the adds that consume it, with scratch
  rotating so no two products in flight share a register. Batching two
  products' multiplies and then both accumulates is the 108.5 variant.

**On the ADC bridge itself: 5.1A is dead.** 104.0 against 102.8, despite 20
fewer ops and 6 fewer triads. Removing 20 ops at the measured `r` should
have saved 8.3 cyc, so the ADC form costs ~0.47 cyc/accumulate more than the
four-op SETCC run. **Why: SETCC reads domain #1, which is per-register.**
Five accumulators own five independent flag domains and their carry chains
genuinely run in parallel; the bridge funnels all 20 carries through the one
architectural CF. Triad-locality makes each group correct in isolation but
does nothing to make consecutive groups independent. Section 5.0 said "these
are alternatives, not partners" and framed one-carry-in-flight as a
correctness constraint that triad-locality satisfies - **it is also a
throughput constraint, and triad-locality does not satisfy that one.**

`lib/ucode_critpath.py` cannot model the bridge: it treats the arch CF as a
scalar each GENARITHFLAGS overwrites, so consecutive bridges look
independent, and it reported depth 23.5 for a schedule whose carries are
~20 deep through the flags register. Do not trust its depth for a patch that
uses ADC. `lib/ucode_audit.py` keeps its two bridge rules (same-TMP GFL_RR,
ADD/GFL/ADC triad-locality), verified to fire against deliberately broken
patches, in case the bridge is ever wanted for a genuinely serial chain.

### 5.1 SECONDARY LEVER: five op-count reductions

### A. ADC bridge replaces SETCC carry emulation
```
now:  ADD(TMP0,TMP0,lo)   SETCC(TMP15,TMP0)   ADD(TMP9,TMP9,TMP15)   ADD(R8,R8,hi)    = 4 ops
new:  ADD(TMP0,TMP0,lo)   GFL_RR(TMP0,TMP0)   ADC(R8,R8,hi)                            = 3 ops
```
Saves 1 op per accumulation: **-20 in fe_mul** (25 accumulations, 5 are
initialising), **-10 in fe_sq**.
Constraints: the lo accumulator must be a TMP; ADC's dest may be arch; keep the
three ops adjacent (see open question O1); only one carry in flight.

### B. First product of each limb initialises in place - zero extra ops
`MUL(acc_hi, srcA, acc_lo)` where `acc_lo` already holds the staged srcB writes
the low half into `acc_lo` and the high half into `acc_hi` directly. No ZEROEXT
pair to initialise the accumulator. **-10 ops in fe_mul, -10 in fe_sq** versus a
naive rewrite (this is partly how the current code already works; verify per limb
rather than assuming).

### C. `IMUL64L` for every x19 scaling
`MUL_DSZ64_DIR(RCX,19,x)` destroys x, wastes the high half, and needs the value
staged first. `((0x264UL<<32)|INSTR_DRI(dst, x, 19))` is one non-destructive op.
Applies to the 4 prep multiplies in fe_mul and the final reduction in both.
**-5 to -8 ops in fe_mul.**

### D. Destructive-last-use operand ordering
MUL's srcB is destroyed. Each of the 10 input values is used 5 times; order the
25 products so each value is consumed destructively on its **last** use, removing
that staging copy. At most 10 of 25 products can be free this way.
**-8 to -10 ops in fe_mul, -3 to -5 in fe_sq.**

### E. Mask register instead of SHL13/SHR13 pairs
`h_i = acc_lo & (2^51-1)` currently costs `SHL(t,acc,13)` + `SHR(h,t,13)` = 2
ops. With the mask in a register it is `AND_DSZ64_DRR(h, acc, MASKREG)` = 1 op.
Five per kernel: **-5 ops each.**

Getting the mask in cheaply: have the **wrapper** load it, since building it
inside the patch from 16-bit immediates costs ~5 ops. RCX is the only free arch
register, and it is currently used as the MUL high destination - but the MUL
destination is free, so **retarget MUL's high output to a TMP and let RCX hold
the mask**. Cost: one wrapper instruction (~0.3 cyc, and it is issue-bound so it
overlaps the firing).

### Carry extraction stays at 3 ops
`carry = (acc_lo >> 51) | (acc_hi << 13)` = SHR + SHL + OR. With no 64-bit funnel
shift this is the floor. OpenSSL pays the same shape in its lazy reduction.

### Resulting budget

Op-count reductions alone (`r` unchanged at 0.48):
```
fe_sq   16 MUL + ~85 other  -> 14.6 + 40.8 = 55 body -> ~71 total   (beats 73.2, thin)
fe_mul  30 MUL + ~120 other -> 27.4 + 57.6 = 85 body -> ~101 total  (LOSES to 97.1)
```
With the parallel restructure bringing `r` to a defensible 0.30:
```
fe_sq   14.6 + 25.5 = 40 body -> ~56 total    (beats 73.2 by 23%)
fe_mul  27.4 + 36.0 = 63 body -> ~79 total    (beats 97.1 by 19%)
```

**Conclusion: op-count work alone wins fe_sq and loses fe_mul. The parallel
restructure is what wins fe_mul.** Do 5.0 first and measure `r` before spending
effort on 5.1; if `r` does not move, the op-count work will not save fe_mul and
the honest move is to reframe the paper (section 10).

## 6. Open questions to settle before writing 100 triads of patch

Cheap probes, all low-risk (verified opcodes only, no encoding fishing).

- **O1. Must ADD, GFL and ADC be in the same triad?** The confirmed case had all
  three in one triad. If the bridge survives across triads, scheduling is much
  freer and packing density improves. If not, each accumulation is exactly one
  triad, which is 3 slots fully used - still fine, but the plan's op-to-triad
  conversion becomes exact rather than optimistic. **Test first; it shapes
  everything.**
- **O2. Does a second GFL clobber a pending first?** Determines whether two
  accumulations can be interleaved for latency. Expected: yes, they serialise.
- **O3. ANSWERED 2026-09-09.** Not content-independent, and not an issue rate.
  See section 2. `probe_issue` is in the tree and can be re-run to re-measure `r`
  after each restructure - that is now its main job. Note the version that
  produced the first (bogus) numbers had two bugs: its patch wrote nothing the
  wrapper stored, so firings overlapped and it measured aggregate throughput,
  and nothing verified the flow reached the last triad. Both are fixed: each arm
  ends with `ZEROEXT(R15, TMP0)` and the headline is a slope over a triad sweep.
  **Any new microcode timing probe must do both of those things.**
- **O4. Does `AND_DSZ64_DRR` against an arch register holding the mask behave?**
  One firing, trivially checked.

- **O5. Why does production run at 1.49 cyc/triad when `probe_issue` says
  0.58 for the same op mix?** 2.4x, unexplained by depth (worth 0.178/unit)
  or op mix. This is now the largest single unknown and the largest
  potential win. Candidate causes: `probe_issue`'s arms use few distinct
  registers while production has ~30 live, so the synthetic may not pay a
  register-file or port cost; or per-triad sequencer overhead that the
  synthetic amortises differently. A probe that sweeps *register count* and
  *chain width* at fixed triad count and op mix would separate these.

---

## 7. Verification protocol - non-negotiable

The load-order bug (see section 9) was invisible for months because nothing
checked. Every step here is cheap and catches a real class of error.

1. **Offline simulator first - `lib/ucode_sim.py`.** Parses a patch array straight
   out of the C source and executes it against a reference multiply/square over
   3000 random inputs, including loose limbs up to 2^53 and all-ones edge cases.
   **Run every candidate patch through it before it ever touches hardware.** No
   root needed, takes seconds, and catches register-allocation errors that would
   otherwise cost a root run.
   ```
   python3 lib/ucode_sim.py full_curve25519_inline2.c sq_patch  sq   # 42 triads, 123 ops, PASS
   python3 lib/ucode_sim.py full_curve25519_inline2.c mul_patch mul  # 66 triads, 196 ops, PASS
   ```
   It models both flag domains: ADD/ADC set a per-register domain-#1 flag,
   `GENARITHFLAGS_RR` copies it into the arch CF, ADC reads that. It rejects
   `GFL_RR(x,y)` with different registers, since that form leaks CF on hardware.
   Raw opcodes with no inst.h macro are understood only if the C wraps them, so
   add to the patch source:
   ```c
   #define IMUL64L_DSZ64_DRR(d,a,b) ((0x264UL << 32) | INSTR_DRR(d,a,b))
   #define IMUL64L_DSZ64_DRI(d,a,i) ((0x264UL << 32) | INSTR_DRI(d,a,i))
   ```
2. **Dependency-depth check - `lib/ucode_critpath.py`.** Reports depth against
   the parallel-issue floor `n_mul*0.91 + n_other*0.153`. Depth above that floor
   means the patch is dependency bound. Both production patches are:
   ```
   sq_patch    triads=42  ops=123  depth=59.3   independent-issue=30.9
   mul_patch   triads=66  ops=196  depth=74.9   independent-issue=52.7
   ```
   **This is the metric to drive down in the 5.0 restructure.** Op count moves
   the floor; the restructure has to move `depth` toward it. If a rewrite cuts
   ops but leaves depth where it is, it will not deliver - that is exactly what
   happened with `sq_fast` (section 9).
3. **RFC 7748 on hardware.** `bench_kernel` and `full_curve25519_inline2` both
   gate on it before timing. Never report a number from a run whose gate failed.
4. **Compare against fiat-crypto per-op**, not just end-to-end - `bench_kernel`'s
   `verify_new_backends()` pattern.

---

## 8. Build and run reference

All from `simple/curve25519/`, as root, on a pinned core
(`no_turbo=1`, `userspace` governor; `../pin_cpu.sh` or `lib/freq_guard.sh`).
Unpinned runs understate everything by ~2.2x.

```
make PROG=bench_kernel        && sudo taskset -c 0 ./bench_kernel_static
make PROG=probe_issue         && sudo taskset -c 0 ./probe_issue_static
python3 lib/gen_kernel_table.py     # -> KERNEL_TABLE.md, kernel_table.tex
./bench_supercop_matrix.sh          # end-to-end sweep, ~16 min
```

Probes already written: `probe_sq_lat`, `probe_trigger`, `probe_wrapper`,
`probe_oplat`, `probe_ldorder`, `probe_issue`, `probe_opsem`, `probe_carry`,
`probe_shrd`. All restore the original microcode state on exit, including on
failure paths.

---

## 9. Dead ends - do not retry

- **Funnel shifts.** Section 3. Finished, and 0x957 kills the machine.
- ~~Copying OpenSSL's five-accumulator layout.~~ **RETRACTED - this is now the
  primary plan, see 5.0.** It was rejected for saving no operations, which is
  true and irrelevant: we are dependency bound, and the one-carry-in-flight
  objection applies only to ADC, not to SETCC's per-TMP flag domain.
- **Restructuring the cross-limb carry chain for latency** (`sq_fast`,
  `sq_fast_patch.h`). Cut dependency depth 45 -> 33 ops and bought 2.0 cycles;
  after the load-order fix it is a *regression* (86.5 vs 81.8). The patches are on
  disk if wanted as a negative result, otherwise delete.
- **Swapping which hook fires which kernel.** Trigger instruction is worth 0.0
  cycles (`probe_trigger` 2x2 cross); the match/patch entry index likewise.
- **RDX / the LEA and IMUL mix in the wrapper.** ~0.5 cycles total.
- **`STC` / `CLC` / `SAHF`** for carry control. No effect on ADC.
- **The recorded "MUL tput 2.5 cyc" figure.** Measured at **0.91** by
  probe_issue. Correct the project memory note.
- **The microcode ROM dump as an opcode oracle.** `MUL_DSZ64` appears zero times
  in it despite being in every patch we ship. It can confirm presence, never
  absence.

---

## 9a. probe_sched: O5 AND O6 ANSWERED 2026-09-09

`probe_sched.c`, four families, triad sweep 12-42, slope as the headline.
Harness validity check: the sweep's intercept is **16.14 cyc against the
independently measured 16.18 dispatch floor**, and independent ALU ops
saturate at exactly 3.01 per cycle on a 3-wide core.

### O5 is not a mystery, it was a bad number

| | cyc/op | ops/cycle |
|---|---|---|
| probe_sched, independent ALU | **0.332** | 3.01 |
| probe_issue's `alu3` | 0.153 | 6.54 |

**6.54 ops/cycle is impossible on this core**, and section 2 flagged that as
a caveat and then used 0.153 anyway. It is wrong; firings were still
overlapping. **Every "independent-issue floor" figure in this document is
therefore wrong**, including the ones printed by `lib/ucode_critpath.py`.
The measured costs are:

```
independent ALU op        0.332 cyc
MUL, <=1 per triad        1.328 cyc
MUL, 3 per triad          1.964 cyc     (+48%)
ALU-only triad            0.996 cyc     (i.e. one triad per cycle)
```

**The corrected model, and it is a good one:**

```
body = n_mul * 1.33 + n_other * 0.332  + clustering + stalls
```

| patch | body | base model | residual | depth | explanation of residual |
|---|---|---|---|---|---|
| fe_mul 5-acc SETCC (shipped) | 86.6 | **86.6** | **+0.0** | 25.4 | none needed |
| fe_mul 5-acc batched (108.5) | 92.3 | 86.6 | +5.7 | 25.4 | 5 extra 2-MUL triads |
| fe_mul 5-acc ADC bridge | 87.8 | 80.0 | +7.8 | 23.5 | arch-CF serialisation |
| fe_mul serial | 106.7 | 94.9 | +11.8 | 74.9 | dependency stalls |
| fe_sq serial | 65.2 | 56.7 | +8.5 | 59.3 | dependency stalls |

**The shipped fe_mul sits exactly on its op-count floor.** There is no 2.4x
of hidden headroom; that gap was an artifact. The only lever left for fe_mul
is a genuinely lower operation count.

### O6 is answered: multiply packing

The width sweep saturates at **W=3-5** (0.996 cyc/triad from W=5 upward), so
production's five accumulators already reach the issue limit - more
parallelism would buy nothing. And the register sweep is **completely flat
from R=1 to R=16**: 126 adds all writing the SAME register cost exactly the
same as 126 writing 16 different ones. **The register file is fully renamed,
WAW/WAR false dependencies are free, and scratch rotation is decoration.**
So register assignment cannot explain the 5.7 cyc.

Multiply packing can, and does. Clustering multiplies costs **48% more per
multiply** (1.964 vs 1.328 cyc). Counting multi-multiply triads in the two
schedules with the identical census:

| | triads with >=2 MULs | MULs in them |
|---|---|---|
| shipped (102.8) | 4 of 58 | 9 |
| batched (108.5) | 9 of 58 | 19 |

Ten more clustered multiplies at +0.64 cyc each is +6.4 predicted against
+5.7 measured. **Rule: never put two multiplies in one triad.** That is now
the one scheduling constraint that matters, and it is cheap to check.

### What this says to do

- **fe_sq, immediately.** It carries 8.5 cyc of dependency stalls that the
  five-accumulator restructure is known to remove, and its floor is
  `16*1.33 + ~85*0.332 = 49.5` against OpenSSL's 65.2-16.18 = 57.0 body.
  **Restructured fe_sq should land near 66 cyc total against OpenSSL's 73.2
  - a ~10% win**, and it repeats a change already validated on hardware.
- **Lever 2's x19 revert is now predicted to LOSE.** Trading 6 ALU ops for 2
  multiplies is -6*0.332 + 2*1.33 = **+0.7 cyc**. Do not do it. This is the
  model earning its keep.
- **The reduction tree saves 10 ALU ops = 3.3 cyc**, taking fe_mul to ~99.5
  total. Still short of 97.1, so op-shaving alone will not close fe_mul.
- `lib/ucode_critpath.py`'s "independent-issue" line should be recomputed
  with 1.33/0.332, and a MUL-per-triad histogram belongs in
  `lib/ucode_audit.py` as a warning.

### One arm was invalid, and how it was caught

The first `dist` family measured 8.9-9.1 cyc/triad, flat in D. That is
~3 cyc/op for ALU-and-multiply, which is physically wrong. Cause:
`MUL_DSZ64_DRR` writes its srcB, so 63 multiplies all naming `RSI` formed a
single **63-deep RAW chain: 63 * 5.9 = 372 cyc against 376 measured.** The
arm was measuring multiply latency, not scheduling distance. Fixed to use
non-destructive `IMUL64L` producers and five accumulators. The tell was
comparing the number against a latency the tree had already measured -
worth doing to every probe result before believing it.

## 9b. FINAL MEASURED STATE 2026-09-09

Pinned (`no_turbo=1`, userspace), RFC 7748 PASS over 1000 chained
iterations, in-run drift +0.02%.

| backend | fe_mul | fe_sq |
|---|---|---|
| **microcode (this work)** | **102.8** | **81.8** |
| OpenSSL fe51 asm | 97.1 | 73.2 |
| s2n-bignum (verified asm, 4x64) | 110.4 | 105.9 |
| fiat-crypto | 132.2 | 117.5 |
| hand-written `__uint128_t` C | 144.2 | 114.8 |
| CryptOpt | 141.4 | 121.0 |
| amd64-51 asm | 146.0 | 118.8 |

fe_mul went 122.9 -> 102.8, a **16.4% improvement**, from the
five-accumulator restructure. fe_sq is untouched. Against OpenSSL we are
**5.9% behind on fe_mul and 11.7% behind on fe_sq**; against everything else
we are ahead, by 22% on fe_mul and 30% on fe_sq versus fiat-crypto.

**METHODOLOGY NOTE, learned the hard way.** Cross-run comparison at the
1-2% level is only valid when the native arms reproduce. Over four runs
the native controls (`ossl_mul_lat`, `fiat_mul_lat`, `a51_mul_lat`) held to
<=0.03% for three of them and then drifted **4.96%** on the fourth, while
the microcode arm reproduced to +0.02 cyc. So: **read the native arms as a
control on every run, and only compare microcode numbers across runs whose
controls agree.** The 1.17 cyc ADC regression and the 5.72 cyc schedule
regression are both trustworthy because their runs' controls matched to
0.03%; a 1 cyc claim from a run like the fourth would not be.

## 10. Context the paper needs regardless of how this lands

- **The load-order fix is already in and is the largest single win so far.**
  `FE_SQ`'s five loads ran limb 4 -> limb 0 while its stores ran 0 -> 4, so in a
  dependent chain the first load of one op read the address the last store of the
  previous op wrote **one instruction earlier**. That store-to-load distance cost
  ~30 cycles. Ascending loads: **fe_sq 122.3 -> 81.8, patch untouched.** Fixed in
  `FE_SQ` and in `fe_sq_ucode`; the comment above `FE_SQ` explains why it must
  not be "tidied" back.
- **Table K2's dispatch floor was measuring the caller's memory ordering**, not
  the microcode mechanism. The old 45.6 fe_sq floor and its "37.3% wrapper share"
  were that stall. Report ~16 cycles for both, and say the floor is a property of
  the calling convention plus the mechanism.
- **The two kernels do not have different invocation costs.** 15.7 vs 16.2 is
  exactly one caller instruction; adding any single instruction to the fe_mul
  wrapper reproduces the fe_sq floor.
- **OpenSSL still wins, so the claim changes, not the benchmark.** The
  measured, defensible sentence is: *within 6% of the best hand-written
  assembly on fe_mul (102.8 vs 97.1) and 12% on fe_sq, while beating every
  compiler-generated and formally-verified backend - 22% faster than
  fiat-crypto on fe_mul, 30% on fe_sq, and faster than s2n-bignum's verified
  assembly.* "Fastest" does not survive review, because a reviewer can run
  OpenSSL.
- **The negative results are publishable and cheap to state.** Three
  census-based cost models were fitted and refuted in one day: `r` as a
  function of exposed parallelism (depth fell 2.95x, `r` fell 13%),
  cost-per-triad (predicted 77.8, measured 87.8), and by implication any
  projection from op/triad/depth counts alone. The decisive datum is two
  schedules with an IDENTICAL census 5.7 cyc apart. For a paper about
  whether microcode is a usable optimisation target, "the census does not
  determine the speed, and the schedule is worth 6%" is a more useful
  finding than another 4 triads.
- The mechanism findings are the durable contribution and are independent of the
  leaderboard: the store-to-load stall, the micro-op latency
  table, MUL at 0.91 cyc against 0.153 for an ALU op, the 128-triad ceiling, the
  carry-flag absence costing 20-23% of every patch, and the finding that patch
  bodies are dependency bound at ~2.8x their independent issue rate.

---

## 11. Kickoff for a fresh session

Paste this as the first message of a new chat:

> Read `curve25519/PLAN_kernel_optimization.md` in full, then implement section
> 5.0: rewrite the `fe_mul` microcode patch in `full_curve25519_inline2.c` to use
> five independent 128-bit accumulators with per-accumulator SETCC carry chains
> and a balanced-tree lazy reduction, following OpenSSL's `fe51_mul` structure.
> Do not change the wrapper's memory-op ordering. Validate with
> `python3 lib/ucode_sim.py full_curve25519_inline2.c mul_patch mul` before
> anything touches hardware, and check depth with `lib/ucode_critpath.py`. Then
> give me the commands to run.

Then, in order:

1. ~~**fe_mul restructure**, simulator-verified~~ **DONE offline, see 5.0.**
   Still to do: `bench_kernel` on hardware, then compute the achieved
   `r = (body - 29*0.91) / 145` and report it. That number decides
   everything downstream.
2. **fe_sq restructure**, same shape, three accumulators if registers are tight.
3. **Section 5.1 op-count work**, only on whichever kernel still needs it.
4. Re-run `probe_issue` to confirm `r` moved as expected.
5. Regenerate the paper tables: `bench_kernel` then
   `python3 lib/gen_kernel_table.py`, and the end-to-end sweep.

Hard rules for that session:

- **Never fire an unverified opcode encoding.** 0x957 killed the machine (§3).
- **Simulator before hardware, every time.** `lib/ucode_sim.py` reproduces both
  production patches exactly; a candidate that fails it is wrong.
- **Every new timing probe needs an observable output and a swept parameter.**
  See O3 for what happens without them.
- **Patch RAM:** fe_mul at U7c00, fe_sq after it; 128 triads total; call the
  lib-micro helpers BEFORE `patch_ucode` to reclaim U7de0-U7df0.
- **RFC 7748 gates every reported number.** Both benchmark binaries already do
  this; never quote a run whose gate failed.
