#!/usr/bin/env python3
"""gen_paper_tex.py — the Curve25519 performance-evaluation tables for the
paper (keccak/gold489.tex, "Case Study 2"), as LaTeX.

Inputs (all produced by bench/paper_eval.sh in one session):
    RESULTS.md                          24-config X25519 sweep (Appendices A.1, A.2)
    bench/results/kernel_5x51.txt       bench_kernel      raw arms (5x51)
    bench/results/kernel_4x64.txt       bench_kernel_4x64 raw arms (4x64)

Output: paper_tables_c25519.tex — seven tables + \\newcommand macros for every
number the prose quotes, so text and tables cannot drift apart:

    tab:c25519-kernel-51     field kernels, unsaturated 5x51
    tab:c25519-kernel-64     field kernels, saturated 4x64
    tab:c25519-ctrl-51       controlled X25519, common C ladder, 5x51 backends
    tab:c25519-decomp-51     amd64-51 framework decomposition
    tab:c25519-ctrl-64       controlled X25519, saturated 4x64
    tab:c25519-e2e           end-to-end X25519
    tab:c25519-dispersion    min / p10 / p90 behind every median above

Units follow the Keccak evaluation: raw RDTSC ticks, which equal core cycles up
to the f_core/f_TSC factor measured for the run (stated in the method macro);
ratios are invariant to it.

    python3 lib/gen_paper_tex.py [RESULTS.md] [kernel_5x51.txt] [kernel_4x64.txt] [out.tex]
"""
import os, re, sys, statistics as st

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
arg = lambda i, d: sys.argv[i] if len(sys.argv) > i else os.path.join(ROOT, d)
SRC  = arg(1, "RESULTS.md")
K51P = arg(2, "bench/results/kernel_5x51.txt")
K64P = arg(3, "bench/results/kernel_4x64.txt")
DST  = arg(4, "paper_tables_c25519.tex")

def die(msg):
    sys.exit(f"gen_paper_tex: {msg}")

# ── RESULTS.md ────────────────────────────────────────────────────────────
TXT = open(SRC).read()

def header_field(pat, default="?"):
    m = re.search(pat, TXT)
    return m.group(1) if m else default

CORR   = header_field(r"correction f_core/f_TSC:\*\*\s*([0-9.]+)", "")
DELIV  = header_field(r"Delivered core freq:\*\*\s*([0-9]+) MHz", "?")
TSCMHZ = header_field(r"TSC \(RDTSC\) rate:\*\*\s*([0-9]+) MHz", "?")
CORE   = header_field(r"Core isolation:\*\*\s*core\s*([0-9]+)", "?")
RUNS   = header_field(r"Runs per config:\*\*\s*([0-9]+)", "?")
NCFG   = header_field(r"Configs that ran:\*\*\s*([0-9]+)", "?")

def matrix(title_prefix):
    """Parse a '| Config | a | b | ... |' matrix by column NAME."""
    try:
        sec = TXT.split(title_prefix, 1)[1]
    except IndexError:
        die(f"section '{title_prefix}' not found in {SRC}")
    sec = sec.split("\n### ", 1)[0]
    hdr, rows = None, {}
    for line in sec.splitlines():
        if line.startswith("| Config |"):
            hdr = [c.strip() for c in line.strip().strip("|").split("|")][1:]
        elif hdr and re.match(r"^\|\s*(gcc|clang)", line):
            cells = [c.strip().replace("**", "") for c in line.strip().strip("|").split("|")]
            if len(cells) - 1 != len(hdr):
                die(f"{title_prefix}: row has {len(cells)-1} cells, header {len(hdr)}")
            rows[cells[0]] = {h: (int(v) if v.isdigit() else None) for h, v in zip(hdr, cells[1:])}
    if not rows:
        die(f"{title_prefix}: no configuration rows")
    return rows

A1 = matrix("### A.1")
A2 = matrix("### A.2")
CFGS = list(A1)
if len(CFGS) != 24:
    print(f"gen_paper_tex: WARNING only {len(CFGS)} configurations in A.1", file=sys.stderr)

def col(M, k):
    v = [(c, M[c].get(k)) for c in M]
    v = [(c, x) for c, x in v if x is not None]
    if not v:
        die(f"column '{k}' missing or empty")
    return v

