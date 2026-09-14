#!/usr/bin/env python3
# Generate the five-accumulator fe_mul microcode patch.
#
# This is the source of truth for mul_patch[] in full_curve25519_inline2.c:
# it emits mul_patch_body.txt, whose contents are the array body verbatim.
# Change the schedule HERE and re-paste rather than editing the C.
#
#     python3 lib/gen_mul_patch.py                 # -> mul_patch_body.txt
#     python3 lib/ucode_sim.py      full_curve25519_inline2.c mul_patch mul
#     python3 lib/ucode_critpath.py full_curve25519_inline2.c mul_patch
#     python3 lib/ucode_audit.py    full_curve25519_inline2.c mul_patch mul
#
# COST MODEL (measured 2026-09-09, PLAN section 5.0-MEASURED). Three patches
# with very different dependency structure fit
#
#     body = 1.415 * triads + 0.178 * depth
#
# so a TRIAD costs 8x what a unit of dependency depth costs. Optimise triad
# count. Do not spend triads to buy depth -- this generator's first version
# did that twice and lost ~4 cyc for it.
#
# Levers not yet taken, in measured-payoff order: revert the two x19 chains
# to IMUL64L (-6 ops), swap the two-pass reduction for OpenSSL's tree
# (-10 ops, and the tree's 128-bit carries are cheap now that the ADC bridge
# is in), then the same treatment for fe_sq.
import re

A    = ['RDI', 'RSI', 'R12', 'R11', 'R14']            # a0..a4, always MUL srcA (preserved)
B    = ['R15', 'R13', 'R9', 'R10', 'RBX']             # b0..b4, from the wrapper
G    = [None, 'TMP0', 'TMP1', 'TMP2', 'TMP3']         # g_j = 19*b_j, j=1..4
LO   = ['TMP4', 'TMP5', 'TMP6', 'TMP7', 'TMP8']       # acc lo -- MUST be TMP, GFL_RR reads it
HI   = ['TMP9', 'TMP10', 'TMP11', 'TMP12', 'TMP13']   # acc hi
MASK = 'RCX'                                          # 2^51-1, loaded by the wrapper
OUT  = ['R15', 'R13', 'R9', 'R10', 'RAX']             # h0..h4 back to the wrapper
SEEDED = set(A + B + ['RAX', 'R8', 'RCX'])            # what the wrapper guarantees

MAX_INFLIGHT = 3          # products whose scratch is live at once (see below)

# How each accumulate folds its carry. MEASURED, both on hardware:
#
#   'setcc'  4 ops: ADD(lo_j,lo_j,p) SETCC(c,lo_j) ADD(h,h,c) ADD(hi_j,hi_j,h)
#            58 triads, 174 ops, body 86.6, total 102.8 cyc
#   'adc'    3 ops in one triad: ADD(lo_j,lo_j,p) GFL_RR(lo_j,lo_j) ADC(hi_j,hi_j,h)
#            52 triads, 154 ops, body 87.8, total 104.0 cyc   <- SLOWER
#
# The ADC bridge is a REGRESSION here, by 1.2 cyc, despite 20 fewer ops and
# 6 fewer triads. Removing 20 ops at the measured r should have saved 8.3
# cyc, so the ADC form costs ~0.47 cyc/accumulate more than the 4-op SETCC
# run it replaces.
#
# Why: SETCC reads domain #1, which is PER-REGISTER, so the five
# accumulators own five independent flag domains and their carry chains
# genuinely run in parallel. The bridge funnels all 20 carries through the
# ONE architectural CF. Triad-locality makes each group correct in
# isolation, but it does not make consecutive groups independent -- every
# accumulate now reads and writes the same flags register, so they
# serialise. PLAN 5.0 said "these are alternatives, not partners" and
# treated one-carry-in-flight as a correctness constraint that
# triad-locality satisfies; it is also a THROUGHPUT constraint, and
# triad-locality does not satisfy that one.
#
# lib/ucode_critpath.py cannot see this: it models the arch CF as a scalar
# that each GENARITHFLAGS overwrites, so consecutive bridges look mutually
# independent and it reported depth 23.5 for a schedule whose carries are
# ~20 deep through the flags register.
ACCUMULATE = 'setcc'      # 'setcc' (faster, shipped) or 'adc' (measured, slower)

