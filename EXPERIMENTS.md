# Sabrina Experiments Summary

Intel Goldmont microcode patching experiments using lib-micro infrastructure.
Hook mechanism: match-and-patch redirects x86 instructions (RDRAND, VMWRITE, PAUSE, etc.) to custom ucode in patch RAM.

---

## experiments/ — Early Exploration

### Instruction Hooking
| File | Hook Point | What it does |
|------|-----------|--------------|
| `rdrand.c` | RDRAND 0x0428 | Basic hook: sets TMP1=0xcafe, RBX=0xbead, RAX=TMP1 |
| `rdseed.c` | RDSEED 0x0430 | Same pattern as rdrand, confirms RDSEED hookable |
| `rdtscp.c` | RDTSCP 0x0788 | Same pattern, confirms RDTSCP hookable |
| `vmwrite.c` | HLT 0x0820 | Hook HLT instruction, outputs via RAX |
| `cpuid.c` | CPUID | Loads TMP1=0x2345, RBX=0xaead, outputs via RAX |
| `pause_mul.c` | PAUSE 0x0bf0 | Hooks PAUSE to do multiply. Latches RAX/RDX to TMP regs |

### Basic ALU Operations
| File | What it tests | Result |
|------|--------------|--------|
| `addshl.c` | ADD immediate + SHL via rdrand hook | Works: add 0x10 to RAX, shift left by 4 |
| `new_addshl.c` | Basic MOV: RAX<-RBX, RDX<-RCX | Works |
| `macrotest.c` | OR_DSZ64_DRM with immediates 0-127 | Verifies immediate operand encoding works |
| `cmpxchg.c` | Hook CMPXCHG, do MUL TMP1*RDX | Works, minimal probe |
| `multest.c` | RAX*RBX via vmwrite hook | Works |
| `mul64.c` | 64-bit multiply with R64SRC/R64DST macros | Works |
| `mul_imm_reg.c` | MUL_DSZ32 with immediate + reg*reg | 32-bit multiply with immediate works |
| `mulxchg.c` | MOV RAX->TMP1, MUL TMP1*RCX | Works |
| `imultest.c` | IMUL via vmwrite hook | Works |

### Flag Architecture Investigation
**`flag_probes.c`** (~700 lines, 9 probes) — Early exploration (conclusions later revised by `mac/flag_test.c`):
- ADD_DSZ64 to architectural regs (RAX) does NOT update readable flags
- GENARITHFLAGS_RR, READAFLAGS don't work as expected in isolation
- SETCC_CONDB reads stale CF=0; CMOVCC always fires (CF=1); UJMPCC never jumps
- **Initial conclusion:** ALU flags live in a separate domain. No bridge found.
- **Revised (see Flag Architecture below):** The real issue was ADD destination register — ADD→TMP works, ADD→RAX doesn't.

### MAC128 Development (Multiply-Accumulate 128-bit)
Core operation: `acc += a * b` where acc/result are 128-bit.

| File | Triads | Carry method | Key finding |
|------|--------|-------------|-------------|
| `mac128.c` | 3 | None (no carry) | First attempt. Documented MUL_DSZ64_DRR: dst=HIGH, TMP1=HIGH(!) |
| `mac128_v2.c` | — | — | 5 probes hunting where MUL low 64 bits land. Scans RCX, RDX, TMP2, TMP3, URAM |
| `mac128_v3.c` | 4 | CMOVCC | SETCC failed, tried CMOVCC. 8 test cases |
| `mac128_final.c` | 4 | SETCC | Confirmed: MUL dst=HIGH, src1 clobbered with LOW. 8 comprehensive tests |
| `mac128_flagfree.c` | 6 (10 uops) | Arithmetic: `carry = SHR((a&b)\|(\~s&(a\|b)), 63)` | **Flag-free solution.** 10 tests including double overflow |
| `mac128_nomovs.c` | 6 | Arithmetic (same) | Via VMWRITE hook 0x0cd8 instead of RDRAND. WMUL (2-triad) + MAC128 (6-triad) |
| `mac128_vmw.c` | 6 (10 uops) | Arithmetic | Confirmed: vmwrite hooks 0x0cd8, MUL clobbers both RCX(low) and RDX(high). 3-MAC chain test |

