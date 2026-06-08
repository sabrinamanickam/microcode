# Keccak-f[1600] in Goldmont microcode — design walkthrough

The most-optimized implementation is the `D_IN_REGS=True` path in `keccak_gen.py`,
which emits `keccak_perm_body.h` (108 triads). It runs the full 24-round
permutation in **874 cyc/perm vs 942 for the fastest SUPERCOP scalar
(`x86_64_asm`) = ~7.2% win**, measured back-to-back in one process
(`asm_op_keccak_vs.c`).

This document goes in two passes: first **why the architecture is shaped the way
it is** (each decision and what forced it), then a **line-by-line read of the
generated round** in execution order, then **how each design was reached**.

---

## Part 1 — The five decisions that define the design

### Decision 1: Loop all 24 rounds inside a single `vmwrite`

The naïve approach is one `vmwrite` per round. But each firing of the hook has a
fixed dispatch/trigger tax (~80–120 cyc of irreducible firing overhead; CLAUDE.md
notes ~7 cyc just for the call). 24 firings would pay that tax 24 times.

More importantly, TMP registers **don't persist across `vmwrite` calls**
(CLAUDE.md lessons #2/#3), and neither do arch regs reliably — so a per-round
firing is forced to round-trip all 25 lanes through memory *every round*. That
memory traffic, not the arithmetic, is what would sink us.

So the entire permutation runs in **one firing**: load the 25 lanes once at the
top, do 24 rounds with state resident in registers, store once at the bottom. The
25 loads + 25 stores (50 memory triads) are paid **once per perm instead of once
per round** — this is the whole reason we can compete with SUPERCOP, whose
hand-tuned asm achieves the same register residency by fully unrolling.

This is what `gen_prologue` / `gen_body` (looped) / `gen_epilogue` encode. The
loop is real (a backward branch), not unrolled, because 24× unrolling wouldn't fit
the **128-triad patch RAM limit**.

### Decision 2: π is a permutation, so a single looped body suffices

A worry with looping: π (pi) permutes lane positions every round. If we held lanes
in fixed registers, after one round the "lane at register RDI" would be a
different logical lane, and the next round's θ would read the wrong things.

The key realization: π on Keccak's 25 lanes is a set of **cycles** (the (0,0) lane
is fixed; the other 24 lanes form cycles). `pi_cycles()` enumerates them. Because
we apply π **in place along its cycles** — walking each cycle and moving
`cyc[k-1] → cyc[k]` with one save-temp for the cycle head — the lanes end up back
in their canonical register slots at the end of every round. So round *r+1* sees
exactly the layout round *r* did. That's what makes a single, identical, looped
body correct for all 24 rounds. No per-round register renaming needed.

### Decision 3: Generator-first, not hand-written triads

We never hand-write 108 triads. `keccak_gen.py`:
1. Emits an **op IR** (`LD/ST/LDX/XOR/ROL/NOTAND/MOV/ADDI/LOOPTEST`).
2. **Simulates** that IR on a register+memory model (`simulate_perm`) that *also
   models the hardware constraints* — crucially the **8-bit signed offset**
   (`at(off)` asserts −128..127) — and asserts the result equals
   `keccak_perm_ref` for 64/64 vectors.
3. Only then **lowers** the IR to `ucode_t` triads.

This is why we caught bugs in software (the signed-offset truncation that on
hardware had crashed the NUC) instead of on the box. Every design change is
validated in the simulator first.

### Decision 4: The hardware facts that constrain every op

Each established with a dedicated probe; they dictate the code shape:

- **Register file is exactly 32**: GPR `0x20–0x2f`, TMP `0x30–0x3f`. Keccak's 25
  lanes + scratch is tight.
- **Offset field is 8-bit *signed*** (−128..+127 bytes = −16..+15 lanes). This is
  why `BASE_LANE=16`: the base register `RCX` points at the *middle* of the
  buffer (`OFF(lane)=(lane-16)*8`), so the 25 state lanes, the D-scratch, counter,
  and RC table all land within ±127 bytes. Lanes too far away must use the
  **index-register form** instead. (`probe_offset.c`)
