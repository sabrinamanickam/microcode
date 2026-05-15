# X25519 Microcode Implementation — Design Notes

How `simple/full_curve25519.c`'s `ours/ucode` variant computes X25519, with
register allocation, memory traffic, and cycle-budget breakdown.

---

## Montgomery ladder step — the math

Each of the 255 iterations of the X25519 ladder (RFC 7748) does:

```
A  = x2 + z2     B  = x2 - z2
AA = A^2         BB = B^2
E  = AA - BB
C  = x3 + z3     D  = x3 - z3
DA = D * A       CB = C * B
x3' = (DA + CB)^2
z3' = x1 * (DA - CB)^2
x2' = AA * BB
z2' = E * (AA + 121665 * E)
```

Plus a constant-time `cswap(x2, x3)` and `cswap(z2, z3)` driven by the
current scalar bit XOR'd with the previous. Total per iteration:

| operation     | count | implementation              |
|---------------|------:|-----------------------------|
| `fe_mul`      |     5 | microcode (vmwrite hook)    |
| `fe_sq`       |     4 | microcode (vmread hook)     |
| `fe_mul121665`|     1 | C with `__uint128_t`        |
| `fe_add` / `fe_sub` |~10 | C, static inline           |
| `fe_cswap`    |     2 | C, XOR-mask loop            |

255 iterations × the above, plus the final inversion (`~265` field ops as
a fixed addition chain), then one multiply and `fe_tobytes`.

---

## C-level structure: every `fe` lives on the stack

Inside `x25519_ucode`:

```c
fe x1, x2, z2, x3, z3;                            /* ladder coords  */
fe A, AA, B, BB, E, C, D, DA, CB, t0;             /* temporaries    */
```

`fe` is `uint64_t[5]` (= 40 bytes). 15 of them = **600 bytes of
stack-resident state** per X25519 call. Far too much to keep in
registers, so every `fe_*` call:

1. Reads its operand `fe`s from stack memory into registers.
2. Computes (in microcode for `fe_mul`/`fe_sq`; in C for everything
   else).
3. Writes the result `fe` back to stack memory.

That marshalling — not the microcode arithmetic — is the dominant
*memory* pattern.

---

## `fe_mul_ucode` — what each call actually does

Optimized inline asm (post wrapper-trim, see `benchmark_results.md`):

```asm
; entry per SysV ABI:  rdi=a-ptr  rsi=b-ptr  rdx=out-ptr
mov rbp, rdx            ; stash out-ptr in callee-saved rbp

; 5 LOADS from b
mov r15, [rsi]          ; b[0]
mov r13, [rsi +  8]     ; b[1]
mov r9,  [rsi + 16]     ; b[2]
mov r10, [rsi + 24]     ; b[3]
mov rbx, [rsi + 32]     ; b[4]

; 5 LOADS from a (a[0] last so the a-ptr survives)
mov rsi, [rdi +  8]     ; a[1]
mov r12, [rdi + 16]     ; a[2]
mov r11, [rdi + 24]     ; a[3]
mov r14, [rdi + 32]     ; a[4]
mov rdi, [rdi]          ; a[0]  -- destroys rdi pointer

; zero the two accumulators the patch expects
xor eax, eax
xor r8d, r8d

; -------- microcode fires here (NO memory access) --------
vmwrite rcx, rdx        ; opcode triggers the hooked patch

; 5 STORES of the 5 result limbs
mov [rbp     ], r15     ; h[0]
mov [rbp +  8], r13     ; h[1]
mov [rbp + 16], r9      ; h[2]
mov [rbp + 24], r10     ; h[3]
mov [rbp + 32], rax     ; h[4]
```

**Memory accesses per `fe_mul`:** 10 reads (5×a + 5×b) + 5 writes (out) =
**15 memops**, all L1-resident.

### Register allocation

| stage              | register      | holds                          |
|--------------------|---------------|---------------------------------|
| entry              | `rdi rsi rdx` | a-ptr, b-ptr, out-ptr (SysV)   |
| after stash        | `rbp`         | out-ptr (survives microcode)   |
| after a-loads      | `rdi rsi r12 r11 r14` | a[0..4]                |
| after b-loads      | `r15 r13 r9 r10 rbx`  | b[0..4]                |
| pre-fire           | `rax`, `r8`   | both 0 (microcode accumulators)|
| inside microcode   | `rcx rdx`     | scratch (MUL hi-halves)        |
| inside microcode   | `TMP0..TMP15` | internal carry/accumulator chains |
| exit microcode     | `r15 r13 r9 r10 rax` | h[0..4] (results)       |
| stores             | `rbp`         | out-ptr                        |