### Seqword & Path Experiments
| File | What it tests | Result |
|------|--------------|--------|
| `rdrand_final.c` | SEQW_GOTO variants with/without CRC parity | Tests 6 variants, uses LDAT readback to verify patch RAM writes |
| `rdrand_path1.c` | Direct ROM shadow at 0x0428 vs hook overhead | Hook = ~950K cycles overhead from match-and-patch scanner. Direct patch_ucode(0x0428) tested |
| `test_udbg.c` | LDAT access to microsequencer | ldat_array_read(0x6a0) works, MS accessible |

---

## mac/ — MAC128 Batching & Curve25519

Goal: amortize hook redirect overhead (~5 cycles/vmwrite) by batching multiple MACs per vmwrite call.

| File | MACs/call | Triads | Notes |
|------|----------|--------|-------|
| `hook.c` | 1 | 6 | Reference single MAC128 (RCX*RDX -> RAX:R8). Can exec CryptOpt after install |
| `mac128_nomovs.c` | 1 | 4 | **SETCC carry (ADD→TMP0).** 4 triads, 6 real ops. See Flag Architecture below |
| `mac128_flags.c` | 1 | 4 | GENARITHFLAGS_RR + SETCC_CONDB attempt (broken — GENARITHFLAGS doesn't generate flags) |
| `flag_test.c` | — | — | **Definitive flag test.** 7 groups, 30+ probes. Discovered ADD→TMP constraint |
| `mac2.c` | 2 | 13 | 2-MAC: RCX/RDX + R9/R10. Cross-MAC carry propagation tested |
| `mac3.c` | 3 | 20 | 3-MAC: RCX/RDX + R9/R10 + R11/R12. For CryptOpt benchmarking |
| `mac3_curve25519.c` | 3 | 19 | Overlapped 3-MAC with dynamic operand shuffling. ~120 cycles for 5 calls |
| `five_macs_regs.c` | 5 | 23 | 5 limbs (RSI/RDI/R9/R10/R11) * scalar (RBX). 4 triads/transition (overlapped) |
| `mac_ldzx.c` | 5 | 29 | Uses LDZX memory loads, takes array ptr in RCX. Requires -DSEG=N, -no-pie |

### Curve25519 Field Square
| File | Approach | Perf |
|------|---------|------|
| `curve_twohooks.c` | 2-phase: Phase1 (9 MACs out[0..2]), Phase2 (6 MACs out[3..4] + reduction) | 8 test vectors including libsodium basepoint |
| `bench_twohooks.c` | Timing: redirect ~75 cycles + MAC body ~45 cycles | ~135 cycles total expected |
| `bench_mac128.c` | Single MAC128 vs native MUL+ADC | ~950K cycles from scanner overhead; per-MAC native 3-4 cycles |
| `bench_mac3_curve25519.c` | MAC3 vs native __uint128_t | ~130 cycles MAC3 vs 142 native |
| `bench_macfive.c` | 5xMAC128 single vmwrite vs 5x native | Tests register-only and memory-loaded (L1 array) variants |

---

## mul/ — Multiply Benchmarks

| File | What it does | Result |
|------|-------------|--------|
| `stdmul.c` | Baseline: native `mul %%rbx` timing (100 iters) | Reference cycle count for native MUL |
| `multest.c` | 5x multiply in single vmwrite (11 triads) | Tests 5*5=25, max*max, Curve25519 limbs |
| `mul_bench.c` | 1 vmwrite (5xMUL) vs 5 native MULs | 1000 ops/batch, 100 reps. Min + avg cycles |
| `rdrand_test.c` | VMWRITE timing (100 iters, RCX=5 RDX=5) | Cycles per vmwrite call |
| `rdrand_test2.c` | RDRAND timing (1000 iters) | Cycles per RDRAND call |
| `five_mul_bench.c` | Trivial 3-NOP patch at vmwrite hook | Smoke test for hook install |
| `empty_test.c` | Bare RDRAND, print registers | No patching, baseline |
| `rdrand.c` | Inline RDRAND with variable capture | Minimal harness |

---

## tests/ — Carry Optimization & Final Benchmarks

### SETCC/Flag Forwarding Investigation
| File | Finding |
|------|---------|
| `test_setcc_repeated.c` | END_SEQWORD restores pre-hook EFLAGS → stale CF on 2nd+ vmwrite calls. Compares 4-triad SETCC vs 6-triad bit-manip |
| `flags.c` | Extended flag probing (strategies A-E: ADD+SETCC, GENARITHFLAGS+SETCC, READAFLAGS) |
| `test_intradata.c` | Intra-triad data forwarding: can slot 1 read slot 0's write in same triad? Includes 5-triad MAC if yes |

### MAC Triad Reduction Iterations
| File | Triads | Method | Key finding |
|------|--------|--------|-------------|
| `bench_mac3t.c` | 3 | Intra-triad SETCC_CONDB | ADD(→RAX)+SETCC same triad. Appeared to work but only tested with non-overflowing Curve25519 limbs |
| `bench_mac3t_v2.c` | 4 | SETCC with NOP constraint | Slot 2 NOP constraint discovered. Still used ADD→RAX (broken for actual carries) |
| `bench_mac3t_v3.c` | 4 | Gap requirements | 7 tests isolating MUL interaction. MUL found innocent; real culprit not yet identified |
| `bench_mac3t_v4.c` | 4 | GENARITHFLAGS+SETCC | Attempted GENARITHFLAGS fix — doesn't work (GENARITHFLAGS doesn't generate flags) |
| `bench_mac3t_v5.c` | 5 | Intra-triad forwarding test | Merged SHR+ADD in same triad T4 vs split across T4+T5 |