# SETCC needs a third scratch register per product for the carry, and only
# five are free during row 1 (RAX R8 RDX + RBX and RDI once b4 and a0 die),
# so two products in flight is the limit there. ADC needs no carry
# register, which frees TMP14/TMP15 and allows three.
MAX_INFLIGHT = 2 if ACCUMULATE == 'setcc' else 3

# ── output is a list of triads; a triad is either 3 free-scheduled ops the
# packer chose, or an ATOMIC group that must occupy one triad exactly.
free_buf, triads = [], []

def emit(op):
    free_buf.append(op)

def flush(pad=True):
    """Pack buffered free ops into whole triads. With pad=False, keep a
    remainder of 1-2 ops buffered for the next batch instead of padding."""
    global free_buf
    n = len(free_buf) if pad else len(free_buf) - len(free_buf) % 3
    for i in range(0, n, 3):
        trio = free_buf[i:i + 3]
        triads.append(trio + ['NOP'] * (3 - len(trio)))
    free_buf = free_buf[n:]

def emit_atomic(trio):
    """One triad, exactly as given. The ADD/GFL/ADC carry bridge is only
    CONFIRMED in this configuration -- probe_carry [6] vs [7] had all three
    ops in a single triad, and O1 (does the bridge survive across triads?)
    is still open. Anything the group reads must already be flushed."""
    assert len(trio) == 3
    flush()
    triads.append(list(trio))

# ── scratch allocator: registers stay busy until the product that owns them
# has been accumulated, so no two in-flight products ever share one.
free_regs = (['RAX', 'R8', 'RDX'] if ACCUMULATE == 'setcc'
             else ['RAX', 'R8', 'RDX', 'TMP14', 'TMP15'])
cpool, _cc = ['TMP14', 'TMP15'], [0]      # SETCC destinations, kept TMP
busy, cursor = set(), [0]

def add_free(r):
    if r not in free_regs:
        free_regs.append(r)

def alloc(n):
    out = []
    guard = 0
    while len(out) < n:
        r = free_regs[cursor[0] % len(free_regs)]
        cursor[0] += 1
        guard += 1
        if guard > 100 * len(free_regs):
            raise SystemExit('scratch exhausted: %d in flight' % MAX_INFLIGHT)
        if r in out or r in busy:
            continue
        out.append(r)
        busy.add(r)
    return out

def bside(i, j):
    """b-side operand of a_i*b_j: pre-scaled by 19 when i+j >= 5."""
    return B[j] if i + j < 5 else G[j]

order = [(i, j) for i in range(5) for j in range(5)]          # row major
lastuse = {}
for k, (i, j) in enumerate(order):
    lastuse[bside(i, j)] = k

# ── PREP: g_j = 19*b_j, non-destructive (IMUL64L 0x264, probe_opsem [3]) ---
for j in (4, 3, 2, 1):
    emit('IMUL64L_DSZ64_DRI(%s, %s, 19)' % (G[j], B[j]))

# ── row 0: a0*b_j initialises accumulator j, no ADD and no carry ----------
# MUL drops its low half into srcB, so staging b_j into lo_j IS the
# initialisation of the 128-bit accumulator.
for j in range(5):
    emit('ZEROEXT_DSZ64_DR(%s, %s)' % (LO[j], B[j]))
    emit('MUL_DSZ64_DRR(%s, %s, %s)' % (HI[j], A[0], LO[j]))
add_free('RBX')                     # b4's only use is spent
add_free('RDI')                     # a0's five products are all issued

