#!/usr/bin/env python3
"""ucode_critpath.py - longest dependency chain through a microcode patch.

Complements ucode_sim.py: that one checks a patch is CORRECT, this one checks a
rewrite did not trade operation count for critical path. Reports triads, real
ops, and the dependency depth from the patch's input registers to its outputs,
optionally weighting MUL by its measured ~5.9 cycle latency.

A patch is issue bound when depth * op_latency < triads * 1.61, which is where
both production patches sit. If a rewrite pushes depth above that line it will
stop tracking the triad count and the cost model in PLAN_kernel_optimization.md
no longer applies.

Usage:
  python3 lib/ucode_critpath.py <file.c> <array_name> [mul_weight]
  python3 lib/ucode_critpath.py full_curve25519_inline2.c mul_patch 5.9
"""
import re, sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from ucode_sim import parse

INPUTS = ['RDI', 'RSI', 'R12', 'R11', 'R14', 'R15', 'R13', 'R9', 'R10',
          'RBX', 'RDX', 'RAX', 'R8', 'RCX']
OUTPUTS_SQ = ('RDI', 'R9', 'R10', 'RBX', 'RAX')
OUTPUTS_MUL = ('R15', 'R13', 'R9', 'R10', 'RAX')


def depth(prog, mulw=1.0):
    rdy = {r: 0.0 for r in INPUTS}
    flag = {}
    cf = 0.0
    for op in prog:
        m = re.match(r'(\w+)\((.*)\)$', op)
        if not m:
            continue
        n, a = m.group(1), [x.strip() for x in m.group(2).split(',')]
        def R(x): return rdy.get(x, 0.0)
        if n == 'MUL_DSZ64_DRR':
            d, s0, s1 = a; t = max(R(s0), R(s1)) + mulw; rdy[s1] = t; rdy[d] = t
        elif n == 'MUL_DSZ64_DIR':
            d, _, s1 = a; t = R(s1) + mulw; rdy[s1] = t; rdy[d] = t
        elif n == 'IMUL64L_DSZ64_DRR':
            d, s0, s1 = a; rdy[d] = max(R(s0), R(s1)) + mulw
        elif n == 'IMUL64L_DSZ64_DRI':
            d, s0, _ = a; rdy[d] = R(s0) + mulw
        elif n in ('ADD_DSZ64_DRR', 'OR_DSZ64_DRR', 'AND_DSZ64_DRR', 'NOTAND_DSZ64_DRR'):
            d, s0, s1 = a; t = max(R(s0), R(s1)) + 1.0; rdy[d] = t
            if n == 'ADD_DSZ64_DRR': flag[d] = t
        elif n == 'ADD_DSZ64_DRI':
            d, s0, _ = a; t = R(s0) + 1.0; rdy[d] = t; flag[d] = t
        elif n in ('ADC_DSZ64_DRR',):
            d, s0, s1 = a; t = max(R(s0), R(s1), cf) + 1.0; rdy[d] = t; flag[d] = t
        elif n == 'ADC_DSZ64_DRI':
            d, s0, _ = a; t = max(R(s0), cf) + 1.0; rdy[d] = t; flag[d] = t
        elif n == 'GENARITHFLAGS_RR':
            s0, _ = a; cf = flag.get(s0, 0.0) + 1.0
        elif n == 'SETCC_CONDB_DR':
            d, s = a; rdy[d] = flag.get(s, 0.0) + 1.9
        elif n in ('ZEROEXT_DSZ64_DR',):
            d, s = a; rdy[d] = R(s) + 0.7
        elif n in ('ZEROEXT_DSZ32_DI', 'ZEROEXT_DSZ64_DI'):
            d, _ = a; rdy[d] = 0.0
        elif n in ('SHR_DSZ64_DRI', 'SHL_DSZ64_DRI'):
            d, s, _ = a; rdy[d] = R(s) + 1.0
    return rdy


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    path, arr = sys.argv[1], sys.argv[2]
    mulw = float(sys.argv[3]) if len(sys.argv) > 3 else 5.9
    ntri, prog = parse(path, arr)
    real = [o for o in prog if o != 'NOP']
    outs = OUTPUTS_SQ if 'sq' in arr else OUTPUTS_MUL
    for w, label in ((1.0, 'unit weights'), (mulw, 'MUL=%.1f' % mulw)):
        rdy = depth(prog, w)
        d = max(rdy.get(r, 0.0) for r in outs)
        nmul = len([o for o in real if o.startswith(('MUL_', 'IMUL64L_'))])
        indep = nmul * 0.91 + (len(real) - nmul) * 0.153
        print('%-12s %-14s triads=%-3d ops=%-4d depth=%6.1f   independent-issue=%.1f cyc'
              % (arr, label, ntri, len(real), d, indep))
    print('  independent-issue = n_mul*0.91 + n_other*0.153, the floor if the')
    print('  schedule exposed full parallelism (probe_issue, 2026-09-09).')
    print('  depth ABOVE that figure means the patch is dependency bound, which')
    print('  is where both production patches sit - see PLAN section 2.')


if __name__ == '__main__':
    main()
