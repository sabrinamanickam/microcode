# Reviewer 2 — Adversarial Review of `full_curve25519` Benchmark

Reviewing `bench_supercop_matrix.sh` + `full_curve25519.c` as an unsympathetic
external reviewer. Issues are ranked by how badly they could mislead a reader
of the headline numbers.

---

## Critical (could invalidate the headline numbers)

### 1. Apples vs oranges in the X25519 comparison

This is the big one. What each `x25519_*` function actually contains:

| function          | ladder source     | fe_invert source  | mul121665 source  | cswap source      |
|-------------------|-------------------|-------------------|-------------------|-------------------|
| `x25519_native`   | ours              | ours              | ours              | ours              |
| `x25519_fiat`     | ours              | ours              | ours              | ours              |
| `x25519_ucode`    | ours              | ours              | ours              | ours              |
| `x25519_donna_c64`| **donna's own**   | **donna's own**   | **donna's own**   | **donna's own**   |
| `x25519_amd64_51` | **SUPERCOP's**    | **SUPERCOP's**    | **SUPERCOP's**    | **SUPERCOP's**    |

When ucode beats donna at full X25519, you can't tell whether it's because
microcode field ops are faster than donna's field ops, because *our* ladder is
faster than *donna's* ladder, or some combination. The clean comparison is
`x25519_native` vs `x25519_fiat` vs `x25519_ucode` — those share infrastructure
and differ only at field-op granularity. The donna / amd64-51 rows are
end-to-end *implementation* comparisons, not field-op comparisons. The
presentation conflates these.

**Fix:** report the matrix in two sections — "same-ladder field-op comparison"
(native/fiat/ucode) vs "end-to-end implementation comparison" (all five). Or
wire microcode into amd64-51's ladder for a true head-to-head.

### 2. No CPU frequency pinning

`rdtsc` counts at the TSC frequency, which on Goldmont is fixed (~1.1 GHz), but
the CPU's actual execution clock varies with P-state. With the `powersave`
governor (Linux default), the CPU can ramp down to ~800 MHz between bench
iterations and back up under load, giving you ~1.4× spread in apparent
"cycles" for the same work. The script doesn't do
`cpupower frequency-set -g performance` or similar. Min-of-N papers over this
somewhat; median doesn't.

**Fix:** prepend the script with `sudo cpupower frequency-set -g performance`
(or fail loudly if not pinned). Report the actual CPU frequency observed via
`/proc/cpuinfo` or `cpufreq-info` at the start.

### 3. No interrupt isolation

The 2× `avg`/`min` gap observed previously is ~12 timer-tick hits per 100
reps. Median rejects them, but they also drift `min` if a tick fires inside the
function on every sample. Core 0 isn't isolated via `isolcpus=` and there's no
`chrt -f 99` for realtime priority.

**Fix:** boot kernel with `isolcpus=0 nohz_full=0 rcu_nocbs=0`, or at minimum
run with `chrt -f 99 taskset -c 0`. Disable `irqbalance` on that core.

---

## Significant (skews ~5–15%)

### 4. Inline-asm wrapper tax unique to microcode

Look at `fe_mul_ucode`:

```
push r15
load 5 a-limbs from memory; load 5 b-limbs from memory
xor accumulators
vmwrite
pop r15
store 5 result limbs
```

That's roughly 15 cycles of register juggling per call that amd64-51's
hand-tuned calling convention avoids (it passes pointers directly and keeps
state in regs across multiple field ops). Over ~1300 field ops per X25519,
this is ~20k cycles of overhead attributed to "microcode" that's really
attributable to "how we wrap microcode."

**Fix:** restructure the ladder to keep operands in registers across multiple
ucode calls, or accept the overhead and disclose it as part of the cost model.

### 5. The microcode results bypass `-O3` entirely

For pure-C contenders (native, fiat), `-O3` shapes their hot path. For
microcode, the C compiler only generates the wrapper; the actual hot code is
in patch RAM. So the `O3 → Os` sensitivity rows for native/fiat are real flag
effects; the matching rows for ucode are pure measurement noise. The matrix
table presents them with equal visual weight as if they're equally meaningful
— they're not.

**Fix:** mark ucode and amd64-51 rows in the matrix with a "(flag-insensitive)"
note, or drop them from the per-flag rows and only report once.

### 6. Single test vector

All measurements use the RFC 7748 test-vector-1 scalar/point. Branch prediction
trains hard on this one input. A different input could shake numbers by a few
percent — and the *relative* ordering of contenders by a percent or two.

**Fix:** rotate through 10–20 different `(scalar, point)` pairs across the 100
reps.

---

## Minor

### 7. Inadequate warmup

One call per contender, then 100 timed calls of the *same* contender
consecutively. By rep 5 the icache, BTB, and dcache are warm — but rep 1 is
being included in min/median, so the first sample is artificially slow. The
min is OK (won't be from rep 1) but the median can be polluted on cold runs.

**Fix:** discard the first ~5 reps from each block before computing stats.

### 8. SMT not disabled

Goldmont N3350 doesn't have HT, so this is fine on this hardware. If the
benchmark is ever ported to a different chip without disabling SMT, the
sibling thread steals cycles. Worth a comment in the script.

### 9. No compiler version reported

SUPERCOP records `gcc --version` and `clang --version` per result row. Without
that, "best result is gcc -O3" isn't reproducible — gcc 11 vs gcc 13 produce
materially different code for some kernels.

**Fix:** dump `$CC --version | head -1` at the start of each config in the
script.

### 10. No standard deviation or IQR

The script reports min + median. A reviewer wants to see "is min
unrepresentative (a once-per-100 lucky run) or stable (most samples cluster
near min)?" That's the IQR (q3 − q1) — if it's small, min is the true cost;
if it's huge, min is an outlier.

**Fix:** also print q1, q3 next to min/median.

### 11. Microcode patch RAM is shared global state

Between `make` runs in the matrix script, the previous binary's patches stay
installed at U7c00/U7d08 (until the new binary re-installs over them). Not a
correctness issue here because each binary calls `init_match_and_patch()`
first, but if a binary crashed mid-bench, the next one inherits dirty state.
The script doesn't check for that.

---

## Things that look wrong but actually aren't

For completeness, items a hasty reviewer might flag that don't actually
matter here:

- **`-fwrapv` for crypto code.** Looks suspicious (crypto is unsigned), but
  harmless — gcc handles it correctly, no codegen change.
- **`-fPIE -fPIC` together.** Looks redundant. Both are legal and together they
  mean "fully PIC compiled, can be linked either as PIE or shared object." No
  codegen difference vs `-fPIE` alone in our static-binary case.
- **`-masm=intel` in our `full_curve25519.c` build but not SUPERCOP's.** Only
  affects inline-asm parsing; doesn't affect codegen. Truly unavoidable for
  the microcode wrappers.

---

## Bottom line

**The microcode-vs-amd64-51 number is the load-bearing claim of this
benchmark, and it's the weakest comparison in the table** because:

1. They use different ladders (issue #1).
2. The microcode wrapper has unmeasured tax (issue #4).
3. Frequency drift could move either result by 10% (issue #2).
4. Interrupts contaminate median (issue #3).

The microcode-vs-native and microcode-vs-fiat comparisons are much cleaner
(same ladder, same wrapper overhead applies uniformly since they all live in
our ladder). Lead with those numbers, and present the SUPERCOP rows as "for
orientation, here are what published end-to-end implementations of X25519
score on this CPU." Don't claim "we beat amd64-51" from this benchmark even
if the numbers say so.
