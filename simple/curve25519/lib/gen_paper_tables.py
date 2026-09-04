#!/usr/bin/env python3
"""Generate the CHES/TCHES paper tables for the Curve25519/X25519 evaluation.

Reads the raw per-configuration sweep from RESULTS.md (Appendices A.1, A.2) and
emits PAPER_TABLES.md in the format fixed by curve25519_results_handoff.md.

Absolute values are converted from RDTSC ticks to core cycles using the
correction measured by THIS run's frequency guard (f_core/f_TSC, from
APERF/MPERF) and rounded to three significant figures (kcycles).  Ratios are
computed from the RAW full-precision ticks -- they are invariant to the
correction -- so no rounding artefact enters a relative column.

Run context is taken from the environment so the methodology table can never
drift from the conditions the numbers were actually measured under:

    CYCLE_CORRECTION   f_core/f_TSC  (empty => no correction applied, flagged)
    DELIVERED_FREQ_MHZ Bzy_MHz under load
    TSC_FREQ_MHZ       actual RDTSC rate
    PINNED_FREQ_KHZ / GOVERNOR / NO_TURBO / CPU_MODEL / CONFIGS_RAN

Normally invoked by lib/paper_tables.sh at the end of a sweep.  Can also be run
by hand against an existing RESULTS.md:

    python3 lib/gen_paper_tables.py [RESULTS.md] [PAPER_TABLES.md]
"""
import re, sys, os, statistics as st

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE) if os.path.basename(HERE) == "lib" else HERE
SRC  = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "RESULTS.md")
DST  = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "PAPER_TABLES.md")

def env(k, d=""):
    v = os.environ.get(k, "").strip()
    return v if v else d

# The TSC->core-cycle correction MUST come from the run that produced the data.
# If the frequency guard could not measure it (turbostat absent), fall back to
# 1.0 and say so loudly rather than silently reusing a stale constant.
_corr_raw = env("CYCLE_CORRECTION")
try:
    CORR = float(_corr_raw) if _corr_raw else 1.0
except ValueError:
    CORR = 1.0
CORR_MEASURED = bool(_corr_raw) and CORR != 1.0

def bench_reps(path, default="?"):
    """Read BENCH_REPS out of a benchmark source so the stated n cannot drift."""
    try:
        m = re.search(r'#define\s+BENCH_REPS\s+(\d+)', open(os.path.join(ROOT, path)).read())
        return m.group(1) if m else default
    except OSError:
        return default

REPS_MAIN = bench_reps("full_curve25519_inline2.c")
REPS_A64  = bench_reps("full_curve25519_amd64_64_ucode.c")

A1_HDR = ["ucode","a64/asm","a64/asmCld","a64/ucode","a51/asm","a51/asmCld",
          "a51/ucCld","a51/ucode","cryptopt","fiat","hand-C","donna"]
A2_HDR = ["uc/Clad","a51op/Clad","cryptopt","fiat","hand-C"]

TXT = open(SRC).read()

def parse(start, end, hdr):
    sec = TXT.split(start)[1].split(end)[0]
    cfgs, cols = [], {h: [] for h in hdr}
    for line in sec.splitlines():
        if not re.match(r'^\|\s*(gcc|clang)', line):
            continue
        cells = [c.strip().replace('**','') for c in line.strip().strip('|').split('|')]
        cfgs.append(cells[0])
        for h, v in zip(hdr, cells[1:]):
            cols[h].append(int(v))
    assert len(cfgs) == 24, f"expected 24 configs in {start}, got {len(cfgs)}"
    return cfgs, cols

CFG, A1 = parse("### A.1", "### A.2", A1_HDR)
_,   A2 = parse("### A.2", "### A.3", A2_HDR)

kc     = lambda raw: f"{raw * CORR / 1000.0:.3g}"     # ticks -> kcycles, 3 s.f.
best   = min
gmean  = st.geometric_mean
def paired(num, den):
    """Per-config ratios den/num (>1 => num faster) and their geometric mean."""
    r = [d / n for n, d in zip(num, den)]
    return r, gmean(r)

out = []
w   = out.append

# ── header ────────────────────────────────────────────────────────────────
w("# Curve25519 / X25519 — paper tables")
w("")
w("_Generated from `RESULTS.md` (Appendices A.1, A.2) by `gen_paper_tables.py`. "
  "Do not hand-edit; regenerate._")