def best(M, k):
    """(median, config) at the configuration with the lowest median."""
    c, x = min(col(M, k), key=lambda t: t[1])
    return x, c

def matched(M, num, den):
    """Geometric mean over configurations where both ran of M[den]/M[num]."""
    r = [M[c][den] / M[c][num] for c in M
         if M[c].get(num) is not None and M[c].get(den) is not None]
    if not r:
        die(f"no matched configurations for {num} / {den}")
    return st.geometric_mean(r), len(r), sum(1 for x in r if x > 1)

# dispersion at best config (median min p10 p90 cfg), by the RESULTS.md key
DISP = {}
if "### Dispersion at each contender" in TXT:
    sec = TXT.split("### Dispersion at each contender", 1)[1].split("\n---", 1)[0]
    for line in sec.splitlines():
        c = [x.strip() for x in line.strip().strip("|").split("|")]
        if len(c) == 7 and c[1].isdigit():
            num = lambda s: int(s) if s.isdigit() else None
            DISP[c[0]] = (num(c[1]), num(c[2]), num(c[3]), num(c[4]), c[6])

# ── kernel outputs ────────────────────────────────────────────────────────
def kernel(path):
    A, meta = {}, {}
    if not os.path.exists(path):
        die(f"kernel output {path} missing -- run bench/paper_eval.sh")
    for ln in open(path):
        if ln.startswith("#"):
            for tok in ln.lstrip("# ").split():
                if "=" in tok:
                    k, v = tok.split("=", 1); meta[k] = v
            continue
        f = ln.split()
        if len(f) >= 5:
            A[f[0]] = tuple(float(x) for x in f[1:5])     # med min p10 p90
    return A, meta

K51, M51 = kernel(K51P)
K64, M64 = kernel(K64P)
for need in ("uc_mul_lat", "uc_sq_lat", "floor_mul_lat", "nfloor_mul"):
    if need not in K51:
        die(f"{K51P} lacks '{need}' (was bench_kernel run as root?)")
for need in ("uc4_mul_lat", "floor4_mul_lat", "nfloor4_mul"):
    if need not in K64:
        die(f"{K64P} lacks '{need}'")
for P, M in ((K51P, M51), (K64P, M64)):
    if CORE != "?" and M.get("core") not in (None, CORE):
        die(f"{P} was measured on core {M.get('core')}, the sweep on core {CORE}")

med = lambda K, k: K[k][0]

# ── formatting ────────────────────────────────────────────────────────────
f1   = lambda x: f"{x:.1f}"
cyc  = lambda x: f"{x:,}"
rat  = lambda x: f"{x:.3f}$\\times$"
def cfg_tex(c):
    cc, o = c.split()
    cc = cc.replace("gcc-", "GCC ").replace("clang-", "Clang ")
    return f"{cc} {o.lstrip('-')}"

out, mac = [], []
w = out.append
def macro(name, val):
    assert re.fullmatch(r"[A-Za-z]+", name), name
    mac.append(f"\\newcommand{{\\{name}}}{{{val}}}")

w("% Generated by lib/gen_paper_tex.py -- do not hand-edit; regenerate.")
w(f"% Sources: {os.path.relpath(SRC, ROOT)}, {os.path.relpath(K51P, ROOT)}, "
  f"{os.path.relpath(K64P, ROOT)}")
w("% Requires booktabs. \\input{} this file once; it defines the \\cx* macros")
w("% used in the prose and the seven tables below.")
w("")

# ── method macros ─────────────────────────────────────────────────────────
macro("cxCore", CORE)
macro("cxDelivered", DELIV)
macro("cxTSC", TSCMHZ)
macro("cxCorrection", CORR or "n/a")
macro("cxConfigs", NCFG)
macro("cxRuns", RUNS)
macro("cxKernelSamples", M51.get("pooled", "?"))
macro("cxKernelPhases", M51.get("phases", "?"))