- **≤1 memory op per triad** — more than one hangs the machine. So loads/stores
  can't be packed with each other.
- **`MOVE_DSZ64` traps to a ~800k-cyc slow path** — banned. Register moves use
  `ZEROEXT_DSZ64_DR`; immediates use `ZEROEXT_DSZ32_DI`.
- **`SEG_DS` (0x18)** is the working segment; wrong segment crashed the box.
  (`probe_seg.c`)
- **Intra-triad execution is fully sequential** — a 3-deep RAW chain within one
  triad works, and a store forwards to a later load in the same patch (STLF).
  (`probe_triad.c`)
- **Loop primitive**: an `XOR` sets ZF *in the same triad* as the `UJMPCC`, and a
  backward `UJMPCC_...CONDNZ` branches on arch flags (it ignores its register
  operand). SUB-immediate is reversed and ADD(−1) doesn't decrement, so we
  **count up**. (`probe_loop.c` / `ujmp_test.c`)

### Decision 5: RSP as a 32nd register — the move that unlocked the win

The biggest single structural win was getting θ's **D[] values to live in
registers** instead of being written to the buffer and re-read once per lane
during the apply phase. Per-lane D-loads were 25 extra memory triads per round.

But 25 lanes already consume all of GPR+TMP except a tiny bit of scratch — there
was no room for 5 resident D values *plus* a save-temp for the π cycle walk. We
needed one more register.

`probe_rsp.c` proved that **RSP is usable as a 32nd data register** if we save it
at entry and restore it before the firing returns (our microcode never touches the
stack). That's the `RSPSAVE` lane: `gen_prologue` does `ST RSP→OFF(RSPSAVE)` and
`gen_epilogue` does `LD RSP←OFF(RSPSAVE)`, **once per perm**. Inside the round, RSP
is free scratch.

Critically: earlier attempts to free a register by *rebuilding the base register
RCX* each round **lost** (2166, 2074 cyc — the rebuild cost more than the D-memory
it saved). RSP wins because it leaves RCX permanently pointing at the base — **no
rebuild**.

---

## Part 2 — Reading the generated round in execution order

Buffer layout (from `keccak_gen.py`, surfaced in `keccak_perm.h`):

```
lanes  0..24  : state          (resident in 13 GPR + 12 TMP during the round)
       25..29 : DSCR  — θ-C parity scratch (5 lanes)
       30     : COUNTER — holds the RC BYTE-INDEX (not a round number)
       31     : RSPSAVE — saved stack pointer
       32..55 : RC[0..23] table
RCX = &buf[16]   (BASE_LANE=16, so OFF(lane) = (lane-16)*8 fits signed-8)
```

### Prologue — `gen_prologue` (26 triads)

```
{ STAD_DSZ64_ASZ32_SC1_RRI(RSP, RCX, 120, SEG_DS), ... }   // save RSP -> buf[31]
{ LDZX_DSZ64_ASZ32_SC1_DRI(RDI, RCX, -128, SEG_DS), ... }  // lane0  -> RDI
{ LDZX_DSZ64_ASZ32_SC1_DRI(RSI, RCX, -120, SEG_DS), ... }  // lane1  -> RSI
... 25 loads total, lane i -> REG[i]
```

`REG = [RDI,RSI,RBX,RDX,RBP,R8..R15] + TMP0..TMP11` — exactly 25 names. After
this, all state is in registers and we never touch lanes 0..24 again until the
epilogue. Offset 120 = `OFF(31)` = `(31-16)*8`; offset −128 = `OFF(0)` =
`(0-16)*8`. Both inside ±127 — that's the payoff of centering the base at lane 16.

### θ-C — balanced XOR tree → buffer (`gen_body` lines 142–147)

For each column x, `C[x] = a^b^c^d^e` of the five lanes in that column. The obvious
encoding is a depth-4 left chain. Instead we emit a **balanced tree of depth 3**:

```
XOR RAX = REG[x]    ^ REG[x+5]    // a^b
XOR RSP = REG[x+10] ^ REG[x+15]   // c^d   (independent — runs parallel to above)
XOR RAX = RAX ^ RSP               // (a^b)^(c^d)
XOR RAX = RAX ^ REG[x+20]         // ^e    -> depth 3
ST  RAX -> OFF(DSCR+x)
```

