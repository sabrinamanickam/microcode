#!/usr/bin/env python3
# Generate the five-accumulator fe_mul microcode patch. SHIPPED VERSION.
#
# Source of truth for mul_patch[] in full_curve25519_inline2.c: emits
# mul_patch_body.txt, whose contents are the array body verbatim. Change the
# schedule HERE and re-paste; never hand-edit the C.
#
#     python3 lib/gen_mul_patch.py                 # -> mul_patch_body.txt
#     python3 lib/ucode_sim.py      full_curve25519_inline2.c mul_patch mul
#     python3 lib/ucode_critpath.py full_curve25519_inline2.c mul_patch
#     python3 lib/ucode_audit.py    full_curve25519_inline2.c mul_patch mul
#
# MEASURED, ALL ON HARDWARE, ALL WITH IDENTICAL SEMANTICS:
#
#   this file, SETCC carry + 1-deep pipeline   174 ops  58 triads  d=25.4  102.8  <- SHIPPED
#   gen_mul_patch_adc.py, ADC bridge           154 ops  52 triads  d=23.5  104.0
#   gen_mul_patch_adc.py, ACCUMULATE='setcc'   174 ops  58 triads  d=25.4  108.5
#   previous serial single accumulator         196 ops  66 triads  d=74.9  122.9
#
# Read rows 1 and 3 again: SAME op count, SAME triad count, SAME dependency
# depth, 5.7 cyc apart. The only difference is the order the ops are issued
# in and which scratch registers the allocator happened to hand out. So on
# this core, INSTRUCTION SCHEDULE AND REGISTER ASSIGNMENT ARE WORTH ~6% AT
# FIXED OP COUNT -- more than the ADC bridge was worth, and invisible to
# every model in PLAN_kernel_optimization.md.
#
# The shipped schedule interleaves one product deep: product k+1's multiply
# is issued between product k's multiply and the adds that consume it, and
# scratch rotates so no two products in flight share a register. Batching
# two products' multiplies together and then both accumulates (what
# gen_mul_patch_adc.py does with ACCUMULATE='setcc') is the 108.5 variant.
# If you touch the schedule, MEASURE IT. Do not infer it.
#
# It also audits register liveness: no operand may be read before it is
# written, because ucode_sim.py zero-fills registers and hardware does not.
import re

A    = ['RDI', 'RSI', 'R12', 'R11', 'R14']            # a0..a4, always MUL srcA (preserved)
B    = ['R15', 'R13', 'R9', 'R10', 'RBX']             # b0..b4, from the wrapper
G    = [None, 'TMP0', 'TMP1', 'TMP2', 'TMP3']         # g_j = 19*b_j, j=1..4
LO   = ['TMP4', 'TMP5', 'TMP6', 'TMP7', 'TMP8']       # acc lo -- MUST be TMP, SETCC reads it
HI   = ['TMP9', 'TMP10', 'TMP11', 'TMP12', 'TMP13']   # acc hi
MASK = 'RCX'                                          # 2^51-1, loaded by the wrapper
OUT  = ['R15', 'R13', 'R9', 'R10', 'RAX']             # h0..h4 back to the wrapper
SEEDED = set(A + B + ['RAX', 'R8', 'RCX'])            # what the wrapper guarantees

ops = []
emit = ops.append
marks = []
def mark(text):
    marks.append((len(ops), text))

# ── scratch allocator -----------------------------------------------------
# Registers are handed out round-robin and marked busy until the product
# that owns them has been fully accumulated, so no two products in flight
# ever share one.  A false dependency here would re-serialise exactly what
# the five-accumulator restructure exists to parallelise.
free_regs = ['RAX', 'R8', 'RDX']
busy = set()
cursor = [0]

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
            raise SystemExit('scratch exhausted')
        if r in out or r in busy:
            continue
        out.append(r)
        busy.add(r)
    return out

def release(regs):
    busy.difference_update(regs)

cpool, _cc = ['TMP14', 'TMP15'], [0]
def alloc_c():
    r = cpool[_cc[0] % len(cpool)]
    _cc[0] += 1
    return r