# ═════ Table: kernels, 5x51 ═══════════════════════════════════════════════
uf, nf = med(K51, "floor_mul_lat"), med(K51, "nfloor_mul")
ufs, nfs = med(K51, "floor_sq_lat"), med(K51, "nfloor_sq")
R51 = [  # label, mul arm, sq arm, is_microcode
    (r"\textbf{microcode $5\times51$}",     "uc_mul_lat",   "uc_sq_lat",   True),
    (r"OpenSSL \code{fe51} asm",            "ossl_mul_lat", "ossl_sq_lat", False),
    (r"fiat crypto",                        "fiat_mul_lat", "fiat_sq_lat", False),
    (r"CryptOpt",                           "copt_mul_lat", "copt_sq_lat", False),
    (r"amd64 51 field asm",                 "a51_mul_lat",  "a51_sq_lat",  False),
    (r"hand written \code{\_\_uint128\_t} C", "hc_mul_lat", "hc_sq_lat",   False),
]
w(r"""\begin{table}[!ht]
\centering
\caption{Cost of $5\times51$ field operations on Goldmont.
Median cycles per operation over a dependent memory-to-memory chain, all backends in one process on core \cxCore{}.
The multiplication and squaring columns include the invocation interface of each implementation.
Arithmetic only removes the separately measured invocation floor, given below the rule.
We use multiplication for the arithmetic comparison because preprocessing for squaring is distributed differently between the microcode and native implementations.}
\label{tab:c25519-kernel-51}
\footnotesize
\setlength{\tabcolsep}{5pt}
\begin{tabular}{@{}lrrr@{}}
\toprule
Backend & Multiplication & Squaring & Arithmetic only \\
\midrule""")
for lab, km, ks, uc in sorted(R51, key=lambda r: med(K51, r[1])):
    m, s = med(K51, km), med(K51, ks)
    a = m - (uf if uc else nf)
    if uc:
        w(f"{lab} & \\textbf{{{f1(m)}}} & \\textbf{{{f1(s)}}} & \\textbf{{{f1(a)}}} \\\\")
    else:
        w(f"{lab} & {f1(m)} & {f1(s)} & {f1(a)} \\\\")
w(r"\midrule")
w(f"microcode invocation floor & {f1(uf)} & {f1(ufs)} & \\\\")
w(f"native invocation floor & {f1(nf)} & {f1(nfs)} & \\\\")
w(r"""\bottomrule
\end{tabular}
\end{table}
""")
um, om = med(K51, "uc_mul_lat"), med(K51, "ossl_mul_lat")
macro("cxFiveUcMul", f1(um)); macro("cxFiveUcSq", f1(med(K51, "uc_sq_lat")))
macro("cxFiveOsslMul", f1(om)); macro("cxFiveOsslSq", f1(med(K51, "ossl_sq_lat")))
macro("cxFiveUcFloor", f1(uf)); macro("cxFiveNatFloor", f1(nf))
macro("cxFiveUcArith", f1(um - uf)); macro("cxFiveOsslArith", f1(om - nf))
macro("cxFiveArithDiffPct", f"{abs(100 * ((um - uf) / (om - nf) - 1)):.1f}")
nat_arith = [med(K51, k) - nf for _, k, _, uc in R51 if not uc and k != "ossl_mul_lat"]
macro("cxFiveOtherArithLo", f1(min(nat_arith))); macro("cxFiveOtherArithHi", f1(max(nat_arith)))

# ═════ Table: kernels, 4x64 ═══════════════════════════════════════════════
uf4, nf4, nf4s = med(K64, "floor4_mul_lat"), med(K64, "nfloor4_mul"), med(K64, "nfloor4_sq")
R64 = [
    (r"\textbf{microcode $4\times64$}",  "uc4_mul_lat", "uc4_sq_lat", True),
    (r"amd64 64 field asm",              "a64_mul_lat", "a64_sq_lat", False),
    (r"s2n bignum (\code{\_alt})",       "s2n_mul_lat", "s2n_sq_lat", False),
]
w(r"""\begin{table}[!ht]
\centering
\caption{Cost of saturated $4\times64$ field operations on Goldmont, measured as in \Cref{tab:c25519-kernel-51}.
The microcode control has no dedicated squaring kernel, so its squaring invokes multiplication with equal operands; both of its columns therefore carry the same invocation floor.}
\label{tab:c25519-kernel-64}
\footnotesize
\setlength{\tabcolsep}{5pt}
\begin{tabular}{@{}lrrr@{}}
\toprule
Backend & Multiplication & Squaring & Arithmetic only \\
\midrule""")
for lab, km, ks, uc in sorted(R64, key=lambda r: med(K64, r[1])):
    m, s = med(K64, km), med(K64, ks)
    a = m - (uf4 if uc else nf4)
    if uc:
        w(f"{lab} & \\textbf{{{f1(m)}}} & \\textbf{{{f1(s)}}} & \\textbf{{{f1(a)}}} \\\\")
    else:
        w(f"{lab} & {f1(m)} & {f1(s)} & {f1(a)} \\\\")
