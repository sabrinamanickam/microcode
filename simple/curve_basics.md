# X25519 / Curve25519 — The Math, Explained

What X25519 actually computes, in pseudocode, with the algorithm and
data structures spelled out. Read this before `design.md` if you want
to understand *what* the implementation is doing, not just *how*.

---

## The 10 000-foot view

X25519 is a function:

```
X25519(scalar: 32 bytes, point: 32 bytes) → result: 32 bytes
```

It computes "**scalar times point**" on a specific mathematical structure
(the curve25519 elliptic curve). The whole reason this function exists is
that it has a magic property:

```
X25519(alice_sk, X25519(bob_sk, basepoint))   ==
X25519(bob_sk,   X25519(alice_sk, basepoint))
```

Both sides equal `alice_sk · bob_sk · basepoint`. Alice and Bob can each
compute the same 32-byte secret without ever sending their private keys
over the network. That shared 32-byte secret is then fed into a KDF to
derive encryption keys.

The 32 bytes that flow over the network are public keys
(= `scalar · basepoint`). The 32 bytes that stay local are private keys
(random scalars). The recovery problem an eavesdropper faces — "given
`scalar · basepoint`, find `scalar`" — is the **elliptic curve discrete
log problem** (ECDLP), believed to require ~2¹²⁸ operations.

---

## What's a "point on the curve"?

Curve25519 is defined by an equation:

```
y² = x³ + 486662·x² + x   (mod p)
```

where `p = 2²⁵⁵ − 19` (a prime number, ~76 decimal digits long).

A "point on the curve" is any `(x, y)` pair of numbers mod `p` that
satisfies this equation. Plus a special "point at infinity" `O` that
acts as the identity element.