def bside(i, j):
    """b-side operand of a_i*b_j: pre-scaled by 19 when i+j >= 5."""
    return B[j] if i + j < 5 else G[j]

order = [(i, j) for i in range(5) for j in range(5)]          # row major
lastuse = {}
for k, (i, j) in enumerate(order):
    lastuse[bside(i, j)] = k

# ── PREP: g_j = 19*b_j, non-destructive (IMUL64L 0x264, probe_opsem [3]) ---
mark('PREP: g_j = 19*b_j, non-destructive, both sources survive')
for j in (4, 3, 2, 1):
    emit('IMUL64L_DSZ64_DRI(%s, %s, 19)' % (G[j], B[j]))

# ── row 0: a0*b_j initialises accumulator j, no ADD and no SETCC ----------
# MUL drops its low half into srcB, so staging b_j into lo_j IS the
# initialisation of the 128-bit accumulator (PLAN 5.1B).
mark('row 0: a0*b_j initialises accumulator j -- no ADD, no SETCC')
for j in range(5):
    emit('ZEROEXT_DSZ64_DR(%s, %s)' % (LO[j], B[j]))
    emit('MUL_DSZ64_DRR(%s, %s, %s)' % (HI[j], A[0], LO[j]))
add_free('RBX')                     # b4's only use is spent
add_free('RDI')                     # a0's five products are all issued

# ── rows 1..4: 20 accumulates on five independent SETCC carry chains ------
# Software-pipelined one product deep: product k+1's multiply is issued
# between product k's multiply and the adds that consume it.
prods = [(i, j) for (i, j) in order if i != 0]
pend = None
for (i, j) in prods + [(None, None)]:
    if i is not None:
        if j == 0:
            mark('row %d: a%d x b, five independent accumulates' % (i, i))
        acc, V = (i + j) % 5, bside(i, j)
        free = (lastuse[V] == order.index((i, j)))   # last use: MUL eats it, no copy
        if free:
            p = V
            h, = alloc(1)
            owned = [h]
        else:
            p, h = alloc(2)
            owned = [p, h]
            emit('ZEROEXT_DSZ64_DR(%s, %s)' % (p, V))
        emit('MUL_DSZ64_DRR(%s, %s, %s)' % (h, A[i], p))
        if j == 4:
            add_free(A[i])                           # row i's last product issued
        c = alloc_c()
        cur = ([
            'ADD_DSZ64_DRR(%s, %s, %s)' % (LO[acc], LO[acc], p),  # lo_j += lo(product)
            'SETCC_CONDB_DR(%s, %s)'    % (c, LO[acc]),           # its carry, domain #1
            'ADD_DSZ64_DRR(%s, %s, %s)' % (h, h, c),              # fold carry into hi(product)
            'ADD_DSZ64_DRR(%s, %s, %s)' % (HI[acc], HI[acc], h),  # hi_j += that
        ], owned, V if free else None)
    else:
        cur = None
    if pend is not None:
        ops.extend(pend[0])
        release(pend[1])
        if pend[2]:
            add_free(pend[2])
    pend = cur

# ── lazy reduction, pass 1 ------------------------------------------------
# acc_j = lo_j + 2^64*hi_j.  Split every accumulator in place; all five
# splits are mutually independent:
#     r_j = lo_j & MASK                          -> stays in LO[j]
#     q_j = acc_j >> 51 = (lo_j>>51)|(hi_j<<13)  -> stays in HI[j]
# q_j is limb j's carry and lands on limb j+1; q_4 wraps with a x19.
# This is where the patch departs from OpenSSL's reduction tree: OpenSSL
# propagates each carry into the *128-bit* accumulator (add + adc, 2 insns),
# which in microcode costs ADD + SETCC + ADD.  Splitting first makes every
# carry add a plain 64-bit ADD, and all five splits go in parallel.
mark('reduce pass 1: split all five accs, r_j = acc_j & M, q_j = acc_j >> 51')
red_tmp = ['TMP0', 'TMP1', 'TMP2']
for j in range(5):
    u = red_tmp[j % len(red_tmp)]
    emit('SHL_DSZ64_DRI(%s, %s, 13)' % (HI[j], HI[j]))
    emit('SHR_DSZ64_DRI(%s, %s, 51)' % (u, LO[j]))
    emit('AND_DSZ64_DRR(%s, %s, %s)' % (LO[j], LO[j], MASK))
    emit('OR_DSZ64_DRR(%s, %s, %s)'  % (HI[j], u, HI[j]))
