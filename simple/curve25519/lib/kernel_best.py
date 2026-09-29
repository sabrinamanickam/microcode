#!/usr/bin/env python3
"""kernel_best.py - merge per-configuration kernel runs into one best-config file.

The field-kernel harnesses (bench_kernel, bench_kernel_4x64) print one row per
timed arm: `label median min p10 p90`. paper_eval.sh runs them once per
compiler configuration (the same 24 the X25519 sweep uses). This picks, for
each CONTENDER, the configuration with the lowest dependent-chain fe_mul
median and takes ALL of that contender's rows from that one configuration,
which is the X25519 tables' rule ("each implementation appears in its best
configuration") and keeps a contender's arms mutually consistent (the
invocation-floor arithmetic subtracts wrap_*/floor_* from uc_* of the SAME
build).

Contender = label prefix before the first '_', with the floor/wrapper arms
folded into the microcode contender they belong to (wrap, floor -> uc;
floor4 -> uc4). Selection key: <contender>_mul_lat, else <contender>_mul,
else the row's own median.

Output keeps the input format, so lib/gen_paper_tex.py reads it unchanged,
plus comment lines naming each contender's configuration, and a
<out>.configs table.

Usage:
  python3 lib/kernel_best.py OUT.txt  "gcc-11 -O3=path/kernel.txt" ...
"""
import os
import sys

ALIAS = {"wrap": "uc", "floor": "uc", "floor4": "uc4"}


def read(path):
    head, rows = [], {}
    for ln in open(path):
        if ln.startswith("#"):
            head.append(ln.rstrip("\n"))
            continue
        f = ln.split()
        if len(f) >= 5:
            rows[f[0]] = ln.rstrip("\n")
    return head, rows


def median(row):
    return float(row.split()[1])


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    out = sys.argv[1]
    runs = []
    for arg in sys.argv[2:]:
        cfg, path = arg.split("=", 1)
        head, rows = read(path)
        if rows:
            runs.append((cfg, head, rows))
    if not runs:
        raise SystemExit("no kernel rows in any input")

    order = list(runs[0][2])                 # label order of the first run
    for _, _, rows in runs[1:]:
        order += [l for l in rows if l not in order]

    group = lambda l: ALIAS.get(l.split("_", 1)[0], l.split("_", 1)[0])
    groups = {}
    for l in order:
        groups.setdefault(group(l), []).append(l)

    pick = {}
    for g, labels in groups.items():
        key = next((k for k in (g + "_mul_lat", g + "_mul") if k in labels), labels[0])
        best = None
        for cfg, _, rows in runs:
            if key in rows and all(l in rows for l in labels):
                m = median(rows[key])
                if best is None or m < best[1]:
                    best = (cfg, m)
        if best is None:
            raise SystemExit(f"no configuration has every arm of contender '{g}'")
        pick[g] = best

    rows_of = {cfg: rows for cfg, _, rows in runs}
    # The harness writes its outputs as root (it runs under sudo), so an old
    # file here may not be writable by us; the directory is, so replace it.
    for path in (out, out + ".configs"):
        if os.path.exists(path):
            os.remove(path)
    with open(out, "w") as fo:
        for h in runs[0][1]:
            fo.write(h + "\n")
        fo.write(f"# selection=best-config-per-contender configs={len(runs)} "
                 f"key=<contender>_mul_lat\n")
        for g in groups:
            fo.write(f"# contender {g}: {pick[g][0]} ({pick[g][1]:.2f})\n")
        for l in order:
            fo.write(rows_of[pick[group(l)][0]][l] + "\n")

    with open(out + ".configs", "w") as fo:
        cfgs = [c for c, _, _ in runs]
        fo.write("contender " + " ".join(c.replace(" ", "_") for c in cfgs) + " best\n")
        for g, labels in groups.items():
            key = next((k for k in (g + "_mul_lat", g + "_mul") if k in labels), labels[0])
            vals = [f"{median(r[key]):.2f}" if key in r else "-" for _, _, r in runs]
            fo.write(f"{g} " + " ".join(vals) + f" {pick[g][0].replace(' ', '_')}\n")
    for g in groups:
        print(f"  {g:8s} best in {pick[g][0]:14s} {pick[g][1]:8.2f}")


if __name__ == "__main__":
    main()
