# Five-accumulator fe_sq and the road to OpenSSL parity — session log (2026-09-28/29)

Everything changed or learned in this session, in order. All numbers are TSC
cycles on the N3350, core 0, pinned, same-process comparisons unless stated.
Nothing here is committed yet.

## Bottom line

| Build (same process as OpenSSL) | X25519 median | OpenSSL | ratio |
|---|---:|---:|---:|
| serial fe_sq (shipped), chained inversion | 266,557 | 250,318 | 0.9391 |
| **five-accumulator fe_sq, unchained inversion** | **252,652** | 250,318 | **0.9908** |
| five-accumulator + fused mul121665 (`-DMUL_A24`) | 253,182 | 250,318 | 0.9887 |

- The five-accumulator fe_sq is worth **-13,905 cyc/X25519 (-5.2%)** and
  brings us to **0.9% behind OpenSSL**. It does not reset the machine as long
  as fe_sq firings are never register-chained.
- No remaining identified lever closes the last ~2.3k. Defensible claim:
  within 1% of OpenSSL's hand-written assembly, ahead of every other
  contender except s2n-bignum.

## 1. The five-accumulator fe_sq reset: cause found

`sq_patch_5acc` (41 triads) had hard-reset the machine six times. The earlier
conclusion "register chaining REFUTED" was wrong:

- `test_sq_soak rfc` calls `test_rfc7748()`, which in the CONTENDERS_ONLY
  build runs **`x25519_ucode`**, not the inline `x25519`. Its
  `fe_invert_ucode` calls **`fe_sq_ucode_n` (register-chained) 8 times**
  regardless of `-DSQ_UNCHAINED`, which only affects the inline `fe_invert`.
  So every "unchained" rfc crash still ran chained firings.
- Every recorded PASS (tight, mixed, soak, ctx, step — ~580k firings) had no
  chained firing; every RESET (chain, all rfc runs) had chained firings.

**Decisive test:** `test_sq_soak ladder 2000` on a
`-DSQ_MASK_R8 -DENABLE_SQ_5ACC -DSQ_UNCHAINED` build (inline x25519 +
one-firing-per-block inversion, zero chained firings, verified in the
disassembly): **2000 X25519, ~5.1M firings, 0 wrong. PASS.**

Rule: **never fire the 5acc fe_sq through anything that chains squarings**
(`fe_sq_ucode_n`, `x25519_ucode`, the chained `INV_RUN*` inversion). The full
`full_curve25519_inline2` matrix binary contains `x25519_ucode`, so it must
not be run with the 5acc patch until those paths are removed.

Binary: `tests/test_sq_soak_5acc_nochain_static`.

## 2. Experiments that did NOT help (kept as negative results)

| Experiment | File | Result |
|---|---|---|
| Looped squaring in fe_invert (one firing per run) | `tests/probe_inv_loop.c` | loop 81.98 cyc/sq vs chained firings 78.2; fe_invert +6.8% (22,531 vs 21,088). n=1 costs +25. Chained firings have almost no per-firing tax left. |
| Ladder glue ablation | `tests/probe_glue_ablation.c` | mul121665 37.5 cyc/step, add/sub 37.9 — but add/sub cost is their operand loads (~1.07 cyc per pre-firing load). add/sub fusion is dead. |
| Operands loaded inside the patch (LDZX prologue) | `tests/probe_patch_loads.c` | +4.1 cyc/mul slower than native loads (+8 independent). A SEQ_GOTO0 costs 1.9 cyc. LDZX with RDI/R15 base registers works. |
| Dead `xor eax` / `xor r8d` trim | see §3 | 1.04 cyc/mul in the kernel bench, but only 0.3 cyc/step in the ladder (within drift). Kept: correct and free. |
| Fused mul121665 prologue (`-DMUL_A24`) | see §4, `tests/probe_mula24.c` | Correct on hardware, but saves 0.4 cyc/step in the same layout and the check triad costs every normal fe_mul ~2.4 cyc/step. Net ~530 cyc/X25519 worse. Off by default. |

Lesson from the last row: an ablation ceiling bounds REMOVING work, not
MOVING it into a firing — relocated work stays on the dependency chain.

