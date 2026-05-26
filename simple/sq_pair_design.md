# sq-pair combined patch — design plan

Goal: collapse the ladder's two adjacent `fe_sq` calls into a single microcode
patch firing, saving ~27 cyc mode-switch overhead × 2 sq-pairs × 255 steps
≈ **15–25k cyc per X25519**. Closes ~half the gap vs amd64-64; not a full win
on its own.

## Background context

- Gap to close vs amd64-64: 40,132 cyc (ours/ucode 312,260 vs a64/asm 272,128).
- Empirical per-firing wrapper overhead (from fe_sq_ucode_n decomposition):
  ~41 cyc, of which ~14 cyc is accounted (precompute, xor, reg shuffle) and
  **~27 cyc is unaccounted ≈ microcode mode-switch entry/exit**.
- Patch RAM cap: 124 usable triads (128 − 4 staging at U7de0–U7df0).
- Current usage: fe_mul 66 triads + fe_sq 42 triads = 108 used.
- Available for sq-pair (replacing fe_sq): **58 triads + recover 42 = 58 free + 42 from fe_sq slot = 100 triads** if fe_mul stays.

## Design

In-patch loop using UJMPCC (verified working — see `Sabrina/tests/ujmp_test.c`).
ONE `vmread` enters microcode mode ONCE; the patch internally iterates twice.

### Wrapper signature
```c
void fe_sq_pair_ucode(const uint64_t *a, const uint64_t *b,
                      uint64_t *AA_out, uint64_t *BB_out);
```

### Wrapper PREP (before firing patch)
1. Load `a[0..4]` into (RDI, RSI, R12, R11, R14) — same as fe_sq_ucode.
2. Precompute 2A, 19A into (R15, R13, R9, R10, RBX, RDX) — same as fe_sq_ucode.
3. **Stash `b[0..4]` into (TMP10, TMP11, TMP12, TMP13, TMP14)** — verified
   free inside fe_sq body by audit.
4. Stash AA_out pointer on stack; put BB_out → RBP (survives patch like fe_sq does).
5. xor eax, eax; xor r8d, r8d (init accumulators).
6. Fire patch via vmread.

### Patch layout (~54 triads)

```
INIT (1 triad):
  ZEROEXT_DSZ32_DI(TMP_count, 0)   // counter = 0

LOOP_START (T1):
  ... 42 triads of fe_sq body (unchanged from existing patch) ...

TRANSITION (~10 triads, only on iter 1):
  // Save AA from (RDI, R9, R10, RBX, RAX) → (TMP2, TMP3, TMP4, TMP5, TMP7)
  { ZEROEXT TMP2, RDI;  ZEROEXT TMP3, R9;  ZEROEXT TMP4, R10;  NOP_SEQWORD },
  { ZEROEXT TMP5, RBX;  ZEROEXT TMP7, RAX;  ZEROEXT RDI, TMP10;  NOP_SEQWORD },
  // Load b from TMP10..14 → arch input regs
  { ZEROEXT RSI, TMP11;  ZEROEXT R12, TMP12;  ZEROEXT R11, TMP13;  NOP_SEQWORD },
  { ZEROEXT R14, TMP14;  ADD R15, RDI, RDI;  ADD R13, RSI, RSI;  NOP_SEQWORD },
  // Recompute 2*b and 19*b precomputes
  { ADD R9, R12, R12;  ADD R10, R11, R11;  NOP;  NOP_SEQWORD },
  { ZEROEXT RBX, R14;  MUL_DSZ64_DIR(scratch, 19, RBX);  NOP;  NOP_SEQWORD },
  { ZEROEXT RDX, R11;  MUL_DSZ64_DIR(scratch, 19, RDX);  NOP;  NOP_SEQWORD },
  // Reset RAX, R8 (zero accumulator)
  { XOR_DSZ64_DRR(RAX, RAX, RAX);  XOR_DSZ64_DRR(R8, R8, R8);  NOP;  NOP_SEQWORD },

LOOP_TEST (1 triad):
  { ADD_DSZ64_DIR(TMP_count, 1, TMP_count),    // counter += 1
    XOR_DSZ64_DIR(TMP_scratch, 2, TMP_count),  // scratch = counter ^ 2
    UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI(TMP_scratch, T(LOOP_START)),
    NOP_SEQWORD },

EXIT (~1 triad — copy AA from TMPs to a saved-output mechanism, or rely on wrapper):
  // BB is in (RDI, R9, R10, RBX, RAX) — wrapper stores via RBP
  // AA is in (TMP2..5, TMP7) — wrapper must extract
  END_SEQWORD
```

