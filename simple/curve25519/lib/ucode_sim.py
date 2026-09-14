#!/usr/bin/env python3
"""ucode_sim.py - execute a microcode patch array offline against a reference.

Parses a `ucode_t <name>[] = { ... }` array straight out of a C source file and
runs it on random inputs, including loose limbs up to 2^53 and all-ones edge
cases, comparing the result against a reference 5x51 multiply or square.

Run every candidate patch through this BEFORE it touches hardware. It costs
seconds, needs no root, and catches register-allocation errors that would
otherwise burn a root run. Both production patches pass exactly.

Usage:
  python3 lib/ucode_sim.py <file.c> <array_name> <sq|mul>

  python3 lib/ucode_sim.py full_curve25519_inline2.c sq_patch  sq
  python3 lib/ucode_sim.py full_curve25519_inline2.c mul_patch mul
  python3 lib/ucode_sim.py full_curve25519_inline2.c sq_patch_5acc sq --mask-r8

Raw opcodes with no inst.h macro (IMUL64L) are only understood if the C source
wraps them in a macro named like a normal op, e.g.

    #define IMUL64L_DSZ64_DRR(d,a,b) ((0x264UL << 32) | INSTR_DRR(d,a,b))
    #define IMUL64L_DSZ64_DRI(d,a,i) ((0x264UL << 32) | INSTR_DRI(d,a,i))

which is worth doing anyway so the patch source stays readable.
"""
import re, sys, random

M64 = (1 << 64) - 1
MASK51 = (1 << 51) - 1
P = 2**255 - 19

# Register file: 16 arch + 16 TMP. Values default to 0.
class Machine:
    def __init__(self):
        self.r = {}
        self.f = {}      # domain #1: per-register carry flag, set by ADD/ADC
        self.cf = 0      # domain #2: arch CF, written by GENARITHFLAGS, read by ADC

    def g(self, x):
        return self.r.get(x, 0)

    def run(self, op):
        m = re.match(r'(\w+)\((.*)\)$', op)
        if m is None:
            if op.strip() in ('NOP', ''):
                return
            raise SystemExit('unparsed op: ' + op)
        n, a = m.group(1), [x.strip() for x in m.group(2).split(',')]

        if n == 'MUL_DSZ64_DRR':                       # srcB gets lo, dst gets hi
            d, s0, s1 = a
            p = self.g(s0) * self.g(s1)
            self.r[s1] = p & M64
            self.r[d] = (p >> 64) & M64
        elif n == 'MUL_DSZ64_DIR':                     # srcB gets lo, dst gets hi
            d, imm, s1 = a
            p = int(imm, 0) * self.g(s1)
            self.r[s1] = p & M64
            self.r[d] = (p >> 64) & M64
        elif n == 'IMUL64L_DSZ64_DRR':                 # non-destructive low half
            d, s0, s1 = a
            self.r[d] = (self.g(s0) * self.g(s1)) & M64
        elif n == 'IMUL64L_DSZ64_DRI':
            d, s0, imm = a
            self.r[d] = (self.g(s0) * int(imm, 0)) & M64
        elif n == 'ADD_DSZ64_DRR':
            d, s0, s1 = a
            v = self.g(s0) + self.g(s1)
            self.r[d] = v & M64
            self.f[d] = 1 if v > M64 else 0
        elif n == 'ADD_DSZ64_DRI':
            d, s0, imm = a
            v = self.g(s0) + int(imm, 0)
            self.r[d] = v & M64
            self.f[d] = 1 if v > M64 else 0
        elif n == 'ADC_DSZ64_DRR':                     # reads arch CF
            d, s0, s1 = a
            v = self.g(s0) + self.g(s1) + self.cf
            self.r[d] = v & M64
            self.f[d] = 1 if v > M64 else 0
        elif n == 'ADC_DSZ64_DRI':
            d, s0, imm = a
            v = self.g(s0) + int(imm, 0) + self.cf
            self.r[d] = v & M64
            self.f[d] = 1 if v > M64 else 0
        elif n == 'GENARITHFLAGS_RR':                  # bridge domain #1 -> arch CF
            s0, s1 = a
            if s0 != s1:
                raise SystemExit('GENARITHFLAGS_RR must use the same register twice '
                                 '(GFL_RR(arch,arch) leaks CF on hardware): ' + op)
            self.cf = self.f.get(s0, 0)
        elif n == 'SETCC_CONDB_DR':
            d, s = a
            self.r[d] = self.f.get(s, 0)
        elif n == 'ZEROEXT_DSZ64_DR':
            d, s = a
            self.r[d] = self.g(s)
        elif n in ('ZEROEXT_DSZ32_DI', 'ZEROEXT_DSZ64_DI'):
            d, imm = a
            self.r[d] = int(imm, 0) & M64
        elif n == 'OR_DSZ64_DRR':
            d, s0, s1 = a
            self.r[d] = self.g(s0) | self.g(s1)
        elif n == 'AND_DSZ64_DRR':
            d, s0, s1 = a
            self.r[d] = self.g(s0) & self.g(s1)
        elif n == 'NOTAND_DSZ64_DRR':
            d, s0, s1 = a
            self.r[d] = (~self.g(s0)) & self.g(s1) & M64
        elif n == 'SHR_DSZ64_DRI':
            d, s, i = a
            self.r[d] = self.g(s) >> int(i, 0)
        elif n == 'SHL_DSZ64_DRI':
            d, s, i = a
            self.r[d] = (self.g(s) << int(i, 0)) & M64
        else:
            raise SystemExit('unknown op: ' + n)