w(r"\midrule")
w(f"microcode invocation floor & {f1(uf4)} & {f1(uf4)} & \\\\")
w(f"native invocation floor & {f1(nf4)} & {f1(nf4s)} & \\\\")
w(r"""\bottomrule
\end{tabular}
\end{table}
""")
macro("cxFourUcMul", f1(med(K64, "uc4_mul_lat"))); macro("cxFourUcSq", f1(med(K64, "uc4_sq_lat")))
macro("cxFourAsmMul", f1(med(K64, "a64_mul_lat"))); macro("cxFourAsmSq", f1(med(K64, "a64_sq_lat")))
macro("cxFourSnMul", f1(med(K64, "s2n_mul_lat"))); macro("cxFourSnSq", f1(med(K64, "s2n_sq_lat")))
macro("cxFourUcFloor", f1(uf4)); macro("cxFourNatFloor", f1(nf4))
macro("cxFourUcArith", f1(med(K64, "uc4_mul_lat") - uf4))
macro("cxFourAsmArith", f1(med(K64, "a64_mul_lat") - nf4))
macro("cxFourMulRatio", rat(med(K64, "uc4_mul_lat") / med(K64, "a64_mul_lat")))

# ═════ Table: controlled 5x51 (common C ladder) ═══════════════════════════
C51 = [  # label, A.2 key
    (r"\textbf{microcode $5\times51$}",       "uc/Clad"),
    (r"OpenSSL \code{fe51} asm",              "osslops/C-ladder"),
    (r"fiat crypto",                          "fiat"),
    (r"CryptOpt",                             "cryptopt"),
    (r"amd64 51 field asm",                   "a51op/Clad"),
    (r"hand written \code{\_\_uint128\_t} C", "hand-C"),
]
w(r"""\begin{table}[!ht]
\centering
\caption{Controlled comparison of $5\times51$ field backends.
Every row uses the same C X25519 implementation and differs only in multiplication and squaring.
Best median reports the lowest median obtained by each backend across the \cxConfigs{} compiler and optimization configurations.
Matched ratio is the geometric mean of native cycles divided by microcode cycles across matching configurations.
Values above one favor microcode.}
\label{tab:c25519-ctrl-51}
\footnotesize
\setlength{\tabcolsep}{4pt}
\begin{tabular}{@{}lrr@{}}
\toprule
Field backend & Best median & Matched ratio \\
\midrule""")
for lab, k in sorted(C51, key=lambda r: best(A2, r[1])[0]):
    b, _ = best(A2, k)
    if k == "uc/Clad":
        w(f"{lab} & \\textbf{{{cyc(b)}}} & 1.000$\\times$ \\\\")
    else:
        w(f"{lab} & {cyc(b)} & {rat(matched(A2, 'uc/Clad', k)[0])} \\\\")
w(r"""\bottomrule
\end{tabular}
\end{table}
""")
macro("cxCtrlUc", cyc(best(A2, "uc/Clad")[0]))
macro("cxCtrlOssl", cyc(best(A2, "osslops/C-ladder")[0]))
g, n, won = matched(A2, "uc/Clad", "osslops/C-ladder")
macro("cxCtrlOsslRatio", rat(g)); macro("cxCtrlOsslWon", f"{won}")
for mname, k in (("Fiat", "fiat"), ("Copt", "cryptopt"), ("Asm", "a51op/Clad"), ("HandC", "hand-C")):
    g, n, won = matched(A2, "uc/Clad", k)
    macro(f"cxCtrl{mname}Ratio", rat(g)); macro(f"cxCtrl{mname}Won", f"{won}")

# ═════ Table: amd64-51 decomposition ══════════════════════════════════════
D51 = [("amd64 51 native", "assembly", "assembly", "a51/asm"),
       ("amd64 51 C ladder", "C", "assembly", "a51/asmCld"),
       ("microcode C ladder", "C", "microcode", "a51/ucCld"),
       ("microcode chained ladder", "inline assembly", "microcode", "a51/ucode")]