### Multi-MAC & Hybrid Strategies
| File | Strategy | Expected perf |
|------|---------|---------------|
| `test_2mac_prereqs.c` | Can we read R9/R10/R11 in ucode? Can MUL see rewritten RCX/RDX? | All yes. 2-MAC feasible |
| `bench_2mac.c` | 2 MACs/vmwrite (11 triads). 15->10 calls | ~85-95 cycles vs ~105 |
| `bench_hybrid.c` | 2 native mul+adc + 1 vmwrite MAC per limb | ~65-75 cycles (10 native muls ~30c + 5 vmwrite ~35c) |
| `bench_sq_compare.c` | C ref vs CryptOpt asm vs MAC128 | p10/p90 percentiles, SIGILL guard for BMI2 |

---

## v2/ — LDZX/STAD Memory Ops & Full Carry Square

### Memory Instruction Probing
**`test_memops.c`** — 6 probes for LDZX and STAD:
- Segment sweep, offset loading, basic stores, stores with offsets
- Round-trip load-add-store, load-multiply pattern
- **Key: ASZ32 only, requires MAP_32BIT mmap for low-4GB buffers**

### Patch RAM Capacity
**`test_single_limb.c` / `test_parts.c` / `test_test.c`** — Probe patch RAM size:
- Up to 500 triads tested, reduced to 32 for safety in hardened version
- `test_test.c` is crash-hardened: static globals, address validation, mfence barriers

### Single Limb in Microcode
**`test_single_limb.c`** — Proof of concept:
- Computes Curve25519 limb 0 entirely in microcode using LDZX/STAD/MAC128
- `limb0 = a[0]^2 + a[1]*(a[4]*38) + a[2]*(a[3]*38)`
- 30 triads for limb 0, 5 test vectors validated

### Full Carry Square Benchmark
**`bench.c`** — Final benchmark:
- C carry_square (gcc -O3) vs MAC128 microcode version
- 5000 warmup calls, 10001 samples, 100 iters/measurement
- Reports p5/p25/p50/p75/p95 percentiles + speedup ratio
- 8 test vectors including edge cases

---

## Flag Architecture on Goldmont (Definitive)

Tested by `mac/flag_test.c` — 7 groups, 30+ probes covering every flag mechanism.

### The TMP Register Rule

**ADD/SUB must write to a TMP register (TMP0–TMP5) for SETCC to detect carry.** Writing to an architectural register (RAX, RCX, RDX, R8, etc.) produces correct arithmetic results but SETCC silently reads CF=0.

```
ADD_DSZ64_DRR(TMP0, TMP3, RCX)  →  SETCC_CONDB reads CF correctly  ✓
ADD_DSZ64_DRR(RAX,  TMP3, RCX)  →  SETCC_CONDB always returns 0    ✗
```

This was the root cause of every prior SETCC failure in the MAC128 experiments. Every attempt (bench_mac3t, bench_mac3t_v2, mac128_flags) used `ADD→RAX` and appeared to work only because Curve25519 51-bit limbs never trigger a 64-bit carry.

### Three Flag Domains

Goldmont microcode has three independent flag domains:

| Domain | Written by | Read by | Behavior |
|--------|-----------|---------|----------|
| **Internal ALU** | ADD/SUB → TMP regs | SETCC (all 16 conditions) | Works intra-triad and cross-triad. Only path for carry detection. |
| **Architectural EFLAGS** | GENARITHFLAGS (copies from internal) | READAFLAGS | Restored by END_SEQWORD. Not updated by ADD alone. |
| **Conditional execution** | Unknown | CMOVCC, SELECTCC, UJMPCC | CF=1 stuck. CMOVCC/SELECTCC always fire. UJMPCC never jumps on CONDB. Unusable. |