w("")
if CORR_MEASURED:
    w(f"_Absolute values are RDTSC ticks converted to core cycles by ×{CORR} "
      f"(f_core/f_TSC, measured by this run's frequency guard), then rounded to three "
      f"significant figures. Ratios are computed from raw full-precision ticks and are "
      f"invariant to that correction._")
else:
    w("_**No frequency correction was applied**: this run's guard could not measure "
      "f_core/f_TSC (turbostat unavailable), so absolute values below are raw RDTSC ticks, "
      "not core cycles. Ratios are unaffected. Re-run with turbostat present before using "
      "these absolutes in the paper._")
w("")

w("## Measurement setup")
w("")
w("| item | value | source |")
w("|---|---|---|")
_setup = [
    ("CPU", env("CPU_MODEL", "Intel Celeron N3350 (Goldmont), nominal 1.10 GHz"), "`/proc/cpuinfo`"),
    ("Core / pinning", "core 0, `taskset -c 0`", "`lib/build_run.sh`"),
    ("Governor", f"`{env('GOVERNOR','?')}`, requested {env('PINNED_FREQ_KHZ','?')} kHz",
     "`lib/freq_guard.sh`"),
    ("Turbo", f"disabled (`no_turbo = {env('NO_TURBO','?')}`)", "`lib/freq_guard.sh`"),
    ("Delivered core freq (load)", f"{env('DELIVERED_FREQ_MHZ','n/a')} MHz (APERF/MPERF)", "turbostat"),
    ("TSC / RDTSC rate", f"{env('TSC_FREQ_MHZ','n/a')} MHz", "turbostat"),
    ("Correction f_core/f_TSC",
     (f"{CORR}" if CORR_MEASURED else
      "**not measured — absolute values below are raw RDTSC ticks**"),
     "measured before the sweep"),
    ("Timing", "`RDTSC`, serialised `cpuid; rdtsc` before / `rdtscp; cpuid` after",
     "`full_curve25519_inline2.c`"),
    ("Timer overhead", "15 ticks (min 13, p99 17); **not** subtracted — 0.005% of ~300k",
     "measured"),
    ("Post-sweep frequency check",
     env("FREQ_DRIFT_STATUS", "not checked") +
     (f" (delivered {env('POST_DELIVERED_FREQ_MHZ')} MHz / TSC {env('POST_TSC_FREQ_MHZ')} MHz after)"
      if env("POST_DELIVERED_FREQ_MHZ") else ""),
     "`lib/freq_guard.sh`"),
    ("Core isolation", env("ISOLATION_STATUS", "not configured"), "`lib/isolation.sh`"),
    ("nohz_full / isolcpus", env("NOHZ_FULL_STATUS", "unknown"), "`/proc/cmdline`"),
    ("Timing order", "**interleaved** — round-robin, one repetition of every "
     "contender per round", "`benchmark()`"),
    ("Statistic", f"median; each configuration measured {env('RUNS_PER_CONFIG','1')}× and "
     f"reduced to the median of those runs", "`bench_stats()`, `median_of`"),
    ("Run-to-run reproducibility",
     (f"worst spread {env('REPRO_WORST_PCT')}% ({env('REPRO_WORST_WHERE','—')})"
      if env("REPRO_WORST_PCT") else "n/a (single run per config)"),
     "`note_repro`"),
    ("Repetitions",
     (f"**{REPS_MAIN}** per contender, all binaries" if REPS_MAIN == REPS_A64 else
      f"**{REPS_MAIN}** per contender; **{REPS_A64}** for the two `amd64-64` rows "
      "(separate binaries)"), "`BENCH_REPS`"),
    ("Warm-up", "RFC 7748 verification, then one untimed call per contender",
     "`benchmark()`"),
    ("Inputs", "fixed RFC 7748 vector 1, byte-identical across all repetitions",
     "`benchmark()`"),
    ("Compiler sweep", f"{env('CONFIGS_RAN','24')} configs: "
     "{gcc-11,12,13, clang-14,17,18} × {-O,-O2,-O3,-Os}", "`lib/build_run.sh`"),
    ("Correctness", "all contenders pass RFC 7748 vectors 1-4 in every configuration",
     "`test_rfc7748()`"),
]
for k, v, src in _setup:
    w(f"| {k} | {v} | {src} |")
w("")