w(r"""\begin{table}[!ht]
\centering
\caption{Decomposition of the $5\times51$ implementation within the \texttt{amd64 51} framework.
Adjacent rows change one component.
The second and third rows use the same C ladder and differ only in the field backend.
Medians are the best across the compiler configurations.
Matched step is the geometric mean, across matching configurations, of the previous row's cycles divided by this row's cycles, so a value above one means the change made X25519 faster.}
\label{tab:c25519-decomp-51}
\footnotesize
\setlength{\tabcolsep}{4pt}
\begin{tabular}{@{}lllrr@{}}
\toprule
Variant & Ladder & Field backend & Median & Matched step \\
\midrule""")
prev = None
for name, lad, fb, k in D51:
    b, _ = best(A1, k)
    step = "" if prev is None else rat(matched(A1, k, prev)[0])
    w(f"{name} & {lad} & {fb} & {cyc(b)} & {step} \\\\")
    prev = k
w(r"""\bottomrule
\end{tabular}
\end{table}
""")
macro("cxDecNative", cyc(best(A1, "a51/asm")[0])); macro("cxDecCasm", cyc(best(A1, "a51/asmCld")[0]))
macro("cxDecCuc", cyc(best(A1, "a51/ucCld")[0])); macro("cxDecChained", cyc(best(A1, "a51/ucode")[0]))
macro("cxDecFieldBest", rat(best(A1, "a51/asmCld")[0] / best(A1, "a51/ucCld")[0]))
macro("cxDecFieldMatched", rat(matched(A1, "a51/ucCld", "a51/asmCld")[0]))
macro("cxDecChainBest", rat(best(A1, "a51/ucCld")[0] / best(A1, "a51/ucode")[0]))
macro("cxDecChainMatched", rat(matched(A1, "a51/ucode", "a51/ucCld")[0]))

# ═════ Table: controlled 4x64 ═════════════════════════════════════════════
w(r"""\begin{table}[!ht]
\centering
\caption{Controlled replacement for the saturated $4\times64$ representation.
The second and third rows use the same C ladder and surrounding framework and are measured interleaved in one process; only multiplication and squaring change between them.
Relative cost divides each median by the C ladder assembly median at the same configuration and takes the geometric mean.}
\label{tab:c25519-ctrl-64}
\footnotesize
\setlength{\tabcolsep}{4pt}
\begin{tabular}{@{}lllrr@{}}
\toprule
Variant & Ladder & Field backend & Median & Matched rel.\ cost \\
\midrule""")
w(f"amd64 64 native & assembly & assembly & {cyc(best(A1, 'a64/asm')[0])} & "
  f"{rat(1 / matched(A1, 'a64/asm', 'a64/asmCld')[0])} \\\\")
w(f"amd64 64 C ladder & C & assembly & {cyc(best(A1, 'a64/asmCld')[0])} & 1.000$\\times$ \\\\")
w(f"amd64 64 microcode & C & microcode & {cyc(best(A1, 'a64/ucode')[0])} & "
  f"{rat(matched(A1, 'a64/asmCld', 'a64/ucode')[0])} \\\\")
w(r"""\bottomrule
\end{tabular}
\end{table}
""")
macro("cxSatNative", cyc(best(A1, "a64/asm")[0])); macro("cxSatCasm", cyc(best(A1, "a64/asmCld")[0]))
macro("cxSatUc", cyc(best(A1, "a64/ucode")[0]))
macro("cxSatBest", rat(best(A1, "a64/ucode")[0] / best(A1, "a64/asmCld")[0]))
macro("cxSatMatched", rat(matched(A1, "a64/asmCld", "a64/ucode")[0]))

# ═════ Table: end to end ══════════════════════════════════════════════════
E2E = [("s2n bignum", r"$4\times64$, verified assembly", "s2n-bignum/asm"),
       ("OpenSSL", r"$5\times51$, assembly", "openssl"),
       (r"\textbf{this work}", r"\textbf{$5\times51$, microcode}", "ucode"),
       ("amd64 64", r"$4\times64$, assembly", "a64/asm"),
       ("amd64 51 with microcode", r"$5\times51$, microcode", "a51/ucode"),
       (r"\code{donna\_c64}", r"$5\times51$, portable C", "donna"),
       ("fiat crypto", r"$5\times51$, generated C", "fiat"),
       ("amd64 51", r"$5\times51$, assembly", "a51/asm"),
       ("hand written C", r"$5\times51$, \code{\_\_uint128\_t}", "hand-C"),
       ("CryptOpt", r"$5\times51$, optimized assembly", "cryptopt")]
