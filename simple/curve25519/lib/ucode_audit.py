#!/usr/bin/env python3
"""ucode_audit.py - hardware-rule audit for a microcode patch array.

ucode_sim.py checks a patch computes the right answer; ucode_critpath.py
checks it is not dependency bound. Neither can see the rules below, because
the simulator zero-fills registers and models flags perfectly. Each of these
is a real Goldmont constraint from CLAUDE.md / the project notes:

  1. READ-BEFORE-WRITE. The simulator starts every register at 0; hardware
     starts them at whatever the last instruction left there. Only the
     registers the wrapper marshals are defined on entry, so any other
     register must be written before it is read or sim and hardware diverge
     silently.
  2. SETCC SOURCE CLASS. SETCC_CONDB_DR reads domain #1, the per-register
     condition state that ADD leaves behind. Arch registers hold no such
     state (project note setcc-arch-dest-ok: the restriction is on the READ,
     not on SETCC's destination), so the source must be a TMP.
  3. FLAG FRESHNESS. A domain #1 flag lives until the next ADD to the same
     register. A SETCC must therefore see no intervening ADD to its source
     between the ADD that set the flag and itself.
  4. ONE MEMORY OP PER TRIAD. Mass-packing two crashed the machine.
  5. SEQWORD PLACEMENT. Exactly one END_SEQWORD, on the last triad.
  6. CARRY BRIDGE SHAPE. GENARITHFLAGS_RR must name the same TMP twice.
     GFL_RR(arch, arch) leaks CF (project note no-cf-bridge), and the
     two-different-registers form is not a bridge at all.
  7. CARRY BRIDGE LOCALITY. ADD / GENARITHFLAGS_RR / ADC must sit in ONE
     triad. That is the only configuration confirmed on hardware
     (probe_carry [6] vs [7]); whether the bridge survives across a triad
     boundary is open question O1. ucode_sim.py models the arch CF as a
     plain variable and so passes a non-local bridge that hardware may not,
     which is exactly the divergence this rule exists to catch.

NOTAND_DSZ64_DRR(x, x, x) is exempt from rule 1: (~x)&x is 0 for any x, so
it is the register-zeroing idiom rather than a read of undefined state.

Usage:
  python3 lib/ucode_audit.py <file.c> <array_name> [mul|sq]
"""
import re, sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from ucode_sim import parse

# What each wrapper guarantees is loaded before the hooked instruction.
# What each wrapper guarantees is loaded before the hooked instruction. Keep
# this in step with FE_MUL / FE_SQ: both now load 2^51-1 into RCX so the
# patches can mask with a single AND, and fe_sq additionally gets
# d_i = 2*a_i in R15/R13/R9/R10 and 19*a3 in RDX, 19*a4 in RBX.
ENTRY = {
    'mul': set('RDI RSI R12 R11 R14 R15 R13 R9 R10 RBX RAX R8 RCX'.split()),
    # fe_sq deliberately omits RCX: its trigger is `vmread rdx, rcx`, so RCX
    # is the VMCS field-encoding operand and the wrapper leaves it alone.
    # The mask lives in R8 instead.
    'sq':  set('RDI RSI R12 R11 R14 R15 R13 R9 R10 RBX RDX RAX R8'.split()),
}
TMP = set('TMP%d' % i for i in range(16))
MEMOPS = ('LDZX', 'LDST', 'STAD', 'LDSTGBUF', 'STSTGBUF')