### Wrapper EXIT (after patch fires)
1. Store BB[0..4] from (RDI, R9, R10, RBX, RAX) via RBP — same as fe_sq_ucode.
2. AA values are in TMP regs which don't survive the patch firing — **problem**.

### The TMP-extraction problem

**Critical design issue**: TMP registers don't persist across vmwrite/vmread
boundaries (per CLAUDE.md). So AA stashed in TMP2..5,7 during patch execution
is **gone** once the patch exits.

**Resolution options**:
- (a) Before END_SEQWORD, the patch must ZEROEXT TMPs → arch regs that BB doesn't use.
  After iter 2, free arch regs include: RSI, R12, R11, R14, R15, R13.
  Move TMP2→RSI, TMP3→R12, TMP4→R11, TMP5→R14, TMP7→R15 (5 ZEROEXTs in 2 triads).
  Then wrapper extracts: BB via RBP (5 stores), AA via second pointer (5 stores).
- (b) AA stored to memory inside the patch using STSTGBUF — but stgbuf loads are
  ~30 cyc each (5 × 30 = 150 cyc just for AA extraction), unacceptable.

**Use option (a).** Adds ~2 triads to EXIT.

Total patch size: 1 INIT + 42 BODY + 8 TRANSITION + 1 LOOP_TEST + 2 EXIT
                = **54 triads**. Fits at 66 + 54 = 120/124 used.

## Validation strategy

1. Build with sq-pair installed, fe_mul unchanged.
2. Run consistency check (RFC 7748 + 1000-iter chain against fiat/donna/amd64).
3. If any mismatch, the test harness pinpoints which iter (1000-step chain
   tracks deviations).
4. Once correct, measure ours/ucode min cycles vs baseline (312,260).
5. **Decision point**: if savings ≥ ~20k cyc, mode-switch overhead is real
   and the path is worth extending to mul-pair (which requires fe_mul shrink,
   1-2 days more work). If savings < ~10k cyc, mode-switch is smaller than
   estimated and the path is dead.

## Risks / known pitfalls

- UJMPCC pitfalls from `ujmp_test.c`: SUB args reversed, 16-bit unsigned imm
  only, UJMPCC reads arch RFLAGS not the reg operand (so the op setting flags
  must be in the same triad or the immediately preceding one).
- TMP audit might miss something — re-verify against fe_sq patch (lines 275–413
  in full_curve25519.c) before writing the transition.
- The MUL_DSZ64_DIR semantics: writes lo to src reg (overwrites), hi to dst.
  Plan above uses scratch for hi; ensure no register collision.
- `ADD_DSZ64_DIR` / `XOR_DSZ64_DIR` immediate forms: confirm these exist in
  the macro headers (build/include/ucode_macros.h or wherever) before relying on them.

## Expected impact

| Measurement | Predicted | If achieved |
|---|---:|---:|
| ours/ucode min cycles | ~287k | -25k vs baseline (8% faster) |
| Gap to amd64-64 | ~15k cyc | Still behind, but much closer |

To fully beat amd64-64: would also need mul-pair combination, which requires
shrinking fe_mul to ≤62 triads (currently 66, needs −4). That's a follow-up
project after sq-pair validates the mode-switch hypothesis.