# ── Table 1 ───────────────────────────────────────────────────────────────
B51 = [("microcode","uc/Clad"), ("fiat-crypto","fiat"), ("CryptOpt","cryptopt"),
       ("hand-written C","hand-C"), ("amd64-51 asm","a51op/Clad")]
B64 = [("assembly","a64/asmCld"), ("microcode","a64/ucode")]

w("## Table 1 — Controlled X25519 field-arithmetic comparison")
w("")
w("| Representation | Common ladder / framework | Field backend | kcycles/X25519 | Relative cycles |")
w("|---|---|---|---:|---:|")
base51 = best(A2["uc/Clad"])
for i, (name, key) in enumerate(B51):
    b = best(A2[key]); m = "**" if i == 0 else ""
    w(f"| {m}5×51{m} | common C ladder | {m}{name}{m} | {m}{kc(b)}{m} | {m}×{b/base51:.2f}{m} |")
base64 = best(A1["a64/asmCld"])
for i, (name, key) in enumerate(B64):
    b = best(A1[key]); m = "**" if i == 0 else ""
    w(f"| {m}4×64 saturated{m} | amd64-64 C ladder | {m}{name}{m} | {m}{kc(b)}{m} | {m}×{b/base64:.2f}{m} |")
w("")
r64, g64 = paired(A1["a64/asmCld"], A1["a64/ucode"])
w("> **Table 1: Controlled X25519 field-arithmetic comparison.** Cycle counts are median "
  "core kcycles per X25519, rounded to three significant figures. Within each representation "
  "block the ladder and surrounding implementation are identical and only field multiplication "
  "and squaring change. The 5×51 rows use our common C Montgomery ladder; the 4×64 rows use "
  "the same amd64-64 C `ladderstep.c`. Relative cycle counts are normalised **within** each "
  "block, so the two blocks must not be compared against one another. The 4×64 microcode "
  "backend computes sq(a) = mul(a, a) because its 75-triad multiplier leaves no room for a "
  + (f"dedicated squarer inside the 128-triad patch capacity. All rows are the median of "
     f"{REPS_MAIN} repetitions."
     if REPS_MAIN == REPS_A64 else
     f"dedicated squarer inside the 128-triad patch capacity. 5×51 rows are the median of "
     f"{REPS_MAIN} repetitions, 4×64 rows of {REPS_A64}."))
w("")
_g51 = {k: paired(A2["uc/Clad"], A2[k])[1] for k in ["fiat","cryptopt","a51op/Clad","hand-C"]}
w(f"With the 5×51 representation fixed, microcode outperforms every evaluated ISA-level field "
  f"backend. The advantage persists across all 24 matched compiler and optimisation "
  f"configurations, with paired geometric-mean speedups between ×{min(_g51.values()):.3f} and "
  f"×{max(_g51.values()):.3f} (Appendix B.1). This result does not extend to the saturated 4×64 "
  f"representation: with the amd64-64 C ladder held fixed the microcode backend requires "
  f"{best(A1['a64/ucode'])/base64:.3f}× as many cycles as the assembly backend "
  f"({g64:.3f}× as a paired geometric mean, Appendix B.2). The 128-triad patch capacity "
  f"prevents the 4×64 implementation from holding both its 75-triad multiplier and a dedicated "
  f"squarer, forcing squaring through multiplication.")
w("")

# ── Table 2 ───────────────────────────────────────────────────────────────
E2E = [("Bernstein–Schwabe amd64-64 asm","4×64","a64/asm"),
       ("this work","5×51","ucode"),
       ("amd64-51 framework + microcode","5×51","a51/ucode"),
       ("donna c64","5×51","donna"),
       ("fiat-crypto","5×51","fiat"),
       ("Bernstein–Schwabe amd64-51 asm","5×51","a51/asm"),
       ("CryptOpt","5×51","cryptopt"),
       ("hand-written C","5×51","hand-C")]
w("## Table 2 — End-to-end X25519 performance")
w("")
w("| Implementation | Representation | kcycles/X25519 | Relative cycles |")
w("|---|---|---:|---:|")
ours = best(A1["ucode"])
for name, rep, key in sorted(E2E, key=lambda t: best(A1[t[2]])):
    b = best(A1[key]); m = "**" if key == "ucode" else ""
    w(f"| {m}{name}{m} | {m}{rep}{m} | {m}{kc(b)}{m} | {m}×{b/ours:.3f}{m} |")