# ── rows 1..4: 20 accumulates, five independent 128-bit accumulators ------
# Products are issued in batches: all of a batch's multiplies, then all of
# its accumulate triads. Batches are sized so the multiplies fill whole
# triads wherever possible, because emit_atomic has to flush.
prods = [(i, j) for (i, j) in order if i != 0]
mulops = {p: (1 if lastuse[bside(*p)] == order.index(p) else 2) for p in prods}

# Batch boundaries by DP, minimising padding. emit_atomic must flush, so a
# batch whose multiplies do not top the buffer up to a whole triad wastes
# slots -- and a wasted triad is 1.4 cyc under the measured model. The
# buffer carries 14 ops (PREP + row 0) into the first batch, and the
# reduction's 48 ops continue the buffer after the last, so only the
# batch boundaries cost anything. 46 free ops before the reduction means
# total padding is >= 2 slots and this reaches it.
def pad_for(x):
    return -x % 3

from functools import lru_cache
@lru_cache(maxsize=None)
def best(i, rem):
    if i == len(prods):
        return (0, ())
    out = None
    for s in range(1, MAX_INFLIGHT + 1):
        if i + s > len(prods):
            break
        n = sum(mulops[q] for q in prods[i:i + s])
        cost = pad_for(rem + n)
        sub, tail = best(i + s, 0)
        cand = (cost + sub, (s,) + tail)
        if out is None or cand < out:
            out = cand
    return out

_pad, sizes = best(0, len(free_buf) % 3) if ACCUMULATE == 'adc' else \
              (0, tuple([MAX_INFLIGHT] * (len(prods) // MAX_INFLIGHT)
                        + ([len(prods) % MAX_INFLIGHT] if len(prods) % MAX_INFLIGHT else [])))
batches, at = [], 0
for s in sizes:
    batches.append(prods[at:at + s]); at += s

for batch in batches:
    staged = []
    for (i, j) in batch:
        acc, V = (i + j) % 5, bside(i, j)
        if lastuse[V] == order.index((i, j)):     # last use: MUL eats it, no copy
            p, owned = V, []
            h, = alloc(1)
        else:
            p, h = alloc(2)
            owned = [p]
            emit('ZEROEXT_DSZ64_DR(%s, %s)' % (p, V))
        emit('MUL_DSZ64_DRR(%s, %s, %s)' % (h, A[i], p))
        if j == 4:
            add_free(A[i])                        # row i's last product issued
        staged.append((acc, p, h, owned + [h], V if p == V else None))
    if ACCUMULATE == 'setcc':
        flush(pad=False)
    for acc, p, h, owned, freed in staged:
        # The carry bridge, three ops in one triad (PLAN 5.1A):
        #   ADD sets lo_j's domain-#1 flag, GENARITHFLAGS_RR copies it into
        #   the arch CF, ADC consumes it. Both registers are TMP, which is
        #   required -- GFL_RR(arch, arch) leaks CF. Only one bridged carry
        #   can be in flight, which is why the group is triad-local.
        if ACCUMULATE == 'adc':
            emit_atomic([
                'ADD_DSZ64_DRR(%s, %s, %s)' % (LO[acc], LO[acc], p),
                'GENARITHFLAGS_RR(%s, %s)'  % (LO[acc], LO[acc]),
                'ADC_DSZ64_DRR(%s, %s, %s)' % (HI[acc], HI[acc], h),
            ])
        else:
            c = cpool[_cc[0] % len(cpool)]; _cc[0] += 1
            emit('ADD_DSZ64_DRR(%s, %s, %s)' % (LO[acc], LO[acc], p))
            emit('SETCC_CONDB_DR(%s, %s)'    % (c, LO[acc]))
            emit('ADD_DSZ64_DRR(%s, %s, %s)' % (h, h, c))      # carry into the product's hi
            emit('ADD_DSZ64_DRR(%s, %s, %s)' % (HI[acc], HI[acc], h))
        busy.difference_update(owned)
        if freed:
            add_free(freed)

# ── lazy reduction, pass 1 ------------------------------------------------
# acc_j = lo_j + 2^64*hi_j. Split every accumulator in place, all five
# independent:  r_j = lo_j & MASK   (stays in LO[j])
#               q_j = acc_j >> 51 = (lo_j>>51)|(hi_j<<13)   (stays in HI[j])
# q_j is limb j's carry and lands on limb j+1; q_4 wraps with a x19.
red_tmp = ['TMP0', 'TMP1', 'TMP2']
for j in range(5):
    u = red_tmp[j % len(red_tmp)]
    emit('SHL_DSZ64_DRI(%s, %s, 13)' % (HI[j], HI[j]))
    emit('SHR_DSZ64_DRI(%s, %s, 51)' % (u, LO[j]))
    emit('AND_DSZ64_DRR(%s, %s, %s)' % (LO[j], LO[j], MASK))
    emit('OR_DSZ64_DRR(%s, %s, %s)'  % (HI[j], u, HI[j]))
for j in range(1, 5):
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (LO[j], LO[j], HI[j - 1]))     # t_j = r_j + q_{j-1}

