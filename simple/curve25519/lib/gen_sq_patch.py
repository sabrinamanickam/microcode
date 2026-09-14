#!/usr/bin/env python3
# Generate the five-accumulator fe_sq microcode patch.
#
# Source of truth for sq_patch[] in full_curve25519_inline2.c. Same structure
# as gen_mul_patch.py, which is deliberate: that schedule shape measured
# 102.8 for fe_mul and a differently-shaped one with an identical census
# measured 108.5, so the shape is copied rather than re-invented.
#
#     python3 lib/gen_sq_patch.py                  # -> sq_patch_body.txt
#     python3 lib/ucode_sim.py      full_curve25519_inline2.c sq_patch sq
#     python3 lib/ucode_critpath.py full_curve25519_inline2.c sq_patch
#     python3 lib/ucode_audit.py    full_curve25519_inline2.c sq_patch sq
#
# COST MODEL (probe_sched, 2026-09-09; PLAN section 9a):
#     body = n_mul * 1.33 + n_other * 0.332   + clustering + stalls
# An independent ALU op is 0.332 cyc (3.01/cycle on a 3-wide core), NOT the
# 0.153 probe_issue reported -- that figure implied 6.5 ops/cycle and was
# wrong. Multiplies cost 1.33 spread and 1.96 clustered, so keep at most one
# per triad. Registers are fully renamed (probe_sched regs sweep is flat from
# R=1 to R=16), so WAW/WAR false dependencies are free and scratch rotation
# is cosmetic; it is kept only because it costs nothing.
#
# THE 15 PRODUCTS. The FE_SQ wrapper precomputes d_i = 2*a_i for i=0..3 and
# n3 = 19*a3, n4 = 19*a4, so every product below is one multiply with no
# scaling multiplies at all (fe_mul needs four):
#
#   h0 = a0*a0 + d1*n4 + d2*n3
#   h1 = d0*a1 + d2*n4 + a3*n3
#   h2 = d0*a2 + a1*a1 + d3*n4
#   h3 = d0*a3 + d1*a2 + a4*n4
#   h4 = d0*a4 + d1*a3 + a2*a2
#
# Three of them square a single value (a0*a0, a1*a1, a2*a2); MUL_DSZ64_DRR
# with srcA == srcB is fine and the previous patch already relied on it.
import re

A    = ['RDI', 'RSI', 'R12', 'R11', 'R14']            # a0..a4
D    = ['R15', 'R13', 'R9', 'R10']                    # d_i = 2*a_i, i=0..3
N3, N4 = 'RDX', 'RBX'                                 # 19*a3, 19*a4
LO   = ['TMP4', 'TMP5', 'TMP6', 'TMP7', 'TMP8']       # acc lo -- MUST be TMP (SETCC)
HI   = ['TMP9', 'TMP10', 'TMP11', 'TMP12', 'TMP13']   # acc hi
# 2^51-1, loaded by the wrapper. NOT RCX: the fe_sq trigger is
# `.byte 0f 78 ca` = `vmread rdx, rcx`, so RCX is the VMCS field-encoding
# SOURCE operand and RDX the destination. Loading a chosen value into RCX
# before that instruction is the one change in the crashed run that had no
# precedent anywhere in the tree, so the mask lives in R8, which the old
# wrapper already wrote (as zero) before the same trigger. The new patch
# needs neither of the wrapper's zeros -- every scratch register is written
# before it is read, which lib/ucode_audit.py checks.
MASK = 'R8'
OUT  = ['RDI', 'R9', 'R10', 'RBX', 'RAX']             # h0..h4 back to the wrapper
SEEDED = set(A + D + [N3, N4, 'RAX', 'R8'])             # RCX is NOT ours to read

REG = {'a0':A[0],'a1':A[1],'a2':A[2],'a3':A[3],'a4':A[4],
       'd0':D[0],'d1':D[1],'d2':D[2],'d3':D[3],'n3':N3,'n4':N4}