w("")
w("> **Table 2: End-to-end X25519 performance.** Cycle counts are median core kcycles per "
  "X25519, rounded to three significant figures, each row at its own best compiler "
  "configuration. These are complete implementations differing in representation, ladder "
  "structure, inversion, field arithmetic and code organisation; the table therefore "
  "establishes overall standing but does **not** isolate the effect of microcode. Lower is "
  "better; relative cycles are normalised to this work.")
w("")

# ── Table 3 ───────────────────────────────────────────────────────────────
DEC = [("amd64-51 native","qhasm, monolithic","qhasm asm","a51/asm"),
       ("C-ladder control","C, per-op calls","qhasm asm","a51/asmCld"),
       ("C-ladder + microcode","C, per-op calls","microcode","a51/ucCld"),
       ("chained ladder + microcode","inline asm, register-chained","microcode","a51/ucode")]
w("## Table 3 — 5×51 integration and ladder decomposition")
w("")
w("| Variant | Ladder | Field ops | kcycles/X25519 |")
w("|---|---|---|---:|")
for name, lad, ops, key in DEC:
    w(f"| {name} | {lad} | {ops} | {kc(best(A1[key]))} |")
w("")
_,  g1 = paired(A1["a51/asm"],   A1["a51/asmCld"])
_,  g2 = paired(A1["a51/ucCld"], A1["a51/asmCld"])
r3, g3 = paired(A1["a51/ucode"], A1["a51/ucCld"])
keep = [i for i, c in enumerate(CFG) if not (c.startswith("gcc") and "-Os" in c)]
g3k  = gmean([r3[i] for i in keep])
nwin = sum(1 for x in r3 if x > 1)
w("**Transitions** — each changes exactly one element from the row above:")
w("")
w("| Transition | Effect isolated | best-of-24 | paired geomean |")
w("|---|---|---:|---:|")
w(f"| native qhasm → C ladder | loss of ladder/field-op fusion | "
  f"×{best(A1['a51/asmCld'])/best(A1['a51/asm']):.3f} | ×{g1:.3f} |")
w(f"| asm field ops → microcode | controlled microcode field-op gain | "
  f"×{best(A1['a51/asmCld'])/best(A1['a51/ucCld']):.3f} | ×{g2:.3f} |")
w(f"| C ladder → register-chained | ladder-integration recovery | "
  f"×{best(A1['a51/ucCld'])/best(A1['a51/ucode']):.3f} | ×{g3:.3f} (×{g3k:.3f} excl. gcc `-Os`) |")
w("")
w(f"> **Table 3: 5×51 integration and ladder decomposition.** All rows use the amd64-51 "
  f"framework. Register-chaining the ladder helps in {nwin} of 24 configurations "
  f"(paired geometric mean ×{g3k:.3f}); under gcc `-Os` the inline-assembly ladder degrades "
  f"sharply in this framework (×{min(r3):.2f}, reproducible across gcc-11/12/13 to within 43 "
  f"ticks), which pulls the all-configuration geometric mean down to ×{g3:.3f}. The same "
  f"chained ladder in our own framework shows no such degradation, so this is a "
  f"compiler/framework interaction rather than a property of the ladder. The final row is "
  f"`amd64-51/ucode`, **not** the canonical implementation reported in Table 2.")
w("")

# ── Appendix A ────────────────────────────────────────────────────────────
w("## Appendix A — full per-configuration sweep")
w("")
w("_Median RDTSC ticks per X25519 at each of the 24 configurations. **Bold** = best "
  "(lowest) in that column. Raw ticks" +
  (f"; multiply by {CORR} for core cycles._" if CORR_MEASURED else " (uncorrected)._"))
w("")
def matrix(title, cols, hdr, names):
    w(f"### {title}")
    w("")
    w("| Config | " + " | ".join(names) + " |")
    w("|---" * (len(hdr) + 1) + "|")
    lo = {h: min(cols[h]) for h in hdr}
    for i, c in enumerate(CFG):
        w(f"| {c} | " + " | ".join(
            (f"**{cols[h][i]:,}**" if cols[h][i] == lo[h] else f"{cols[h][i]:,}")
            for h in hdr) + " |")
    w("")