# srcs, dsts per op form. MUL_DSZ64_DRR's srcB is both: it receives the low half.
def operands(n, a):
    if n == 'MUL_DSZ64_DRR':
        return [a[1], a[2]], [a[0], a[2]]
    if n == 'MUL_DSZ64_DIR':
        return [a[2]], [a[0], a[2]]
    if n in ('IMUL64L_DSZ64_DRI', 'ZEROEXT_DSZ64_DR', 'SETCC_CONDB_DR',
             'SHL_DSZ64_DRI', 'SHR_DSZ64_DRI', 'ADD_DSZ64_DRI'):
        return [a[1]], [a[0]]
    if n in ('ZEROEXT_DSZ32_DI', 'ZEROEXT_DSZ64_DI'):
        return [], [a[0]]
    if n == 'GENARITHFLAGS_RR':
        return [a[0], a[1]], []
    return [a[1], a[2]], [a[0]]                      # every _DRR three-operand form


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    path, arr = sys.argv[1], sys.argv[2]
    kind = sys.argv[3] if len(sys.argv) > 3 else ('sq' if 'sq' in arr else 'mul')
    ntri, prog = parse(path, arr)
    entry = ENTRY[kind]

    src = open(path).read()
    m = re.search(r'ucode_t\s+' + arr + r'\[\]\s*=\s*\{(.*?)\n\s*\};', src, re.S)
    seqwords = re.findall(r'(\w*SEQWORD\w*|END_SEQWORD)\s*\}', m.group(1))

    written, flag_from, errs, warns = set(entry), {}, [], []
    nmem = [0] * ntri
    for idx, op in enumerate(prog):
        mm = re.match(r'(\w+)\((.*)\)$', op)
        if mm is None:
            continue
        n, a = mm.group(1), [t.strip() for t in mm.group(2).split(',')]
        srcs, dsts = operands(n, a)
        if n == 'NOTAND_DSZ64_DRR' and a[0] == a[1] == a[2]:
            srcs = []            # (~x)&x == 0 for any x: the register-zeroing idiom
        if n.startswith(MEMOPS):
            nmem[idx // 3] += 1
        for s in srcs:
            if s not in written and not s.isdigit():
                errs.append('op %d %s: reads %s before any write' % (idx, op, s))
        if n == 'SETCC_CONDB_DR':
            if a[1] not in TMP:
                errs.append('op %d %s: SETCC source %s is not a TMP '
                            '(arch registers carry no domain #1 state)' % (idx, op, a[1]))
            elif a[1] not in flag_from:
                errs.append('op %d %s: SETCC source %s has no live ADD flag' % (idx, op, a[1]))
        for d in dsts:
            written.add(d)
        if n.startswith(('ADD_DSZ64', 'ADC_DSZ64', 'SUB_DSZ64')):
            flag_from[dsts[0]] = idx
        elif n != 'SETCC_CONDB_DR':
            for d in dsts:
                flag_from.pop(d, None)               # non-ADD write kills the flag

    # rules 6 and 7: the ADD/GFL/ADC carry bridge
    for t in range(ntri):
        trio = [prog[3 * t + k] if 3 * t + k < len(prog) else 'NOP' for k in range(3)]
        names = []
        for op in trio:
            mm = re.match(r'(\w+)\((.*)\)$', op)
            names.append((mm.group(1), [x.strip() for x in mm.group(2).split(',')])
                         if mm else (op.strip(), []))
        for k, (n, a) in enumerate(names):
            if n == 'GENARITHFLAGS_RR':
                if len(a) != 2 or a[0] != a[1]:
                    errs.append('triad %d: %s must name the same register twice' % (t, trio[k]))
                elif a[0] not in TMP:
                    errs.append('triad %d: %s bridges through an arch register, '
                                'which leaks CF' % (t, trio[k]))
                # the flag it bridges must be set by an ADD earlier in THIS triad
                if not any(nn.startswith(('ADD_DSZ64', 'ADC_DSZ64')) and aa and aa[0] == a[0]
                           for nn, aa in names[:k]):
                    errs.append('triad %d: %s has no ADD to %s earlier in the same '
                                'triad (rule 7)' % (t, trio[k], a[0]))
            if n.startswith('ADC_DSZ64'):
                if not any(nn == 'GENARITHFLAGS_RR' for nn, _ in names[:k]):
                    errs.append('triad %d: %s reads the arch CF with no '
                                'GENARITHFLAGS_RR earlier in the same triad (rule 7, '
                                'open question O1)' % (t, trio[k]))

    for t, c in enumerate(nmem):
        if c > 1:
            errs.append('triad %d: %d memory ops (limit is 1)' % (t, c))
    if seqwords.count('END_SEQWORD') != 1 or seqwords[-1] != 'END_SEQWORD':
        errs.append('END_SEQWORD must appear exactly once, on the last triad; got %r'
                    % (seqwords[-3:],))

    print('%s: %d triads, %d ops, entry set %s' % (arr, ntri, len(prog), kind))
    for e in errs:
        print('  ERROR  ' + e)
    for w in warns:
        print('  warn   ' + w)
    print('audit: %s' % ('PASS' if not errs else 'FAIL (%d)' % len(errs)))
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