PRODS = {                          # (srcA, srcB); srcB is the one MUL destroys
 0: [('a0','a0'), ('d1','n4'), ('d2','n3')],
 1: [('d0','a1'), ('d2','n4'), ('a3','n3')],
 2: [('d0','a2'), ('a1','a1'), ('d3','n4')],
 3: [('d0','a3'), ('d1','a2'), ('a4','n4')],
 4: [('d0','a4'), ('d1','a3'), ('a2','a2')],
}
# Issue order: one product per accumulator in rotation, so adjacent products
# are always independent. Chosen so that each value's LAST use is as srcB
# wherever possible, which is what makes a staging copy unnecessary there.
ORDER = [(0,2),(1,0),(2,0),(3,0),(4,0),      # initialise the five accumulators
         (0,0),(1,2),(2,1),(3,1),(4,1),
         (0,1),(1,1),(2,2),(3,2),(4,2)]

lastuse = {}
for pos, (k, i) in enumerate(ORDER):
    for nm in PRODS[k][i]:
        lastuse[nm] = max(lastuse.get(nm, -1), pos)

ops = []
emit = ops.append

free_regs = ['RAX', 'TMP0', 'TMP1', 'TMP2', 'TMP3']      # R8 now holds the mask
busy, cursor = set(), [0]
def add_free(r):
    if r not in free_regs:
        free_regs.append(r)
def alloc(n):
    out, guard = [], 0
    while len(out) < n:
        r = free_regs[cursor[0] % len(free_regs)]
        cursor[0] += 1
        guard += 1
        if guard > 100 * len(free_regs):
            raise SystemExit('scratch exhausted')
        if r in out or r in busy:
            continue
        out.append(r); busy.add(r)
    return out
cpool, _cc = ['TMP14', 'TMP15'], [0]

# ── five accumulator initialisations, then ten accumulates ──────────────
# MUL writes its low half into srcB, so staging a value into lo_j IS the
# initialisation of accumulator j: no ADD and no SETCC for the first product.
#
# Allocation and emission run in ONE pass, so a register is released the
# moment the product that owns it has been accumulated and is available to
# the next product. Splitting them into two loops means releases never
# happen while allocating, which exhausts the pool after three products.
pend = None
for pos, (k, i) in enumerate(ORDER + [(None, None)]):
    cur = None
    if k is not None:
        sa, sb = PRODS[k][i]
        ra, rb = REG[sa], REG[sb]
        if pos < 5:                                   # initialise accumulator k
            emit('ZEROEXT_DSZ64_DR(%s, %s)' % (LO[k], rb))
            emit('MUL_DSZ64_DRR(%s, %s, %s)' % (HI[k], ra, LO[k]))
        else:
            if lastuse[sb] == pos:        # last use of the value: MUL may eat it
                p, owned, freed = rb, [], rb
                h, = alloc(1)
                owned = [h]
            else:
                p, h = alloc(2)
                owned, freed = [p, h], None
                emit('ZEROEXT_DSZ64_DR(%s, %s)' % (p, rb))
            emit('MUL_DSZ64_DRR(%s, %s, %s)' % (h, ra, p))
            c = cpool[_cc[0] % len(cpool)]; _cc[0] += 1
            cur = ([
                'ADD_DSZ64_DRR(%s, %s, %s)' % (LO[k], LO[k], p),   # lo_k += lo(product)
                'SETCC_CONDB_DR(%s, %s)'    % (c, LO[k]),          # its carry, domain #1
                'ADD_DSZ64_DRR(%s, %s, %s)' % (h, h, c),           # fold into product hi
                'ADD_DSZ64_DRR(%s, %s, %s)' % (HI[k], HI[k], h),   # hi_k += that
            ], owned, freed)
    if pend is not None:
        ops.extend(pend[0])
        busy.difference_update(pend[1])
        if pend[2]:
            add_free(pend[2])
    pend = cur

