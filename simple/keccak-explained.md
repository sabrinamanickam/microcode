# `asm_op_keccak.c` Explained — from basics to implementation, and what we learned about microcode

This document walks through the Keccak-f[1600] microcode work: what the program does, the chain of ideas behind it, how it's built, and the microcode lessons baked into it.

---

## 1. The basics: what this program actually does

On this Goldmont CPU, **microcode is patchable**. Each x86 instruction is internally cracked into "micro-ops" (uops); some instructions run a sequence of uops fetched from a small patch RAM. The lib-micro infrastructure lets us:

- **write our own uops** into patch RAM (`patch_ucode`), and
- **hook an x86 instruction** so that executing it jumps into our uops (`hook_match_and_patch`).

We hijack the `vmwrite` instruction (opcode `0x0cd8`). Normally meaningless in user code, but once hooked, every `vmwrite rcx, rcx` we execute runs *our* microcode instead. That's the bridge: C code on the "native" side sets up registers, executes `vmwrite`, and our microcode runs with full access to the physical register file and memory.

**Why Keccak?** It's the permutation inside SHA-3. We're trying to beat SUPERCOP's hand-tuned `x86_64_asm` Keccak (939 cyc/permutation on this chip). The bet: native x86 has only ~14 general registers, but Keccak's state is 25×64-bit lanes, so native code spills to memory constantly. Microcode gives us 16 GPRs **+ 16 TMP registers** = enough to hold the whole 25-lane state resident. That's a structural edge native code can't access.

---

## 2. What one Keccak round is

`keccak_round_ref()` is the spec, in plain C. The state is 25 lanes, indexed `s[5*y + x]`. One round is five steps:

- **θ (theta)** — column parity mixing. Compute `C[x]` = XOR of column `x` (5 lanes), then `D[x] = C[x-1] ⊕ rotate(C[x+1], 1)`, then XOR `D[x]` into every lane of column `x`.
- **ρ (rho)** — rotate each lane by a fixed per-lane amount (`RHO[]`).
- **π (pi)** — permute lane *positions*: lane `(x,y)` moves to `(y, 2x+3y mod 5)`.
- **χ (chi)** — the only nonlinear step, per row: `out[x] = B[x] ⊕ ((¬B[x+1]) ∧ B[x+2])`.
- **ι (iota)** — XOR a round constant into lane (0,0).

This C reference is the **ground truth** — the microcode must reproduce it exactly. A full permutation is 24 of these rounds with different round constants (`KECCAK_RC[]`).

---

## 3. The file's structure

`asm_op_keccak.c` is the **test harness**; it doesn't contain the microcode by hand. The flow:

| Part | Role |
|---|---|
| `keccak_round_ref` | C reference (ground truth) |
| `#include "keccak_round.h"` | generated `#define`s (triad count, buffer size, base lane) |
| `install_keccak_round_patch` | write the microcode to patch RAM + hook vmwrite |
| `keccak_round_ucode` | the C→microcode bridge (one `vmwrite`) |
| `verify_round` | run microcode, compare to reference, lane by lane |
| `bench_single_round` | rdtsc timing |

The actual microcode triads live in **`keccak_round_body.h`**, `#include`d *inside* a local array initializer in `install_keccak_round_patch`. That file is **machine-generated** by `keccak_gen.py` — which is the important architectural decision (see §5).

**The bridge** is worth reading closely:

```c
register uint64_t *_buf asm("rcx") = &g_keccak_buf[KECCAK_BASE_LANE];
asm volatile("vmwrite rcx, rcx\n\t" : "+r"(_buf) : : <every GPR>, "memory", "cc");
```

It pins the buffer pointer into `RCX`, fires the hooked `vmwrite`, and clobbers every GPR because the microcode trashes them all. The microcode reads/writes `g_keccak_buf` through `RCX`.

---

## 4. The microcode design (what runs inside the one vmwrite)

The 108 triads do, in order:

1. **Prologue** — 25 `LDZX` loads: pull the 25 lanes from `g_keccak_buf` into 13 GPRs + 12 TMP registers. Now the whole state is resident.
2. **θ parity** — compute `C[0..4]` into 5 scratch registers (`RAX, TMP12–15`).
3. **θ broadcast** — compute `D[x]` and **store to the buffer** (lanes 25–29). D goes to memory because there aren't enough free registers to hold state + C + D simultaneously.
4. **θ-apply + ρ + π fused, in place** — for each lane: load its `D[col]`, XOR it in, rotate. The π permutation is done by **following π's cycles and shuffling registers in place** — no separate copy of the state.
5. **χ** — per row, in place, using `NOTAND` for the `(¬B[x+1]) ∧ B[x+2]` term.
6. **ι** — XOR the round constant into lane 0.
7. **Epilogue** — 25 `STAD` stores: write the 25 lanes back to `g_keccak_buf`.