## 3. Production code changes (`full_curve25519_inline2.c`)

### Dead-instruction trim
- `fe_mul` wrappers (`FE_MUL`, `FE_MUL_FROM_REGS_A`, `INV_MUL`,
  `fe_mul_ucode`): removed `xor eax,eax` and `xor r8d,r8d`. The patch writes
  RAX and R8 before reading them (verified with `ucode_sim --poison`).
- Serial `sq_patch`, triads 0-1:
  `ZEROEXT_DSZ64_DR(TMP0, RAX)` -> `ZEROEXT_DSZ32_DI(TMP0, 0)` and
  `ADD_DSZ64_DRR(R8, R8, RCX)` -> `ZEROEXT_DSZ64_DR(R8, RCX)`, so it no longer
  needs RAX = R8 = 0. Still 42 triads / 123 ops; validated on hardware via
  RFC x1000 (bench_sq5acc serial).
- fe_sq wrappers: removed `xor eax,eax`; `FE_SQ_R8` is now empty for the
  serial patch and `mov r8, 2^51-1` under `-DSQ_MASK_R8` (5acc).
- Comments above `FE_MUL` and in the patch header updated to the new contract
  and to the 2026-09-29 5acc finding.

### `-DMUL_A24` (fused z2 = E*(AA + 121665*E); off by default)
- `MUL_A24_MARK` (0x5A24), `FE_MUL_R8` (`xor r8d,r8d` under MUL_A24, empty
  otherwise) added to every normal fe_mul wrapper.
- `mul_a24_prologue[]` (16 triads, one multiply per triad) and a new layout in
  `install_field_patches`: `[check][fe_mul 58][fe_sq 42][prologue 16][goto]`,
  118 triads ending U7dd8. Check = `XOR(R8^MARK)` + forward
  `UJMPCC_DIRECT_NOTTAKEN_CONDZ`; goto = `SEQ_GOTO0` back to the check.
- `FE_MUL_A24(out, e, aa)` wrapper (121665 in RAX, marker in R8).
- `ladder_step` tail uses `FE_MUL_A24` under MUL_A24, the native
  `mul121665` + add + `FE_MUL_FROM_REGS_A` otherwise.
- The default (non-MUL_A24) layout is unchanged apart from the trim.

## 4. Tooling changes

- `lib/ucode_sim.py`
  - `--poison R1,R2,...`: seeds registers with random 64-bit garbage per trial
    and statically reports the first read-before-write. Guards every "the
    wrapper doesn't need to set X" claim.
  - Arrays joined with `+` run back to back (e.g. `mul_a24_prologue+mul_patch`).
  - Kind `mula24`: reference E*(AA + 121665*E).
- `bench/bench_kernel.c`: `FE_MUL_NOFIRE` / `FE_SQ_NOFIRE` updated to mirror
  the trimmed wrappers instruction for instruction (invocation-floor table).
- `Makefile`: added `probe_inv_loop probe_glue_ablation probe_patch_loads
  bench_sq5acc probe_mula24` to the inline2 CFLAGS list and to the
  `test_sq_fire test_sq_soak` EXTRA_OBJS list.

## 5. New test/benchmark files (`tests/`)

| File | Purpose |
|---|---|
| `probe_inv_loop.c` | looped vs per-firing squaring in fe_invert |
| `probe_glue_ablation.c` | in-ladder cost of native add/sub/mul121665 |
| `probe_patch_loads.c` | in-patch LDZX operand loads vs native loads; dead-xor trim |
| `bench_sq5acc.c` | X25519 serial vs 5acc fe_sq, OpenSSL as in-process control |
| `probe_mula24.c` | staged hardware validation + timing of `-DMUL_A24` |

Built binaries: `bench_sq5acc_serial_static`, `bench_sq5acc_5acc_static`,
`probe_mula24_serial_static`, `probe_mula24_5acc_static`,
`test_sq_soak_5acc_nochain_static`.

## 6. How to rebuild and rerun

