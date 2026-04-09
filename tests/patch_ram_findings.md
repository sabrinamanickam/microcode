# Patch RAM Capacity Findings

## Summary

We need 55 contiguous-equivalent triads for a microcode sequence. Testing reveals only **50 usable triads** across all free regions (43 payload after link overhead). The bottleneck is **seqword address space**, not uop slot availability.

---

## Architecture: Why "Free Slots" ≠ "Usable Triads"

### Patch RAM Layout

- **Uop storage**: 0x200 slots (addresses U7C00–U7DFF), each holds one micro-op.
- **Seqword storage**: Separate address space, **128 entries per bank** (0x00–0x7F), with 4 banks selected by `base % 4`.
- One triad = 3 uops + 1 seqword = 4 uop slots + 1 seqword slot.

### The Seqword Bottleneck

`patch_ucode()` computes seqword addresses as:

```c
base = addr - 0x7c00;
seq_addr = ((base % 4) * 0x80 + (base / 4)) % 0x80;
// Then writes seqwords at seq_addr + 0, seq_addr + 1, ..., seq_addr + (n-1)
```

This means:
- Each starting address maps to a **fixed seqword bank** (determined by `base % 4`).
- Within that bank, you get `0x80 - seq_start` entries before wrapping to address 0x00.
- **Wrapping corrupts seqwords belonging to other patches in the same bank.**
- Existing patches from slots 1–18 already occupy seqword entries, reducing what's available.

### Theoretical vs Actual Capacity

For the largest free region (U7d7c, 133 free uop slots):
- Theoretical seqword limit: `0x80 - 0x5f = 33 triads`
- **Actual tested limit: 25 triads** (other patches consume 8 seqword entries in that bank)

---

## Current State

### Match & Patch Table (18 of 32 slots active)

```
Slot  Dst     Src     Enabled
00    U0000   U0000   0        (unused)
01    U4dc0   U7c4c   1
02    U2078   U7c0e   1
03    U682a   U7c86   1
04    U1c3c   U7c30   1
05    U6a10   U7c44   1
06    U3c7a   U7c22   1
07    U4f52   U7cca   1
08    U01d6   U7c6a   1
09    U2e44   U7cbe   1
10    U70fa   U7c9e   1
11    U13c2   U7cea   1
12    U67a0   U7c6e   1
13    U0cd2   U7c82   1
14    U209c   U28d8   1
15    U141e   U7c96   1
16    U24bc   U7c8a   1
17    U623a   U7d16   1
18    U0cd8   U7c00   1
19–30 U0000   U0000   0        (free)
31    U58ba   U017a   1        (IN fix — always needed)
```

### Free Uop Regions (from dump)

| Region | Start  | Dump Slots | 4-Aligned Start | Usable Triads (tested) |
|--------|--------|------------|-----------------|----------------------|
| 0      | U7c47  | 24         | U7c48           | 5                    |
| 1      | U7c6b  | 13         | U7c6c           | 3                    |
| 2      | U7c98  | 6          | U7c98           | 1 (too small)        |
| 3      | U7cc7  | 24         | U7cc8           | 5                    |
| 4      | U7cea  | 14         | U7cec           | 3                    |
| 5      | U7d46  | 29         | U7d48           | 6                    |
| 6      | U7d6a  | 14         | U7d6c           | 3                    |
| 7      | U7d7c  | 133        | U7d7c           | 25                   |
| **Total** |     |            |                 | **50 triads**        |

After subtracting 1 link triad per fragment boundary: **~43 payload triads**. Not enough for 55.

---

## Tested & Confirmed Facts

1. **Multiple `patch_ucode()` calls coexist** — installing fragment B then fragment A works, both stay resident.
2. **`SEQ_GOTO0` works across non-contiguous patch RAM regions** — tested with markers, all 8 runs passed.
3. **TMP registers survive cross-region jumps** — accumulator in TMP0 preserved across a GOTO to a different region.
4. **Link cost: 1 triad per fragment boundary** — `{NOP, NOP, NOP, SEQ_GOTO0(next_region)}`.
5. **Seqword space is the real limiter** — not uop slot availability.

---

## Options to Fit 55 Triads

### Option A: Free Existing Patches (Recommended)

Clear unused match-and-patch entries from slots 1–18. Each cleared patch frees:
- Its uop slots (enlarging free regions or creating new ones)
- Its seqword entries (increasing usable triad depth in nearby regions)

**Action needed**: Identify which of slots 1–18 are still needed. Clearing ~4–6 patches near the U7d7c region would likely give enough seqword headroom for all 55 triads in one contiguous block.

Key question: which patches (slots 1–18) are required for your current work?

### Option B: Optimize the Microcode Sequence

Reduce the 55 triads. Possible approaches:
- Pack more work into each triad (use all 3 uop slots effectively)
- Eliminate redundant moves or temporaries
- Use branchless patterns to merge conditional paths

### Option C: Two-Phase Execution

Split the 55-triad sequence into two logical phases that share state via staging buffer or URAM:
- Phase 1: First 24 payload triads, writes intermediate state to staging buffer
- Phase 2: Triggered separately, reads state, runs remaining 31 triads
- Downside: Requires two hooks and careful state management

### Option D: Reset and Reallocate Everything

Call `init_match_and_patch()` to wipe all patches, then allocate from a clean slate. The full patch RAM (0x200 uop slots = 128 triads in bank 0 alone) is more than enough. Then reinstall only the patches you actually need plus your 55-triad sequence.

**This is the nuclear option** — you lose all existing hooks and must rebuild them.

---

## Quick Reference

```
Seqword capacity from address:
  base = addr - 0x7c00
  bank = base % 4
  seq_start = (base / 4) % 0x80
  max_triads_before_wrap = 0x80 - seq_start

  But actual usable < max_triads_before_wrap due to
  other patches occupying seqword entries in the same bank.

Link triad template:
  { NOP, NOP, NOP, SEQ_GOTO0(next_region_addr) }

Cross-region jump requirements:
  - Target must be 4-aligned (multiple of 4)
  - GOTO is encoded in seqword, not uop
  - TMP registers preserved across jump
```