Why this matters: **C is the head of the round's dependency chain** — every later
phase (D, apply, χ) waits on it. Shaving one level of latency off C shortens
*every* round. RSP is free here, so we use it as the second tree accumulator. C is
written to the **buffer** (DSCR lanes), not kept in registers, because we need the
registers for the resident D next and C is only read a few times immediately after.

### θ-D — into 5 registers, not the buffer (`gen_body` lines 150–154)

```
LD  RSP      <- C[(x+1)%5]    // from DSCR buffer
ROL RSP, 1
LD  D_REG[x] <- C[(x+4)%5]    // C[x-1]
XOR D_REG[x] = D_REG[x] ^ RSP // D[x] = C[x-1] ^ rol(C[x+1],1)
```

`D_REG = [RAX, TMP12, TMP13, TMP14, TMP15]` — the 5 D values now **live in
registers** for the rest of the round. RSP is the rotate temp. No in-place hazard
because C is read from memory while D is written to regs. This is the part that
RSP-as-32nd-register made affordable: without it, D had to go back to the buffer
and be re-loaded per lane (the +25 mem-triads/round earlier versions paid).

### θ-apply + ρ + π — in place, reading D from registers (`gen_body` lines 157–171)

Walk each π cycle. For a multi-lane cycle, save the head into RSP, then shift each
element to its π destination, folding in θ (`^ D_REG[col(src)]`) and ρ (the `ROL`
by `RHO[src]`) as we go:

```
MOV RSP = REG[cyc[L-1]]                       // save cycle head (ZEROEXT, not MOVE)
for k = L-1 .. 1:
    XOR REG[cyc[k]] = REG[cyc[k-1]] ^ D_REG[col(cyc[k-1])]
    ROL REG[cyc[k]] by RHO[cyc[k-1]]
XOR REG[cyc[0]] = RSP ^ D_REG[col(cyc[L-1])]  // close cycle from the saved head
ROL REG[cyc[0]] by RHO[cyc[L-1]]
```

Two things to notice. First, **D is read straight from `D_REG[...]`** — there is
*no per-lane load*, which is exactly the win. Second, `col(src)=src%5` selects the
right D column for each source lane. Because we move along the cycle, every lane
lands in its canonical register slot, so the layout is preserved for the next
round (Decision 2).

### χ — MOV-free (`gen_body` lines 209–216)

χ is `s[x] ^= ~s[x+1] & s[x+2]` across each row of 5. Done naïvely in place, the
first lanes you overwrite are still needed by later lanes in the row, so you're
forced into save-MOVs — native code, register-starved, pays ~2 MOVs/row = 10/round.
We have headroom (the 5 D regs are dead now), so we **compute all 5 NOTANDs of the
row first into the freed D registers, then do 5 in-place XORs**:

```
SCR5 = [RAX, TMP12, TMP13, TMP14, TMP15]   // the now-dead D regs
for x in 0..4:  NOTAND SCR5[x] = (~REG[r[x+1]]) & REG[r[x+2]]   // B still intact
for x in 0..4:  XOR    REG[r[x]] = REG[r[x]] ^ SCR5[x]
```

No save-MOVs at all. This is something native *can't* do because it has no spare
registers — our register residency directly buys this.

### ι — round constant via byte-indexed table (`gen_body` lines 230–232)

```
LD  TMP14 <- OFF(COUNTER)        // COUNTER holds a BYTE-INDEX, not a round number
LDX TMP13 <- mem[base + TMP14]   // index-register form: RC = table[counter]
XOR REG[0] = REG[0] ^ TMP13
```

The subtle optimization: the COUNTER lane stores the **byte offset of the current
RC directly** (initialized to `(RCTAB-BASE_LANE)*8` = 128, incremented by 8 each
round). So fetching RC is just `LD idx; LDX RC; XOR` — there is **no SHL/ADD on the
ι→RC→lane0 critical path**. We pay the `+=8` arithmetic in the loop-control phase
instead, off the critical path. The RC table lives at lanes 32..55, too far for
the signed-8 immediate offset, which is exactly why ι uses the **index-register
`LDX`** form.