def parse(path, arr):
    src = open(path).read()
    m = re.search(r'ucode_t\s+' + arr + r'\[\]\s*=\s*\{(.*?)\n\s*\};', src, re.S)
    if m is None:
        raise SystemExit('array %s not found in %s' % (arr, path))
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)
    triads = re.findall(r'\{(.*?)\},?\s*(?=\{|$)', body, re.S)
    prog = []
    for t in triads:
        out, depth, cur = [], 0, ''
        for ch in t:
            if ch == '(':
                depth += 1
            if ch == ')':
                depth -= 1
            if ch == ',' and depth == 0:
                out.append(cur.strip()); cur = ''
            else:
                cur += ch
        out.append(cur.strip())
        ops = [o for o in out if o]
        if len(ops) != 4:
            raise SystemExit('triad does not have 3 ops + seqword: %r' % (ops,))
        prog.extend(ops[:3])            # drop the seqword
    return len(triads), prog


def ref_mul(a, b):
    acc = [0] * 5
    for i in range(5):
        for j in range(5):
            k, c = i + j, a[i] * b[j]
            if k >= 5:
                k -= 5; c *= 19
            acc[k] += c
    carry, out = 0, [0] * 5
    for k in range(5):
        v = acc[k] + carry
        out[k] = v & MASK51
        carry = v >> 51
    out[0] += 19 * carry
    out[1] += out[0] >> 51
    out[0] &= MASK51
    return out


def canon(h):
    return sum(x << (51 * i) for i, x in enumerate(h)) % P


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    path, arr, kind = sys.argv[1], sys.argv[2], sys.argv[3]
    # The shipped fe_sq zeroes R8 and uses it as an accumulator. The parked
    # five-accumulator patch instead wants 2^51-1 there as an AND mask:
    #   python3 lib/ucode_sim.py <file> sq_patch_5acc sq --mask-r8
    mask_r8 = '--mask-r8' in sys.argv
    ntri, prog = parse(path, arr)
    real = [o for o in prog if o != 'NOP']
    print('%s: %d triads, %d real ops (%.2f ops/triad)'
          % (arr, ntri, len(real), len(real) / ntri))

    random.seed(20260909)
    bad = 0
    for trial in range(3000):
        if trial == 0:
            a = [0] * 5
        elif trial == 1:
            a = [MASK51] * 5
        elif trial == 2:
            a = [(1 << 52) - 1] * 5
        else:
            a = [random.getrandbits(random.choice([51, 52, 53])) for _ in range(5)]
        b = a if kind == 'sq' else \
            [random.getrandbits(random.choice([51, 52, 53])) for _ in range(5)]

        mm = Machine()
        if kind == 'sq':
            mm.r.update({'RDI': a[0], 'RSI': a[1], 'R12': a[2], 'R11': a[3],
                         'R14': a[4], 'R15': (2*a[0]) & M64, 'R13': (2*a[1]) & M64,
                         'R9': (2*a[2]) & M64, 'R10': (2*a[3]) & M64,
                         'RBX': (19*a[4]) & M64, 'RDX': (19*a[3]) & M64,
                         'RAX': 0, 'R8': MASK51 if mask_r8 else 0})
            # RCX is deliberately NOT seeded for sq: the fe_sq trigger is
            # `vmread rdx, rcx`, so rcx is the VMCS field-encoding operand and
            # the wrapper must not load anything into it. A patch that reads
            # RCX under this convention will read 0 here and garbage on
            # hardware, which lib/ucode_audit.py flags.
            outregs = ('RDI', 'R9', 'R10', 'RBX', 'RAX')
        else:
            mm.r.update({'RDI': a[0], 'RSI': a[1], 'R12': a[2], 'R11': a[3],
                         'R14': a[4], 'R15': b[0], 'R13': b[1], 'R9': b[2],
                         'R10': b[3], 'RBX': b[4], 'RAX': 0, 'R8': 0,
                         'RCX': MASK51})
            outregs = ('R15', 'R13', 'R9', 'R10', 'RAX')

        for op in prog:
            mm.run(op)
        got = [mm.g(x) for x in outregs]
        want = ref_mul(a, b)
        if canon(got) != canon(want):
            bad += 1
            if bad <= 3:
                print('  MISMATCH a=%s b=%s\n    got =%s\n    want=%s'
                      % (a, b, [hex(x) for x in got], [hex(x) for x in want]))
        if max(got) >= (1 << 53):
            print('  OUTPUT TOO LOOSE (limb >= 2^53) for a=%s -> %s' % (a, [hex(x) for x in got]))
            bad += 1
            break

    print('mismatches: %d / 3000  %s' % (bad, 'PASS' if bad == 0 else 'FAIL'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