A `scalar` is a number between 0 and p–1 (roughly — there are some
clamping rules we'll get to).

**Scalar multiplication** is repeated point addition:

```
k · P  =  P + P + P + … + P     (k times)
```

The catch: "+" here is *elliptic-curve point addition*, defined via the
chord-and-tangent method. It's not just adding coordinates — it's a
geometric construction. The result is another point on the curve.

---

## Why X25519 only uses the x-coordinate

Curve25519 has a special property: **the x-coordinate of `k · P`
depends only on the x-coordinate of `P`** (and the scalar `k`). The
y-coordinate is determined up to sign, and the two possible points
(`(x, y)` and `(x, −y)`) give the same x-coordinate when multiplied
by `k`.

This means we can drop y entirely. X25519 operates only on
x-coordinates, which is much cheaper:

- Inputs: `scalar` (32 bytes) and `point` (the x-coordinate as 32 bytes).
- Output: the x-coordinate of `scalar · point`, as 32 bytes.
- No need to compute or transmit y.

The algorithm that does scalar multiplication using only x-coordinates
is the **Montgomery ladder**.

---

## Projective coordinates: avoiding division

Naïvely, every elliptic-curve operation involves a field *division*
(`a / b mod p`), which on a CPU costs ~265 multiplications via Fermat
(`a / b = a · b^(p-2)`). Way too expensive to do per step.

The fix: represent each point not as a single `x` but as a *projective
pair* `(X, Z)` where the actual x-coordinate is `X / Z`.

- Two pairs `(X, Z)` and `(2X, 2Z)` represent the *same* curve point
  (same `X/Z` ratio).
- We do all ladder arithmetic without dividing.
- At the very end, we compute `Z⁻¹` once and multiply: `x_final = X · Z⁻¹`.

So one inversion at the end of the whole computation, instead of one
per step.

---

## The Montgomery ladder

The clever algorithm at the heart of X25519. It walks through the
scalar's bits, maintaining **two** points and an **invariant**:

- `R0` = some multiple of the input point `P`
- `R1` = `R0 + P` (R1 is always exactly one P ahead of R0)
- Invariant: `R1 − R0 = P` (constant)

This invariant is what makes the ladder work with only x-coordinates:
"differential addition" (computing the sum of two points whose
difference is known) can be done without their y-coordinates.

For each scalar bit, from most-significant to least-significant:

```
if bit == 0:
    R1 = R0 + R1     # advance R1 by P
    R0 = 2 · R0      # advance R0 by R0 (doubling)
if bit == 1:
    R0 = R0 + R1     # advance R0 by P
    R1 = 2 · R1      # advance R1 by R1 (doubling)
```

After processing all 255 bits, `R0` holds `scalar · P` in projective
form. We compute `R0_X / R0_Z` once and output the result.

### Why this works (intuition)

At iteration `i`, R0 holds the multiplier formed by the top `i` bits of
the scalar — call that value `k_i`. Then R1 holds `(k_i + 1) · P`. After
processing one more bit:

- bit 0: new value = `2·k_i` → still `bit-0` shift. R1 becomes
  `(2·k_i + 1)·P`.
- bit 1: new value = `2·k_i + 1`. R0 becomes `(2·k_i + 1)·P`, R1 becomes
  `(2·k_i + 2)·P = 2(k_i + 1)·P`.

The difference R1 − R0 stays at exactly `P` across all transitions.

### Constant time: the `cswap` trick

The "if bit == 0 vs 1" branch above can't be implemented as a real
branch in cryptographic code — that would leak the secret scalar via
timing. Instead, we always do the *same* sequence of operations, and
use a constant-time conditional swap on R0 and R1 to logically achieve
the same effect:

```
prev_swap = 0
for bit in scalar (from MSB to LSB):
    swap = bit XOR prev_swap
    cswap(R0, R1, swap)          # if swap, exchange R0 and R1
    (R0, R1) = ladderstep(R0, R1, x1)
    prev_swap = bit
cswap(R0, R1, prev_swap)         # one last unswap
```

`cswap(a, b, c)` exchanges `a` and `b` iff `c == 1`, using bitmask
arithmetic (no branches). Always takes the same time regardless of `c`.

---

## The "ladderstep" — combined add-and-double

In one go, the ladderstep computes:

- `R0' = 2·R0`
- `R1' = R0 + R1`

…all in x-coordinate-only projective form. The classic Bernstein/RFC 7748
formula:

```
A  = x2 + z2     B  = x2 - z2          # R0 = (x2, z2)
AA = A²          BB = B²
E  = AA - BB
C  = x3 + z3     D  = x3 - z3          # R1 = (x3, z3)
DA = D · A       CB = C · B
x3' = (DA + CB)²
z3' = x1 · (DA - CB)²                  # x1 is the original x of P
x2' = AA · BB
z2' = E · (AA + 121665 · E)            # 121665 = (a + 2)/4 - 1
```

That's **5 multiplications + 4 squarings + 1 multiplication-by-constant**
plus a bunch of cheap additions. The 121665 is a curve parameter
derived from the equation's `486662` coefficient.

This formula is the load-bearing computation of X25519 — it runs 255
times per call.

---

## The full algorithm — pseudocode

```python
def X25519(scalar_bytes: 32 bytes, point_bytes: 32 bytes) -> 32 bytes:
    # 1. Clamp the scalar (RFC 7748). Forces specific bits so the
    #    scalar lies in a structurally-safe subset.
    e = bytearray(scalar_bytes)
    e[0]  &= 248         # clear bottom 3 bits (mult by 8)
    e[31] &= 127         # clear top bit
    e[31] |= 64          # set bit 254 (forces large fixed-length scalar)

    # 2. Decode the input x-coordinate from 32 little-endian bytes.
    x1 = fe_frombytes(point_bytes)    # field element in GF(2^255 - 19)

    # 3. Initialize the ladder state.
    R0 = (x = 1, z = 0)               # represents "point at infinity"
    R1 = (x = x1, z = 1)              # represents P itself

    # 4. Run the Montgomery ladder, 255 iterations from bit 254 down to 0.
    prev_swap = 0
    for pos in range(254, -1, -1):
        bit = (e[pos // 8] >> (pos % 8)) & 1
        swap = bit ^ prev_swap
        cswap(R0, R1, swap)
        (R0, R1) = ladderstep(R0, R1, x1)
        prev_swap = bit
    cswap(R0, R1, prev_swap)          # unswap one more time

    # 5. After the ladder, R0 = scalar · P in projective form (X, Z).
    #    Convert back to affine by computing X / Z.
    inv_Z = fe_invert(R0.z)           # Z^(p-2) via Fermat's chain (~265 ops)
    x_final = fe_mul(R0.x, inv_Z)     # x_final = X · Z^(-1)

    # 6. Encode result as 32 little-endian bytes.
    return fe_tobytes(x_final)


def ladderstep(R0, R1, x1):
    # R0 = (x2, z2),  R1 = (x3, z3)
    A  = fe_add(R0.x, R0.z)
    AA = fe_sq(A)
    B  = fe_sub(R0.x, R0.z)
    BB = fe_sq(B)
    E  = fe_sub(AA, BB)
    C  = fe_add(R1.x, R1.z)
    D  = fe_sub(R1.x, R1.z)
    DA = fe_mul(D, A)
    CB = fe_mul(C, B)

    t0      = fe_add(DA, CB)
    new_x3  = fe_sq(t0)

    t0      = fe_sub(DA, CB)
    t1      = fe_sq(t0)
    new_z3  = fe_mul(x1, t1)

    new_x2  = fe_mul(AA, BB)

    t0      = fe_mul121665(E)
    t1      = fe_add(AA, t0)
    new_z2  = fe_mul(E, t1)

    return ((new_x2, new_z2), (new_x3, new_z3))


def fe_invert(z):
    # Compute z^(p-2) where p = 2^255 - 19. This equals z^(-1) mod p
    # via Fermat's little theorem. Implemented as a fixed addition chain
    # of ~265 squarings interleaved with ~11 multiplications. The chain
    # is constant-time (same shape regardless of z).
    z2     = fe_sq(z)
    t      = fe_sq(z2)
    t      = fe_sq(t)
    z9     = fe_mul(t, z)
    z11    = fe_mul(z9, z2)
    # ... ~250 more operations forming the addition chain ...
    return result_of_chain


def cswap(R0, R1, c):
    # Constant-time conditional swap of two projective points.
    # If c == 1, swap their fields; if c == 0, leave alone.
    # Implemented via XOR mask, no branch:
    mask = -c                        # 0x000...0 if c=0, 0xFFF...F if c=1
    for i in 0..4:                    # for each of the 5 limbs
        t = mask & (R0.x[i] ^ R1.x[i])
        R0.x[i] ^= t
        R1.x[i] ^= t
        # ... same for z[i] ...
```

That's the entire X25519 algorithm. The rest is field arithmetic.

---

## Field arithmetic — the "fe_*" operations

All those `fe_add`, `fe_mul`, `fe_sq` etc. are operations on a single
field element — a number in the range `[0, p)` where `p = 2²⁵⁵ − 19`.

Representations:
- **Logically:** a 255-bit non-negative integer < p.
- **In memory:** 5 × 64-bit limbs, each holding 51 bits of the value
  (unsaturated radix-2⁵¹). The `40 byte` `fe` arrays you see in the
  code.

Operations:

| op             | what it computes      | cost (microcode) |
|----------------|----------------------|------------------|
| `fe_add`       | `a + b mod p`         | ~3 cyc (just add limbs; lazy mod) |
| `fe_sub`       | `a − b mod p`         | ~3 cyc (add bias, subtract; lazy mod) |
| `fe_mul`       | `a · b mod p`         | ~40 cyc (microcoded patch) |
| `fe_sq`        | `a² mod p`            | ~22 cyc (microcoded patch, fewer cross-products) |
| `fe_mul121665` | `a · 121665 mod p`    | ~25 cyc (C, __int128) |
| `fe_invert`    | `a⁻¹ mod p`           | ~10 000 cyc (uses 265 sq + 11 mul) |
| `fe_cswap`     | conditional swap      | ~25 cyc |
| `fe_frombytes` | parse 32 input bytes  | one-time |
| `fe_tobytes`   | serialize result      | one-time |

Multiplication is the expensive one because two 255-bit numbers can
produce a 510-bit product, which then has to be reduced back mod p.
The microcode patches we built handle this in 66 / 42 triads
(`fe_mul` / `fe_sq` respectively).

---

## Why the constants matter

| constant   | what it is                              | where it appears                    |
|------------|-----------------------------------------|--------------------------------------|
| `p`        | `2²⁵⁵ − 19`                              | the field modulus                   |
| `486662`   | coefficient `a` in the curve equation    | the curve definition                |
| `121665`   | `(a − 2) / 4 = (486662 − 2)/4`           | the `fe_mul121665` step in ladderstep |
| `19`       | the constant in `p`                      | drives all the modular reductions   |
| `9`        | the x-coordinate of the basepoint        | the canonical input for keypair generation |
| basepoint  | 32-byte encoding of x = 9, all other bytes 0 | `crypto_scalarmult_base` input |

The `121665` shows up because the ladderstep formula needs the
coefficient `(a+2)/4` in one slot. With `a = 486662`, that's
`121666`. Different conventions use `121665 = 121666 − 1` to fit a
specific algebraic rearrangement. We use the 121665 convention because
RFC 7748 does, and because it's the formula our code was derived from.

---

## Why X25519 is fast in cycles

For a 256-bit cryptosystem with 128-bit security level, every operation
costs:

- 5 `fe_mul` + 4 `fe_sq` per ladder iteration → ~210 cyc per iteration
  on our microcode.
- 255 iterations → ~53 k cyc just for the ladder field ops.
- + ~10 k cyc for inversion (one-time, ~265 field ops in a chain).
- + cswap, mul121665, ladder driver overhead.

The ladder is *not* the bottleneck the way ECDSA on P-256 was a decade
ago. With good field arithmetic (and ours is good), X25519 is the
cheapest serious public-key operation in widespread use.

Compare to RSA-2048: ~3 million cycles per operation, on the same CPU.
X25519 at ~150k cycles is 20× faster, with arguably *better* security.

---

## The full pipeline of one X25519 call

```
input scalar (32 bytes)                input point (32 bytes)
        │                                       │
        ├─ clamp (3 byte ops)                   │
        │                                       ├─ fe_frombytes
        │                                       │     │
        │                                       │     ▼
        │                                       │   x1 (field element)
        │                                       │
        ▼                                       ▼
    e[32]                                R0 = (1, 0), R1 = (x1, 1)
        │                                       │
        └────────────┬───────────────────────────┘
                     ▼
        ┌──────────────────────────────┐
        │  for pos = 254 down to 0:    │
        │      bit = scan e            │
        │      cswap(R0, R1, swap)     │
        │      ladderstep(R0, R1, x1)  │   ─── 255 iterations,
        │      prev_swap = bit         │       each calls
        │                              │       5 mul + 4 sq +
        │                              │       1 mul121665 + 10 add/sub
        └──────────────────────────────┘
                     │
                     ▼
              R0 = scalar · P (projective: X, Z)
                     │
                     ├─ fe_invert(R0.Z)      ← ~265 field ops
                     ├─ fe_mul(R0.X, Z_inv)  ← one more mul
                     │
                     ▼
                  x_final (field element)
                     │
                     ├─ fe_tobytes
                     │
                     ▼
              result (32 bytes)
```

That's the entire X25519 function. The microcode-accelerated bits are
the `fe_mul` and `fe_sq` calls inside `ladderstep` and `fe_invert` —
roughly 2 300 calls of those per X25519. Everything else is glue.

---

## Where to look in the code

| concept                | file                                   | function / line  |
|------------------------|----------------------------------------|------------------|
| The X25519 entry point | `simple/full_curve25519.c`             | `x25519_ucode()` |
| The ladderstep formula | `simple/full_curve25519.c`             | inside `x25519_ucode` for-loop |
| Constant-time cswap    | `simple/full_curve25519.c`             | `fe_cswap()` |
| The microcoded fe_mul  | `simple/full_curve25519.c`             | `fe_mul_ucode()` + the mul patch in `install_field_patches()` |
| The microcoded fe_sq   | `simple/full_curve25519.c`             | `fe_sq_ucode()` + the sq patch |
| Fermat inversion chain | `simple/full_curve25519.c`             | `fe_invert_ucode()` |
| Byte (de)serialization | `simple/full_curve25519.c`             | `fe_frombytes()`, `fe_tobytes()` |

The implementation-level details — register allocation, memory traffic,
cycle breakdown — are in **`design.md`**. This file is the algorithm;
`design.md` is the engineering.