### Loop control — `gen_loopctrl` (the branch)

```
ADDI TMP14 = TMP14 + 8           // advance the byte-index
ST   TMP14 -> OFF(COUNTER)       // persist it for next iteration
LOOPTEST(TMP12, TMP14, 320)
```

`LOOPTEST` lowers to a single forced triad:

```
{ XOR_DSZ64_DRI(TMP12, TMP14, 320), NOP,
  UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP12, 0x7c68), NOP_SEQWORD }
```

`320 = RC_END_IDX = (RCTAB-BASE_LANE)*8 + 24*8` — the byte-index just past RC[23].
The **XOR and the UJMPCC are in the same triad**: the XOR computes `idx ^ 320` and
sets ZF in that triad; the UJMPCC, evaluated right after in the same triad,
branches back to `loop_top = 0x7c68` (the address right after the 26-triad
prologue) while ZF is clear, i.e. as long as `idx != 320`. We count **up** to 320
because SUB-immediate is reversed and decrement-by-ADD doesn't work on this
microcode (Decision 4). When `idx == 320`, XOR yields 0, ZF set, branch not taken
→ fall through to the epilogue. 24 iterations, exactly.

### Epilogue — `gen_epilogue` (26 triads)

```
for i in 0..24:  ST REG[i] -> OFF(i)     // write the 25 lanes back
LD RSP <- OFF(RSPSAVE)                    // restore the stack pointer
```

Store the final state and restore RSP so the firing returns cleanly (what
`probe_rsp` verified is safe). Total: prologue 26 + (body+loopctrl) 56 + epilogue
26 = **108 triads**, under the 128 cap with room to spare.

---

## Part 3 — How each design was reached (the journey, with measured deltas)

The optimizations weren't designed up front; each was an empirical step, measured
**back-to-back in the same process** (because rdtsc runs at a constant ~1.1 GHz
while the core bursts to 2.4 GHz — cross-run cycle counts are meaningless, only
same-process ratios are valid; this is the entire reason `asm_op_keccak_vs.c`
interleaves the timing):

1. **Per-lane D, naïve χ with save-MOVs** → ratio **1.00×** (tie). Correct but not
   winning. The bottleneck looked like per-round D memory traffic.
2. **D resident in registers via RSP** (the 32nd-register trick, no RCX rebuild) →
   **0.99×**. Small, because we traded D-stores for keeping them live — but it
   freed the path to (3).
3. **MOV-free χ** using the now-dead D registers as NOTAND scratch → **0.95×**. The
   biggest single jump (~5%), the clearest "register headroom native lacks."
4. **θ balanced-XOR tree (depth 4→3) + RC byte-index** (no SHL/ADD on the ι
   critical path) → **0.93×**, the final ~7.2% win. Both attack the round's
   *critical path* directly: C is the head of the dependency chain, and ι feeds
   lane0.

We also confirmed we're racing the **right** target: `asm_op_keccak_vs.c`
benchmarks *all four* SUPERCOP scalar variants and reports the fastest.
`x86_64_asm` (Van Keer, ROL) = 942 is fastest; `opt64lcu24` = 979; the two SHLD
variants ≈ 4630 (SHLD is catastrophic on Goldmont/Atom). So 874 vs 942 is a win
against the genuinely fastest scalar implementation, not a strawman.

The honest ceiling: Keccak is θ→ρ→π→χ chain-bound, so each trick buys only a few
percent, and we can't unroll for more parallelism because of the 128-triad patch
RAM. The win comes entirely from **doing per-round work native can't**: full
register residency (no per-round I/O), a free 32nd register, and MOV-free χ — paid
for by loading/storing the state exactly once per permutation.

The single source of truth is `keccak_gen.py` (the `D_IN_REGS=True` branch);
everything in `keccak_perm_body.h` is mechanically lowered from it and
simulator-verified (64/64) before it ever touches the hardware, where it's
KAT-anchored at `0xF1258F7940E1DDE7`.