w, x = 'TMP3', 'TMP14'
def times19(dst, addend, src):
    emit('SHL_DSZ64_DRI(%s, %s, 4)'  % (w, src))                      # 16*src
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (x, src, src))                 #  2*src
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (w, w, x))                     # 18*src
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (w, w, src))                   # 19*src
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (dst, addend, w))
times19(LO[0], LO[0], HI[4])                                          # t_0 = r_0 + 19*q_4

# ── lazy reduction, pass 2 ------------------------------------------------
# t_j < 2^63.6 after pass 1, so one more parallel pass lands every limb at
# < 2^51 + 2^17.  s_j reuses HI[j], dead since the t_j adds above.
for j in range(5):
    emit('SHR_DSZ64_DRI(%s, %s, 51)' % (HI[j], LO[j]))                # s_j
    emit('AND_DSZ64_DRR(%s, %s, %s)' % (LO[j], LO[j], MASK))          # m_j
for j in range(1, 5):
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (OUT[j], LO[j], HI[j - 1]))    # h_j = m_j + s_{j-1}
times19(OUT[0], LO[0], HI[4])                                         # h_0 = m_0 + 19*s_4
flush()

# ── read-before-write audit ----------------------------------------------
ops = [o for t in triads for o in t if o != 'NOP']
written, bad = set(SEEDED), []
for o in ops:
    m = re.match(r'(\w+)\((.*)\)$', o)
    n, a = m.group(1), [t.strip() for t in m.group(2).split(',')]
    if n == 'MUL_DSZ64_DRR':
        srcs, dsts = [a[1], a[2]], [a[0], a[2]]
    elif n == 'GENARITHFLAGS_RR':
        srcs, dsts = [a[0]], []
    elif n in ('IMUL64L_DSZ64_DRI', 'ZEROEXT_DSZ64_DR', 'SHL_DSZ64_DRI',
               'SHR_DSZ64_DRI', 'SETCC_CONDB_DR'):
        srcs, dsts = [a[1]], [a[0]]
    else:
        srcs, dsts = [a[1], a[2]], [a[0]]
    bad += [(o, s) for s in srcs if s not in written and not s.isdigit()]
    written |= set(dsts)
if bad:
    raise SystemExit('READ-BEFORE-WRITE: %r' % (bad,))

# ── emit C ---------------------------------------------------------------
lines = []
for t, trio in enumerate(triads):
    sw = 'END_SEQWORD' if t == len(triads) - 1 else 'NOP_SEQWORD'
    lines.append('    { %s, %s,\n      %s, %s },' % (trio[0], trio[1], trio[2], sw))
open('mul_patch_body.txt', 'w').write('\n'.join(lines) + '\n')

nmul = len([o for o in ops if o.startswith(('MUL_', 'IMUL'))])
pad = 3 * len(triads) - len(ops)
print('ops=%d  n_mul=%d  n_other=%d  triads=%d  (padding slots: %d)'
      % (len(ops), nmul, len(ops) - nmul, len(triads), pad))
print('accumulate form: %s' % ACCUMULATE)