```
# 5acc vs serial, same-process OpenSSL control
make PROG=tests/bench_sq5acc -B && mv tests/bench_sq5acc_static tests/bench_sq5acc_serial_static
make PROG=tests/bench_sq5acc -B EXTRA_CPPFLAGS="-DSQ_MASK_R8 -DENABLE_SQ_5ACC -DSQ_UNCHAINED" \
  && mv tests/bench_sq5acc_static tests/bench_sq5acc_5acc_static
sudo taskset -c 0 ./tests/bench_sq5acc_serial_static
sudo taskset -c 0 ./tests/bench_sq5acc_5acc_static

# 5acc stability soak (chain-free full X25519)
make PROG=tests/test_sq_soak -B EXTRA_CPPFLAGS="-DSQ_MASK_R8 -DENABLE_SQ_5ACC -DSQ_UNCHAINED" \
  && cp tests/test_sq_soak_static tests/test_sq_soak_5acc_nochain_static
sudo taskset -c 0 ./tests/test_sq_soak_5acc_nochain_static ladder 2000

# simulator checks
python3 lib/ucode_sim.py full_curve25519_inline2.c mul_patch mul --poison RAX,R8
python3 lib/ucode_sim.py full_curve25519_inline2.c sq_patch sq --poison RAX,R8
python3 lib/ucode_sim.py full_curve25519_inline2.c sq_patch_5acc sq --mask-r8 --poison RAX
python3 lib/ucode_sim.py full_curve25519_inline2.c mul_a24_prologue+mul_patch mula24
```

## 8. SHIPPED (2026-09-29): five-accumulator fe_sq is the default

`full_curve25519_inline2.c` now selects the five-accumulator fe_sq unless built
with `-DSQ_SERIAL`. The `-DMUL_A24` fusion stays OFF (no gain, §2).

- Default build implies `ENABLE_SQ_5ACC`, `SQ_MASK_R8`, `SQ_UNCHAINED`
  (`#error` if 5acc is selected without the other two, or together with
  `SQ_SERIAL`).
- `INV_SQ_RENAME` (register chaining) is only defined for `-DSQ_SERIAL`, so a
  chained inline sequence fails to compile in a 5acc build.
- `fe_sq_ucode_n` in a 5acc build = separate `fe_sq_ucode` calls ping-ponged
  through two buffers (the `tight` shape that passed 100k firings). This
  changes the `x25519_ucode` contender's inversion (slower, unchained) and
  means `test_sq_soak chain` no longer chains in a default build.
- Install sequence unchanged from the validated one (serial written first,
  5acc overwrites it at the same address).
- `tests/probe_inv_loop.c` now `#error`s without `-DSQ_SERIAL` (it chains).
  `bench_sq5acc` / `probe_mula24` / `test_sq_soak`: serial builds now need
  `EXTRA_CPPFLAGS="-DSQ_SERIAL"`; build notes updated. Makefile comment updated.
- Offline audit of the default `full_curve25519_inline2_static`,
  `bench/bench_kernel_static`, `tests/test_sq_soak_static`: 380 vmread sites,
  every one preceded by the R8 mask load, 0 register-rename sequences, 0
  chained loops.
- Pre-existing, unrelated: `bench/bench_table` fails to link (both
  `freq_guard.h` and `include/freq_guard.h` use the guard `FREQ_GUARD_H`).

### Still to run on hardware before trusting it
1. `sudo taskset -c 0 ./tests/test_sq_soak_static ladder 2000` after a reboot.
2. `sudo taskset -c 0 ./tests/test_sq_soak_static rfc` — now exercises every
   contender that fires fe_sq (`x25519_ucode`, the amd64-51-ucode hybrids),
   all unchained. Never run before with 5acc.
3. The full 24-config sweep, then regenerate RESULTS.md and the paper tables.

## 7. Open items

1. **Repeat `ladder 2000` after a reboot** — the 5acc patch has a history of
   single-sample "passes".
2. ~~Make 5acc + unchained inversion the default~~ DONE (§8); still re-run
   the full 24-config sweep and regenerate the paper tables.
3. Update the paper: five-accumulator squaring (currently described as
   single-accumulator), and parity (not a win) with OpenSSL.
4. Commit this work.