15 of the 16 GP registers used. Only `rsp` is preserved as the stack
pointer.

`rbp` is the unique register the microcode *doesn't* touch, which is why
we use it to hold the output pointer across the patch invocation.

---

## `fe_sq_ucode` — same structure, half the operands

```asm
; entry: rdi=a-ptr  rsi=out-ptr  (only 2 args)
mov rbp, rsi            ; stash out-ptr

; 5 LOADS from a, a[0] last
mov r14, [rdi + 32]     ; a[4]
mov r11, [rdi + 24]     ; a[3]
mov r12, [rdi + 16]     ; a[2]
mov rsi, [rdi +  8]     ; a[1]
mov rdi, [rdi]          ; a[0]

; pre-compute the doubled and 19×-reduced limbs the patch needs
lea r15, [rdi + rdi]    ; 2·a[0]
lea r13, [rsi + rsi]    ; 2·a[1]
lea r9,  [r12 + r12]    ; 2·a[2]
lea r10, [r11 + r11]    ; 2·a[3]
imul rbx, r14, 19       ; 19·a[4]
imul rdx, r11, 19       ; 19·a[3]

xor eax, eax
xor r8d, r8d

; vmread opcode (0f 78 ca) -- hooked to fe_sq patch
.byte 0x0f, 0x78, 0xca

; 5 STORES
mov [rbp     ], rdi     ; h[0]  -- result reg is rdi here, not r15
mov [rbp +  8], r9      ; h[1]
mov [rbp + 16], r10     ; h[2]
mov [rbp + 24], rbx     ; h[3]
mov [rbp + 32], rax     ; h[4]
```

**Memory accesses per `fe_sq`:** 5 reads + 5 writes = **10 memops**.
Fewer than `fe_mul` because there's only one input.

The `lea` and `imul` setup is unique to squaring: the patch internally
exploits the symmetry of `a[i] · a[j]` cross-products by precomputing
the doubled and the 19×-reduced limbs in arch registers up front.
Those six instructions are pure register operations — no memory.

---

## Per-iteration memory traffic

```
operation         calls   loads      stores     total
─────────────────────────────────────────────────────
fe_mul_ucode        5     5·10 = 50   5·5 = 25    75
fe_sq_ucode         4     4·5  = 20   4·5 = 20    40
fe_mul121665        1     5            5          10
fe_add              5     5·10 = 50   5·5 = 25    75
fe_sub              5     5·10 = 50   5·5 = 25    75
fe_cswap            2     2·10 = 20   2·10= 20    40
                                                ─────
                                                 315  per iteration
```

× 255 iterations ≈ **80 000 memory operations per X25519**, plus ~3 000
from the inversion chain.