# ── lazy reduction: two fully parallel passes, as in gen_mul_patch.py ────
red_tmp = ['TMP0', 'TMP1', 'TMP2']
for j in range(5):
    u = red_tmp[j % len(red_tmp)]
    emit('SHL_DSZ64_DRI(%s, %s, 13)' % (HI[j], HI[j]))
    emit('SHR_DSZ64_DRI(%s, %s, 51)' % (u, LO[j]))
    emit('AND_DSZ64_DRR(%s, %s, %s)' % (LO[j], LO[j], MASK))
    emit('OR_DSZ64_DRR(%s, %s, %s)'  % (HI[j], u, HI[j]))
for j in range(1, 5):
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (LO[j], LO[j], HI[j - 1]))
w, x = 'TMP3', 'TMP14'
def times19(dst, addend, src):
    emit('SHL_DSZ64_DRI(%s, %s, 4)'  % (w, src))
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (x, src, src))
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (w, w, x))
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (w, w, src))
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (dst, addend, w))
times19(LO[0], LO[0], HI[4])
for j in range(5):
    emit('SHR_DSZ64_DRI(%s, %s, 51)' % (HI[j], LO[j]))
    emit('AND_DSZ64_DRR(%s, %s, %s)' % (LO[j], LO[j], MASK))
for j in range(1, 5):
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (OUT[j], LO[j], HI[j - 1]))
times19(OUT[0], LO[0], HI[4])

# ── read-before-write audit ─────────────────────────────────────────────
written, bad = set(SEEDED), []
for o in ops:
    m = re.match(r'(\w+)\((.*)\)$', o)
    n, a = m.group(1), [t.strip() for t in m.group(2).split(',')]
    if n == 'MUL_DSZ64_DRR':
        srcs, dsts = [a[1], a[2]], [a[0], a[2]]
    elif n in ('ZEROEXT_DSZ64_DR','SHL_DSZ64_DRI','SHR_DSZ64_DRI','SETCC_CONDB_DR'):
        srcs, dsts = [a[1]], [a[0]]
    else:
        srcs, dsts = [a[1], a[2]], [a[0]]
    bad += [(o, s) for s in srcs if s not in written and not s.isdigit()]
    written |= set(dsts)
if bad:
    raise SystemExit('READ-BEFORE-WRITE: %r' % (bad,))

# ── emit C, and report the multiply-per-triad histogram (PLAN O6) ───────
# Pack three ops per triad but NEVER two multiplies in one. probe_sched
# measured clustered multiplies at 1.96 cyc against 1.328 spread (+48%), so
# a NOP costs 0.332 and buys back 0.64; and the version of this patch that
# crashed the machine had a three-multiply triad, which is not something any
# validated patch in the tree contains.
packed, cur = [], []
for o in ops:
    ismul = o.startswith('MUL_')
    if len(cur) == 3 or (ismul and any(x.startswith('MUL_') for x in cur)):
        packed.append(cur + ['NOP'] * (3 - len(cur)))
        cur = []
    cur.append(o)
if cur:
    packed.append(cur + ['NOP'] * (3 - len(cur)))
ntri = len(packed)
lines = []
for t, trio in enumerate(packed):
    sw = 'END_SEQWORD' if t == ntri - 1 else 'NOP_SEQWORD'
    lines.append('    { %s, %s,\n      %s, %s },' % (trio[0], trio[1], trio[2], sw))
open('sq_patch_body.txt', 'w').write('\n'.join(lines) + '\n')

import collections
h = collections.Counter()
for trio in packed:
    h[sum(1 for o in trio if o.startswith('MUL_'))] += 1
nmul = len([o for o in ops if o.startswith('MUL_')])
print('ops=%d  n_mul=%d  n_other=%d  triads=%d' % (len(ops), nmul, len(ops)-nmul, ntri))
print('MULs per triad: %s   (triads with >=2: %d -- clustering costs +48%%/MUL)'
      % (dict(sorted(h.items())), sum(v for k, v in h.items() if k >= 2)))
print('predicted body = %.1f cyc, total %.1f  (OpenSSL fe_sq 73.2)'
      % (nmul*1.33 + (len(ops)-nmul)*0.332, nmul*1.33 + (len(ops)-nmul)*0.332 + 16.18))