ours = best(A1, "ucode")[0]
w(r"""\begin{table}[!ht]
\centering
\caption{End to end X25519 performance on the Goldmont N3350.
Each implementation appears in its best configuration across the \cxConfigs{} compiler and optimization configurations.
Values report median cycles per scalar multiplication; relative cost divides each median by that of this work.}
\label{tab:c25519-e2e}
\footnotesize
\setlength{\tabcolsep}{4pt}
\begin{tabular}{@{}llrrl@{}}
\toprule
Implementation & Representation and backend & Median & Rel.\ cost & Best config \\
\midrule""")
for name, rep, k in sorted(E2E, key=lambda r: best(A1, r[2])[0]):
    b, c = best(A1, k)
    if k == "ucode":
        w(f"{name} & {rep} & \\textbf{{{cyc(b)}}} & \\textbf{{1.000$\\times$}} & {cfg_tex(c)} \\\\")
    else:
        w(f"{name} & {rep} & {cyc(b)} & {rat(b / ours)} & {cfg_tex(c)} \\\\")
w(r"""\bottomrule
\end{tabular}
\end{table}
""")
macro("cxEOurs", cyc(ours))
for mname, k in (("Snb", "s2n-bignum/asm"), ("Ossl", "openssl"), ("Asixfour", "a64/asm"),
                 ("Afiveone", "a51/asm"), ("Donna", "donna"), ("Fiat", "fiat")):
    macro(f"cxE{mname}", cyc(best(A1, k)[0]))

# ═════ Table: dispersion ══════════════════════════════════════════════════
w(r"""\begin{table}[!ht]
\centering
\caption{Dispersion behind the medians of the Curve25519 tables.
Kernel rows give cycles per operation over the pooled samples of \Cref{tab:c25519-kernel-51,tab:c25519-kernel-64}; X25519 rows give cycles per scalar multiplication at the configuration selected in \Cref{tab:c25519-e2e,tab:c25519-ctrl-64,tab:c25519-decomp-51}.}
\label{tab:c25519-dispersion}
\footnotesize
\setlength{\tabcolsep}{4pt}
\begin{tabular}{@{}lrrrr@{}}
\toprule
Measurement & Median & Min & $p_{10}$ & $p_{90}$ \\
\midrule
\multicolumn{5}{@{}l}{\emph{field kernels, multiplication}} \\""")
for lab, K, k in [(r"microcode $5\times51$", K51, "uc_mul_lat"),
                  (r"OpenSSL \code{fe51} asm", K51, "ossl_mul_lat"),
                  (r"fiat crypto", K51, "fiat_mul_lat"),
                  (r"microcode $4\times64$", K64, "uc4_mul_lat"),
                  (r"amd64 64 field asm", K64, "a64_mul_lat"),
                  (r"s2n bignum (\code{\_alt})", K64, "s2n_mul_lat")]:
    m, mn, p10, p90 = K[k]
    w(f"\\quad {lab} & {f1(m)} & {f1(mn)} & {f1(p10)} & {f1(p90)} \\\\")
w(r"\multicolumn{5}{@{}l}{\emph{complete X25519}} \\")
for lab, k in [("this work", "ucode"), ("s2n bignum", "s2n-bignum/asm"),
               ("OpenSSL", "openssl"), ("amd64 64", "a64/asm"),
               ("amd64 64 C ladder", "a64/asmCld"), ("amd64 64 microcode", "a64/ucode"),
               ("amd64 51", "a51/asm")]:
    if k not in DISP:
        continue
    m, mn, p10, p90, _ = DISP[k]
    dash = lambda v: cyc(v) if v is not None else "--"
    w(f"\\quad {lab} & {dash(m)} & {dash(mn)} & {dash(p10)} & {dash(p90)} \\\\")
w(r"""\bottomrule
\end{tabular}
\end{table}""")

text = "\n".join(["% ---- numbers quoted in the prose ----"] + mac + [""] + out) + "\n"
open(DST, "w").write(text)
print(f"wrote {DST}: 7 tables, {len(mac)} macros (core {CORE}, correction {CORR or 'n/a'})")