All hit L1 cache (the working set is well under L1's 24 KB on Goldmont)
and overlap with microcode execution in the pipeline. Memory bandwidth
is *not* the bottleneck.

---

## Cycle-budget breakdown per X25519

```
microcode fe_mul work        1030 × ~40 cyc  =  41 k     (pure patch time)
microcode fe_sq  work        1275 × ~22 cyc  =  28 k
fe_mul/sq wrapper overhead   2305 × ~15 cyc  =  35 k     (the asm above)
C ladder driver loop                            16 k
fe_mul121665 (__uint128_t)    255 × ~25 cyc  =   6 k
fe_add + fe_sub (inline C)   2550 × ~3 cyc   =   8 k
fe_cswap (XOR-mask C)         255 × ~25 cyc  =   6 k
pack / unpack / clamp                            3 k
                                              ──────
                                              ~143 k cycles per X25519
```

Roughly:
- **~70 k** in microcode itself (computing field-op math)
- **~35 k** in inline-asm wrappers (loads, stores, register shuffling)
- **~38 k** in C-side glue (ladder loop, helpers, control flow)

The dominant remaining target for optimization is the wrapper overhead —
that's the ~15 cyc/call of marshalling between memory and registers.
Algorithmically, the microcode is already near-optimal for this CPU
(verified by the field-op-only comparisons in `benchmark_results.md`
table b: microcode ~5% faster than amd64-51's hand-tuned asm field
ops on Goldmont).

---

## Why we use `rbp` for the output pointer

The microcode patches clobber **r15, r13, r9, r10, rax** (these are the
output-limb registers). Among the SysV callee-saved regs (`rbx, rbp,
r12, r13, r14, r15`), only `rbp` is *not* used by the patches —
everything else either holds an input operand or receives an output limb.

Stashing the output pointer in `rbp` means we can write the result
limbs directly from registers to `[rbp + offset]` after the patch
returns, without needing a stack push/pop pair to preserve the pointer
across the microcode invocation.

GCC's prologue/epilogue handles `rbp`'s callee-save once per function
call (one push, one pop), which we'd pay anyway. The win is eliminating
the *inner* `push r15 / pop rcx` that the previous wrapper required.

(`rbp` is the conventional x86-64 frame pointer. GCC omits it by default
at `-O1` and above on this platform — all our bench configs satisfy
this. With `-fno-omit-frame-pointer`, this design would break.)

---

## Why load order matters: `a[0]` last

For both `fe_mul_ucode` and `fe_sq_ucode`, the **a[0]** limb is loaded
*last*:

```asm
mov rsi, [rdi +  8]   ; a[1]
mov r12, [rdi + 16]   ; a[2]
mov r11, [rdi + 24]   ; a[3]
mov r14, [rdi + 32]   ; a[4]
mov rdi, [rdi]        ; a[0]  -- destroys the a-pointer
```

The final load `mov rdi, [rdi]` reads from `[rdi]` (using rdi as the
*source* address) and writes to `rdi` (destroying it as a *destination*).
This is a deliberate trick: it lets us free up the rdi register slot
for the a[0] value at exactly the right moment, without an intermediate
move. The same pattern works on `rsi` in `fe_mul`'s b-load sequence,
where `b[4]` lands in `rbx` while `rsi` is still valid as the b-pointer.

Don't reorder these loads without preserving the "pointer used as source
in the same instruction that writes its register" invariant.

---

## What's *not* in microcode

Several operations stay in C:

- **`fe_add` / `fe_sub`** — trivial element-wise ops, 5 limbs each.
  Microcoding would cost ~15 cyc of wrapper overhead to save ~3 cyc of
  C arithmetic. Net loss.
- **`fe_mul121665`** — five `__uint128_t` multiplies by the small
  constant 121665, plus carry propagation. ~25 cyc in C. Microcoding
  *might* break even but requires a third patch and a third hook.
- **`fe_cswap`** — XOR-mask conditional swap, constant-time. ~25 cyc
  in C. Could potentially be ~8 cyc in SSE2; modest win.
- **`fe_frombytes` / `fe_tobytes`** — byte-level packing of 51-bit limbs
  into 32-byte X25519 wire format. Called once at the start and end;
  total impact ~2k cyc.
- **`fe_invert`** — Fermat exponentiation chain. The chain itself stays
  in C; each step in the chain *is* a microcoded `fe_mul` or `fe_sq`.
- **Scalar clamping** — three lines of byte-mask ops on the secret key,
  negligible cost.

These choices are pragmatic: anything that doesn't take long enough to
amortize the ~15-cycle vmwrite/vmread wrapper round-trip stays in
generated C code.

---

## Layer summary

```
┌──────────────────────────────────────────────────────────────┐
│  C: x25519_ucode()                                            │
│     ↳ Montgomery ladder driver (loop, scalar bits, cswap)    │
│     ↳ fe_invert_ucode() — addition chain                     │
│     ↳ fe_frombytes / fe_tobytes — byte (un)packing           │
├──────────────────────────────────────────────────────────────┤
│  C: fe_add, fe_sub, fe_mul121665, fe_cswap, fe_copy, ...     │
│     ↳ inline / __uint128_t / bitmask                          │
├──────────────────────────────────────────────────────────────┤
│  Asm wrappers: fe_mul_ucode() / fe_sq_ucode()                │
│     ↳ load 5–10 limbs into specific GP registers              │
│     ↳ fire vmwrite (mul) or vmread (sq)                       │
│     ↳ store 5 result limbs                                    │
├──────────────────────────────────────────────────────────────┤
│  Microcode patches in patch RAM (108 triads total)            │
│     ↳ U7c00:  fe_mul (66 triads), fired by vmwrite hook       │
│     ↳ U7d08:  fe_sq  (42 triads), fired by vmread hook        │
│     ↳ pure register/TMP-register computation, NO memory       │
└──────────────────────────────────────────────────────────────┘
```

Each layer marshals state down to the next one. The C ladder pushes
`fe` arrays through stack memory; the asm wrappers move them into
specific GP registers; the microcode operates entirely in registers
(arch + TMP) plus its own internal patch-RAM state. No memory inside
microcode; no microcode inside the C layer.