**Register layout** (canonical): lanes 0–24 ↦ `RDI, RSI, RBX, RDX, RBP, R8–R15` (13 GPRs) then `TMP0–TMP11` (12 TMPs). `RCX` is the buffer base. `RAX, TMP12–15` are scratch.

**Buffer layout**: 30 lanes total. `[0..24]` = state, `[25..29]` = D scratch. C lives in registers, not memory. The base pointer `RCX` is **centered** at lane 14 (`KECCAK_BASE_LANE`) so every load/store offset stays inside the hardware's narrow signed range (see §6).

**The single most important design realization** (the thing that makes looping easy): after the in-place π shuffle, each B value lands at its *canonical* register, so χ reads canonical positions and writes canonical output. **`REG[i]` holds lane `i` both before and after a round.** There's no positional drift between rounds — so we can just loop the same body 24× with no special handling. The feared "two alternating round bodies" scheme turned out to be unnecessary.

---

## 5. Why a generator instead of hand-written triads

The round body is ~46 dense triads where a single mis-placed lane is a silent wrong answer, and every hardware test risks crashing the NUC. So `keccak_gen.py`:

1. emits the round as an ordered **op-list** using real register names,
2. **simulates** that op-list on a model of the register file + memory (including the hardware's quirky signed-offset addressing),
3. **asserts the simulation matches the C reference for all 24 rounds**, and
4. only then emits the C triad array (`keccak_round.h` + `keccak_round_body.h`).

This moves correctness debugging into software (instant, safe) instead of onto the fragile hardware. It also handles **triad packing**: chunk the op-list 3 ops per triad, respecting the hardware rules below.

---

## 6. What we discovered about microcode (the expensive lessons)

Each of these cost a crash, a segfault, or a misdiagnosis — and is now encoded as a constraint in the generator:

- **Memory access needs the right segment.** `LDZX`/`STAD` only work with `SEG_DS` (0x18) on this box. `SEG=3` (copied from another test) makes the core hang so hard the watchdog can't fire — it crashed the whole NUC. The working segment is machine-specific.

- **`_DM` "memory" forms aren't arbitrary addressing.** `MOVE/ZEROEXT_DSZ64_DM` read a *field of the triggering instruction* (its RIP/immediate), not `[arbitrary address]`. General memory access is only `LDZX`/`STAD` with base-register + segment.

- **The offset field is 8-bit *signed*** (−128..+127 bytes = ±16 lanes). This was the killer bug: the first buffer put scratch at offsets ≥256, which silently wrapped, corrupting everything — and it was initially misdiagnosed as a "RAW dependency" failure. Fix: **center the base** (`RCX = &buf[14]`) and keep the buffer ≤32 lanes.

- **The immediate field is 16-bit.** You can't load a 32-bit address in one op; build it with `ZEROEXT` + `SHL` + `XOR` (the init code does the same).

- **Intra-triad execution is fully sequential** — even a 3-deep dependent chain in one triad works, and later slots see earlier slots' writes. That validates dense 3-op packing.

- **Store-to-load forwarding works within a patch** — so spilling D to the buffer and reloading it is safe.

- **At most one memory op per triad** — two `LDZX`/`STAD` in the same triad (single L1 port) hangs the core.

- **`ROL` and `NOTAND` work** — `ROL_DSZ64` is single-uop and correct for all 24 rotation amounts; `NOTAND(d,a,b) = (¬a)∧b` (BMI1 `ANDN` order), exactly χ's primitive. (The plan only *assumed* ROL existed; we verified it.)

- **A patch that faults skips cleanup**, leaving a stale hook that poisons the next run — always restore the match-and-patch table.

And the headline performance finding: **0.49 cycles per triad** for Keccak's op mix — about 2× better than assumed, which flipped the projection from "likely loss" to "likely ~20% win" once looped.

---

## 7. Status & next step

- **Phases 0–3 complete.** A correct single Keccak-f[1600] round runs in microcode and matches the C reference (random + zero-state vectors). 108 triads, measured at 53 cyc.
- **Phase 4 (next):** loop the round body 24× inside one `vmwrite` — prologue load once, 24× compute via backward `SEQ_GOTO` + round counter, round-constant lookup for ι, epilogue store once — then verify against full KAT vectors. The "π self-resolves" finding means no two-body complexity. Stretch goal: move D from the buffer into registers to lock in the win.

### Related files
- `asm_op_keccak.c` — the harness described here
- `keccak_gen.py` — self-verifying generator
- `keccak_round.h`, `keccak_round_body.h` — generated microcode
- `keccak-plan.md` — full phase-by-phase plan and results
- probes: `probe_seg.c`, `probe_rol.c`, `probe_stlf.c`, `probe_triad.c`, `probe_offset.c` — each established one hardware fact above