### What Works, What Doesn't

| Mechanism | Status | Notes |
|-----------|--------|-------|
| ADD→TMP + SETCC_CONDB | **Works** | All 16 condition codes correct. Intra-triad and cross-triad. |
| ADD→RAX + SETCC_CONDB | **Broken** | Architectural reg writes don't forward to internal ALU flag domain |
| GENARITHFLAGS_RR alone | **Broken** | Doesn't generate flags from operands — only copies existing internal flags to architectural EFLAGS |
| GENARITHFLAGS_RR after ADD→TMP | Unnecessary | ADD→TMP already makes SETCC work. GENARITHFLAGS only needed if you want READAFLAGS to reflect the flags |
| MUL before ADD+SETCC | **Safe** | MUL does NOT poison the internal flag domain |
| MOVEINSERTFLGS_DRR | **Works** | Acts as MOV(dst, src1) + sets internal flags. Alternative flag source. |
| MOVEMERGEFLGS_DRR | **Broken** | Acts as MOV(dst, src0), does not set flags |
| CMOVCC / SELECTCC / UJMPCC | **Broken** | Read from conditional execution domain (CF=1 stuck) |
| READAFLAGS | Works | But reads architectural EFLAGS (0x6 baseline), not internal ALU flags |

### Working 4-Triad MAC128

```
T0: ZEROEXT TMP3, RAX         | MUL RCX×RDX → RCX:RDX       | NOP
T1: ADD TMP0, TMP3, RCX       | SETCC_CONDB TMP1, TMP0       | NOP        ← ADD→TMP0!
T2: ZEROEXT RAX, TMP0         | ADD R8, R8, RDX              | NOP
T3: ADD R8, R8, TMP1          | NOP                          | END
```

Key: the ADD in T1 writes to **TMP0** (not RAX), so SETCC correctly reads CF. The result is then copied to RAX via ZEROEXT in T2.

### Corrected History

The flag investigation went through several wrong turns before finding the real constraint:

1. `flag_probes.c` — Tested ADD→RAX + SETCC → failed. Concluded "flags unreachable." **Wrong: should have tested ADD→TMP.**
2. `bench_mac3t.c` — ADD→RAX + SETCC appeared to work for Curve25519. **Misleading: 51-bit limbs never overflow 64 bits, so carry was never tested.**
3. `bench_mac3t_v3.c` — Blamed MUL for poisoning flags. **Wrong: MUL is innocent.**
4. `bench_mac3t_v4.c` — Tried GENARITHFLAGS as fix. **Wrong: GENARITHFLAGS doesn't generate flags.**
5. `mac/flag_test.c` — Systematic 7-group test. Discovered the real constraint: **ADD destination must be a TMP register.**

---

## Key Findings (Chronological)

1. **Hookable instructions:** RDRAND, RDSEED, RDTSCP, VMWRITE, CPUID, CMPXCHG, PAUSE, HLT all confirmed hookable
2. **MUL_DSZ64_DRR behavior:** dst=HIGH 64 bits, src1 clobbered with LOW 64 bits, src0 unchanged
3. ~~**Flag isolation on Goldmont:**~~ **Revised.** ALU flags ARE reachable by SETCC — but only when ADD writes to TMP registers, not architectural registers. See Flag Architecture section.
4. ~~**EFLAGS restore bug:**~~ Partially valid. END_SEQWORD restores architectural EFLAGS, but SETCC reads from the internal domain which is not affected. The real bug was ADD→RAX.
5. ~~**Intra-triad SETCC works:**~~ SETCC works both intra-triad and cross-triad. The constraint is ADD→TMP, not triad placement.
6. **MAC128 evolution:** 6 triads (flag-free) → **4 triads (ADD→TMP + SETCC)**. The intermediate "3-triad" and "GENARITHFLAGS" attempts were based on misdiagnosis.
7. **Batching wins:** 5-MAC/vmwrite (23 triads) amortizes redirect overhead; hybrid (native+vmwrite) best at ~65-75 cycles
8. **Memory ops (LDZX/STAD):** Work but ASZ32-only, need low-4GB buffers
9. **Patch RAM:** At least 32 triads safe, up to 500 probed
10. **Match-and-patch scanner cost:** ~950K cycles. Direct ROM shadow avoids this but limited support below 0x7c00