mark('t_j = r_j + q_{j-1}, and t_0 = r_0 + 19*q_4')
for j in range(1, 5):
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (LO[j], LO[j], HI[j - 1]))     # t_j = r_j + q_{j-1}

w, x = 'TMP3', 'TMP14'
def times19(dst, addend, src):
    """dst = addend + 19*src, as shifts and adds: depth 4 against IMUL64L's
    5.9, and this sits on the patch's critical path out to h0 and h1."""
    emit('SHL_DSZ64_DRI(%s, %s, 4)'  % (w, src))                      # 16*src
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (x, src, src))                 #  2*src
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (w, w, x))                     # 18*src
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (w, w, src))                   # 19*src
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (dst, addend, w))
times19(LO[0], LO[0], HI[4])                                          # t_0 = r_0 + 19*q_4

# ── lazy reduction, pass 2 ------------------------------------------------
# t_j < 2^63.6 after pass 1, so one more parallel pass lands every limb at
# < 2^51 + 2^17.  s_j reuses HI[j], dead since the t_j adds above.
mark('reduce pass 2: same split again, t_j < 2^63.6 -> limbs < 2^51 + 2^17')
for j in range(5):
    emit('SHR_DSZ64_DRI(%s, %s, 51)' % (HI[j], LO[j]))                # s_j
    emit('AND_DSZ64_DRR(%s, %s, %s)' % (LO[j], LO[j], MASK))          # m_j
for j in range(1, 5):
    emit('ADD_DSZ64_DRR(%s, %s, %s)' % (OUT[j], LO[j], HI[j - 1]))    # h_j = m_j + s_{j-1}
times19(OUT[0], LO[0], HI[4])                                         # h_0 = m_0 + 19*s_4

# ── read-before-write audit ----------------------------------------------
written, bad = set(SEEDED), []
for o in ops:
    m = re.match(r'(\w+)\((.*)\)$', o)
    n, a = m.group(1), [t.strip() for t in m.group(2).split(',')]
    if n == 'MUL_DSZ64_DRR':
        srcs, dsts = [a[1], a[2]], [a[0], a[2]]
    elif n in ('IMUL64L_DSZ64_DRI', 'ZEROEXT_DSZ64_DR', 'SETCC_CONDB_DR',
               'SHL_DSZ64_DRI', 'SHR_DSZ64_DRI'):
        srcs, dsts = [a[1]], [a[0]]
    else:
        srcs, dsts = [a[1], a[2]], [a[0]]
    bad += [(o, s) for s in srcs if s not in written and not s.isdigit()]
    written |= set(dsts)
if bad:
    raise SystemExit('READ-BEFORE-WRITE: %r' % (bad,))

# ── emit C ---------------------------------------------------------------
ntri = (len(ops) + 2) // 3
lines, pending = [], list(marks)
for t in range(ntri):
    while pending and pending[0][0] < 3 * (t + 1):
        lines.append('    /* %s */' % pending.pop(0)[1])
    trio = ops[3 * t:3 * t + 3] + ['NOP'] * 3
    sw = 'END_SEQWORD' if t == ntri - 1 else 'NOP_SEQWORD'
    lines.append('    { %s, %s,\n      %s, %s },' % (trio[0], trio[1], trio[2], sw))
open('mul_patch_body.txt', 'w').write('\n'.join(lines) + '\n')

nmul = len([o for o in ops if o.startswith(('MUL_', 'IMUL'))])
print('ops=%d  n_mul=%d  n_other=%d  triads=%d' % (len(ops), nmul, len(ops) - nmul, ntri))
print('independent-issue floor = %.1f cyc' % (nmul * 0.91 + (len(ops) - nmul) * 0.153))