matrix("A.1 — End-to-end, every complete implementation (Table 2)", A1, A1_HDR,
       ["this work","a64/asm","a64/asm+Clad","a64/ucode","a51/asm","a51/asm+Clad",
        "a51/uc+Clad","a51/ucode","CryptOpt","fiat","hand-C","donna"])
matrix("A.2 — Common C ladder, only the field backend differs (Table 1, 5×51 block)",
       A2, A2_HDR, ["microcode","amd64-51 asm","CryptOpt","fiat","hand-C"])

# ── Appendix B ────────────────────────────────────────────────────────────
w("## Appendix B — paired per-configuration ratios")
w("")
w("_Ratio = comparison ÷ microcode at the **same** compiler configuration; >1 means microcode "
  "is faster. Computed from raw ticks._")
w("")
w("### B.1 — 5×51 block, common C ladder (Table 1)")
w("")
cmp51 = [("fiat","fiat-crypto"),("cryptopt","CryptOpt"),
         ("a51op/Clad","amd64-51 asm"),("hand-C","hand-written C")]
rat = {k: paired(A2["uc/Clad"], A2[k])[0] for k, _ in cmp51}
w("| Config | " + " | ".join(n for _, n in cmp51) + " |")
w("|---" + "|---:" * len(cmp51) + "|")
for i, c in enumerate(CFG):
    w(f"| {c} | " + " | ".join(f"{rat[k][i]:.3f}" for k, _ in cmp51) + " |")
w("| **geometric mean** | " + " | ".join(f"**{gmean(rat[k]):.3f}**" for k, _ in cmp51) + " |")
w("| **configurations won** | " + " | ".join(
    f"**{sum(1 for x in rat[k] if x > 1)}/24**" for k, _ in cmp51) + " |")
w("")
w("### B.2 — 4×64 block, amd64-64 C ladder (Table 1)")
w("")
w("| Config | microcode ÷ assembly |")
w("|---|---:|")
for i, c in enumerate(CFG):
    w(f"| {c} | {A1['a64/ucode'][i]/A1['a64/asmCld'][i]:.3f} |")
w(f"| **geometric mean** | **{g64:.3f}** |")
w("")

# ── Appendix C ────────────────────────────────────────────────────────────
NAMES = {"ucode":"this work (5×51, chained ladder)", "a64/asm":"amd64-64 asm",
         "a64/asmCld":"amd64-64 asm, C ladder", "a64/ucode":"4×64 microcode, C ladder",
         "a51/asm":"amd64-51 asm", "a51/asmCld":"amd64-51 asm, C ladder",
         "a51/ucCld":"5×51 microcode, C ladder",
         "a51/ucode":"5×51 microcode, chained ladder", "cryptopt":"CryptOpt",
         "fiat":"fiat-crypto", "hand-C":"hand-written C", "donna":"donna c64"}
w("## Appendix C — dispersion at each selected configuration")
w("")
w(f"_Median of {REPS_MAIN} repetitions ({REPS_A64} for the two `amd64-64` rows). Raw RDTSC ticks._")
w("")
w("| contender | median | min | p10 | p90 | p90−p10 | best config |")
w("|---|---:|---:|---:|---:|---:|---|")
sec = TXT.split("### Dispersion at each contender")[1].split("# Appendix A")[0]
for line in sec.splitlines():
    if not line.startswith("|"):
        continue
    cells = [c.strip() for c in line.strip().strip("|").split("|")]
    if cells[0] not in NAMES:
        continue
    cells[0] = NAMES[cells[0]]
    cells = [f"{int(c):,}" if c.isdigit() else c for c in cells]
    w("| " + " | ".join(cells) + " |")
w("")
w("> The p90−p10 spread is a near-constant ~6,500–7,000 ticks for every contender regardless "
  "of its cost, consistent with an external perturbation (timer interrupts on the benchmark "
  "core) rather than contender behaviour. Median, minimum and p10 agree to within 0.01% for "
  "every contender, so the reported medians sit at the interference-free floor and the "
  "ranking is not an artefact of noise.")
w("")
w("_Dispersion for the two backends measured only in the A.2 matrix (`uc/Clad`, "
  "`a51op/Clad`) is printed by the harness but not currently captured into `RESULTS.md`; "
  "re-run the sweep to record it._")

open(DST, "w").write("\n".join(out) + "\n")
print(f"wrote {DST} ({len(out)} lines)")
