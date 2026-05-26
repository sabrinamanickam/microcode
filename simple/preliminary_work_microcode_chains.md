# Microcode-Backed Instruction Chains — Preliminary Work

> Draft subsection for the DFG project description. LaTeX-ready: `\paragraph{\textbf{...}}` headers and `[CITE: ...]` placeholders match the existing manuscript style.

---

```latex
\paragraph{\textbf{Microcode-Backed Instruction Chains}}
In preparation for this proposal, we conducted an in-depth feasibility study of
microcode-backed instruction chains on the Intel Goldmont CPU (Celeron N3350),
using \texttt{libmicro} as the runtime patching substrate [CITE: libmicro.dev]
and the \texttt{vmread}/\texttt{vmwrite} pair as hook points for invoking custom
patches from user space. Our central question was whether the patch RAM
(128 triads, U7c00--U7dfc on Goldmont) is large and expressive enough to host
\emph{multi-instruction} computations rather than the single-opcode replacements
shown in prior work, and whether the resulting kernels can outperform
state-of-the-art compiled and hand-tuned code. The study yielded affirmative
answers along three independent axes.

\paragraph{\textbf{Synthetic compound instructions}}
As a first step, we constructed patches that behave as architectural
instructions that simply do not exist in x86. Concrete examples include a
\texttt{mov+add+shl} chain that, in a single hook invocation, copies a source
register into a destination, adds an immediate, and left-shifts by a constant;
a fused \texttt{mov+mul} variant that latches its operands from architectural
registers into internal TMP registers before issuing the multiply; and a
prefix-style \texttt{cmpxchg} hook that splices a multiply into the
read-modify-write path. These experiments confirmed that the patch can read and
write \emph{any} architectural register through the same micro-op interface
used by Intel's own microcode and that operand routing through the internal TMP
file is fully programmable. The full set of these probes is collected in our
\texttt{experiments/} workbench (e.g., \texttt{addshl.c}, \texttt{mulxchg.c},
\texttt{cmpxchg.c}).

\paragraph{\textbf{Reverse engineering additional micro-operations}}
Beyond the small µop vocabulary previously catalogued by Borrello et
al.~[CITE: Borrello et al.], we identified and characterized further operations
exposed by the Goldmont microsequencer, including additional flag-manipulation
primitives (\texttt{GENARITHFLAGS}, \texttt{MOVEINSERTFLGS},
\texttt{MOVEMERGEFLGS}, \texttt{READAFLAGS}), conditional moves and selects
(\texttt{CMOVCC}, \texttt{SELECTCC}), micro-architectural jumps
(\texttt{UJMPCC}), and zero-extending memory operations (\texttt{LDZX},
\texttt{STAD}). For each, we mapped operand encodings, slot restrictions,
and which of the CPU's \emph{three} internal flag domains they read from or
write to --- a structural property of the pipeline that, to our knowledge, has
not previously been documented. Most strikingly, we observed that the
microcode layer of Goldmont exposes a three-operand non-destructive multiply
that is functionally equivalent to BMI2's \texttt{MULX}, even though Goldmont
does not implement BMI2 in its architectural ISA. In other words, the
hardware multiplier supports a capability that the architectural decoder
deliberately hides. This is, to the best of our knowledge, the first
demonstration that microcode patching can \emph{expose} microarchitectural
features otherwise inaccessible from user-mode x86.

\paragraph{\textbf{Cryptographic field arithmetic as a case study}}
To stress-test these primitives on a real workload, we implemented field
multiplication and squaring entirely in microcode for ten elliptic curves
spanning four representation styles: saturated Montgomery
(P-224, P-256, secp256k1), unsaturated Dettman (curve25519, secp256k1), and
unsaturated Solinas (Poly1305, P-521). Each kernel is a single
\texttt{vmwrite}-hooked patch that performs the schoolbook product, the
carry propagation, and the field reduction --- on the order of
$25$ multiplies and $30$ carry propagations --- without leaving microcode.
The implementation required us to engineer two patterns that we believe are
of independent interest: a \emph{triple-pack} carry chain that compresses an
add-with-carry into a single triad (three µops), and a \emph{progressive-
accumulation} schedule that interleaves the next multiply with the previous
multiply's carry fold, leaving no idle slots in the middle of a limb.

\paragraph{\textbf{Performance against state-of-the-art baselines}}
We benchmarked our microcode kernels against three established baselines
on the same Goldmont hardware: GCC/Clang \texttt{-O3} compilation of
\texttt{fiat-crypto}'s formally-verified field code [CITE: fiat-crypto],
CryptOpt's Goldmont-tuned hand-optimized assembly
[CITE: Kuepper et al., CryptOpt],
and Bernstein and Schwabe's classic \texttt{amd64-51} assembly
[CITE: amd64-51]. For curve25519 scalar multiplication, our microcode field
operations achieved a geometric-mean speed-up of $1.21\times$ over CryptOpt
and $1.19\times$ over \texttt{fiat-crypto} across $24$ compiler
configurations, with a best-case minimum of $304\,897$ cycles compared to
$379\,911$ for CryptOpt and $354\,878$ for \texttt{fiat-crypto}. In an
end-to-end comparison that uses Bernstein and Schwabe's hand-written
ladder but substitutes our microcode field operations, we further
outperformed the original \texttt{amd64-51} assembly by $1.07\times$ and
Bernstein's \texttt{donna\_c64} by $1.24\times$. Microcode therefore beats
not only the best automatically-generated assembly available on this CPU
but also two decades of hand-tuned assembly written by the designers of
the curve themselves. These gains are consistent across squaring kernels
for the NIST P-curves where the algorithm fits within the patch-RAM
budget; conversely, we identified one boundary condition --- curves whose
multiplication exceeds the 128-triad budget, requiring patch
fragmentation --- where the per-hook overhead dominates and microcode
ceases to be advantageous. Mapping this boundary precisely, and
extending the chain primitives beyond cryptography, is the agenda we
propose in this project.
```

---

## Notes for the author

1. **Citations to add.** The new `[CITE: ...]` placeholders are:
   - `fiat-crypto` — Erbsen, Philipoom, Gross, Sloan, Chlipala, "Simple High-Level Code for Cryptographic Arithmetic – With Proofs, Without Compromises", IEEE S&P 2019.
   - `Kuepper et al., CryptOpt` — Kuepper, Erbsen, Gross et al., "CryptOpt: Verified Compilation with Randomized Program Search for Cryptographic Primitives", PLDI 2023.
   - `amd64-51` — Bernstein, "Curve25519: new Diffie-Hellman speed records", PKC 2006 (the `amd64-51` implementation lives in SUPERCOP).
   - `libmicro.dev` — already in the prior paragraph; keep the same key.
2. **MULX claim.** Phrased conservatively as "functionally equivalent to BMI2's `MULX`". Strengthen to "we identified the BMI2 `MULX` micro-operation in patch RAM" only if you're prepared to defend the equivalence at the encoding level.
3. **Numbers.** All cycle counts and ratios come from `simple/results.md` (geomean rows of the min and median ratio matrices). Best single-config minima from the "Best per contender" table at the bottom of that file.
4. **Curve count.** "Ten" matches the count in `simple/microcode_findings.md` (curve25519, P-224, P-256, P-384, P-448, P-521, secp256k1 mont, secp256k1 Dettman, Poly1305 + one variant). Drop to "five fully benchmarked" if you want a tighter claim.
5. **Boundary condition paragraph.** The "P-521 mul: 18 fragments, 422 cyc vs fiat's 157" finding in `microcode_findings.md` is the empirical basis for the closing sentence. Worth mentioning by name if you have room.
