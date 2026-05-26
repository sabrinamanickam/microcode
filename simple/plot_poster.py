#!/usr/bin/env python3
"""
plot_poster.py — render two poster panels from bench_supercop_matrix.sh output.

Usage:
    ./bench_supercop_matrix.sh | tee bench.log
    ./plot_poster.py bench.log poster.pdf

Two panels, each a bar chart of cycles normalised to our microcode (=1.0×):
  (A) Same-ladder field-op comparison — isolates the field-op backend.
  (B) End-to-end implementations — whole-stack X25519 cost.

Bar height = ratio at the BEST compiler/-O config for each contender.
Whiskers   = full min/max range of ratios across all compiler configs,
             so reviewers can see microcode's win isn't compiler-luck.
On-bar label shows "X.XX× (N cyc)" so absolute cycle counts stay visible.
"""
import re
import sys
from collections import defaultdict

import matplotlib.pyplot as plt
import numpy as np

LOG = sys.argv[1] if len(sys.argv) > 1 else "bench.log"
OUT = sys.argv[2] if len(sys.argv) > 2 else "poster.pdf"

# Short column headers in print_matrix() → canonical contender labels.
SHORT_TO_FULL = {
    "hand-C":    "ours/hand-C",
    "fiat":      "ours/fiat",
    "cryptopt":  "ours/cryptopt",
    "ucode":     "ours/ucode",
    "donna":     "donna_c64",
    "a51/asm":   "amd64-51/asm",
    "a51/ucode": "amd64-51/ucode",
}

# data[metric][contender][cfg] = cycles
data = {"min": defaultdict(dict), "median": defaultdict(dict)}

metric = None
is_ratio = False
cols = None
cfg_row_re = re.compile(r"^(gcc|clang)-\d+\s+-O")

with open(LOG) as f:
    for line in f:
        m = re.search(r"\[metric: (min|median) cycles\]", line)
        if m:
            metric, is_ratio, cols = m.group(1), False, None
            continue
        if re.search(r"\[ratio = (min|median) cycles", line):
            metric, is_ratio, cols = None, True, None
            continue
        if is_ratio or metric is None:
            continue

        stripped = line.strip()
        if stripped.startswith("Config"):
            parts = [p.strip() for p in line.split("|")[1:]]
            cols = [SHORT_TO_FULL.get(p, p) for p in parts]
            continue

        if cols and "|" in line and not stripped.startswith("---"):
            head, *rest = [p.strip() for p in line.split("|")]
            if not cfg_row_re.match(head):
                continue
            for col, val in zip(cols, rest):
                val = val.rstrip("*").strip()
                if val and val != "—":
                    try:
                        data[metric][col][head] = int(val)
                    except ValueError:
                        pass

if not data["min"]:
    sys.exit(f"error: no min-cycles data parsed from {LOG} — is this the right log?")

# ─── panel definitions ───────────────────────────────────────────────
PANELS = [
    {
        "title":      "Same-ladder: field-op backend only",
        "subtitle":   "(our Montgomery ladder; only the field multiply/square differs)",
        "contenders": ["ours/hand-C", "ours/fiat", "ours/cryptopt", "ours/ucode"],
        "labels":     ["hand-C", "fiat-crypto", "CryptOpt", "microcode"],
        "ref":        "ours/ucode",
    },
    {
        "title":      "End-to-end: whole X25519 stack",
        "subtitle":   "(each implementation brings its own ladder, invert, cswap, …)",
        "contenders": ["donna_c64", "amd64-51/asm", "ours/ucode"],
        "labels":     ["donna_c64", "amd64-51", "microcode"],
        "ref":        "ours/ucode",
    },
]

# ─── render ──────────────────────────────────────────────────────────
plt.rcParams.update({"font.size": 11, "axes.spines.top": False, "axes.spines.right": False})
fig, axes = plt.subplots(1, 2, figsize=(13, 5.2),
                         gridspec_kw={"width_ratios": [4, 3]})

for ax, spec in zip(axes, PANELS):
    cs, labels, ref = spec["contenders"], spec["labels"], spec["ref"]

    # Drop contenders we have no data for (e.g. cryptopt not built).
    keep = [(c, l) for c, l in zip(cs, labels) if data["min"].get(c)]
    if not keep:
        ax.set_visible(False)
        continue
    cs, labels = zip(*keep)

    ref_best = min(data["min"][ref].values())
    best     = [min(data["min"][c].values()) for c in cs]
    ratios   = [v / ref_best for v in best]

    # Whisker = ratio range over configs where both contender and ref ran.
    lo, hi = [], []
    for c in cs:
        shared = set(data["min"][c]) & set(data["min"][ref])
        rs = [data["min"][c][k] / data["min"][ref][k] for k in shared]
        lo.append(min(rs) if rs else ratios[cs.index(c)])
        hi.append(max(rs) if rs else ratios[cs.index(c)])

    x      = np.arange(len(cs))
    colors = ["#d62728" if c == ref else "#4a7fb0" for c in cs]
    ax.bar(x, ratios, color=colors, edgecolor="black", linewidth=0.6, width=0.65)

    yerr = [[r - l for r, l in zip(ratios, lo)],
            [h - r for r, h in zip(ratios, hi)]]
    ax.errorbar(x, ratios, yerr=yerr, fmt="none",
                ecolor="black", capsize=5, lw=1)

    for xi, r, b in zip(x, ratios, best):
        ax.text(xi, max(r, hi[xi]) + 0.04,
                f"{r:.2f}×\n({b:,} cyc)",
                ha="center", va="bottom", fontsize=10)

    ax.axhline(1.0, color="#d62728", lw=0.8, ls="--", alpha=0.5)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=15, ha="right")
    ax.set_ylabel("Cycles ÷ microcode cycles  (lower = faster)")
    ax.set_title(spec["title"], fontsize=12, pad=8)
    ax.text(0.5, -0.22, spec["subtitle"], transform=ax.transAxes,
            ha="center", va="top", fontsize=9, style="italic", color="#555")
    ymax = max(hi) if hi else max(ratios)
    ax.set_ylim(0, ymax * 1.25)

fig.suptitle("X25519 scalar multiplication — Intel Goldmont (Celeron N3350)",
             fontsize=13, y=0.99)
fig.tight_layout(rect=(0, 0.02, 1, 0.95))
fig.savefig(OUT, bbox_inches="tight")
print(f"wrote {OUT}  ({len(data['min'])} contenders, "
      f"{max(len(v) for v in data['min'].values())} configs)")
