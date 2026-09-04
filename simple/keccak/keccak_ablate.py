#!/usr/bin/env python3
"""
keccak_ablate.py — generate ablated variants of the Keccak microcode kernel.

The paper attributes the speedup to four design choices. An attribution argument
is not evidence, so this script builds a variant of the kernel with each choice
individually removed, verifies every variant against the reference permutation
and the published KAT vectors, and emits each one as its own patch body. The
benchmark harness then installs them one at a time in a single process and times
them against the baseline, which turns the attribution into a measurement.

Each variant changes exactly ONE thing and holds everything else — register
allocation, packing rules, loop structure — constant. Variants that no longer fit
in the 128-triad patch RAM are reported as capacity results rather than estimated
cycle counts.

Run:  python3 keccak_ablate.py      (writes abl/*.h, prints the triad table)
"""
import os, sys
import keccak_gen as G
from keccak_gen import emit, OFF, REG, RHO, D_REG, col, pi_cycles, DSCR, COUNTER, RCTAB, BASE_LANE

OUT = "abl"

# ─────────────────────────── round phases ───────────────────────────
# Each phase is a function so a variant can replace one and inherit the rest.

def theta_c_balanced(ops):
    """Baseline: depth-3 balanced XOR tree, RSP as the second accumulator."""
    for x in range(5):
        emit(ops,"XOR", "RAX", REG[x],    REG[x+5])
        emit(ops,"XOR", "RSP", REG[x+10], REG[x+15])
        emit(ops,"XOR", "RAX", "RAX", "RSP")
        emit(ops,"XOR", "RAX", "RAX", REG[x+20])
        emit(ops,"ST",  "RAX", OFF(DSCR+x))

def theta_c_serial(ops):
    """Ablation: serial reduction, depth 4. Same op count, longer chain."""
    for x in range(5):
        emit(ops,"XOR", "RAX", REG[x], REG[x+5])
        emit(ops,"XOR", "RAX", "RAX", REG[x+10])
        emit(ops,"XOR", "RAX", "RAX", REG[x+15])
        emit(ops,"XOR", "RAX", "RAX", REG[x+20])
        emit(ops,"ST",  "RAX", OFF(DSCR+x))

def theta_d(ops):
    """D[x] = C[x-1] ^ rol(C[x+1],1), into the five resident D registers."""
    for x in range(5):
        emit(ops,"LD",  "RSP", OFF(DSCR+(x+1)%5))
        emit(ops,"ROL", "RSP", "RSP", 1)
        emit(ops,"LD",  D_REG[x], OFF(DSCR+(x+4)%5))
        emit(ops,"XOR", D_REG[x], D_REG[x], "RSP")

def theta_d_spilled(ops):
    """Ablation: same computation, then spill D to the (now dead) C lanes."""
    theta_d(ops)
    for x in range(5):
        emit(ops,"ST", D_REG[x], OFF(DSCR+x))

def apply_rho_pi(ops, d_in_reg=True):
    """Fused theta-apply + rho + pi as one in-place cycle walk.

    d_in_reg: read D from a register (baseline) or reload it from memory per
    lane (ablation). Everything else is identical, so the difference isolates
    exactly what D residency buys.
    """
    def dsrc(ops, src):
        if d_in_reg:
            return D_REG[col(src)]
        # D_REG[0] is free here: this variant keeps D in memory, not registers.
        emit(ops,"LD",D_REG[0],OFF(DSCR+col(src)))
        return D_REG[0]
    for cyc in pi_cycles():
        if len(cyc)==1:
            i=cyc[0]
            emit(ops,"XOR", REG[i], REG[i], dsrc(ops,i))
            if RHO[i]: emit(ops,"ROL", REG[i], REG[i], RHO[i])
            continue
        L=len(cyc)
        emit(ops,"MOV", "RSP", REG[cyc[L-1]])
        for k in range(L-1,0,-1):
            dst,src=cyc[k],cyc[k-1]
            emit(ops,"XOR", REG[dst], REG[src], dsrc(ops,src))
            if RHO[src]: emit(ops,"ROL", REG[dst], REG[dst], RHO[src])
        src=cyc[L-1]
        emit(ops,"XOR", REG[cyc[0]], "RSP", dsrc(ops,src))
        if RHO[src]: emit(ops,"ROL", REG[cyc[0]], REG[cyc[0]], RHO[src])

SCR5 = ["RAX","TMP12","TMP13","TMP14","TMP15"]

def chi_movefree(ops):
    """Baseline: all five NOTANDs into the dead D registers, then five XORs."""
    for y in range(5):
        r=[5*y+x for x in range(5)]
        for x in range(5):
            emit(ops,"NOTAND", SCR5[x], REG[r[(x+1)%5]], REG[r[(x+2)%5]])
        for x in range(5):
            emit(ops,"XOR", REG[r[x]], REG[r[x]], SCR5[x])

def chi_savemov(ops):
    """Ablation: the register-starved shape native code is forced into — save two
    lanes per row, then update in place, using three scratch registers instead of
    five. Isolates what the register headroom buys, holding NOTAND constant."""
    S0,S1,T = "TMP12","TMP13","RAX"
    for y in range(5):
        r=[5*y+x for x in range(5)]
        emit(ops,"MOV", S0, REG[r[0]])
        emit(ops,"MOV", S1, REG[r[1]])
        pairs=[(REG[r[1]],REG[r[2]]), (REG[r[2]],REG[r[3]]),
               (REG[r[3]],REG[r[4]]), (REG[r[4]],S0), (S0,S1)]
        for x,(b,c) in enumerate(pairs):
            emit(ops,"NOTAND", T, b, c)
            emit(ops,"XOR", REG[r[x]], REG[r[x]], T)

def chi_no_notand(ops):
    """Ablation: no NOTAND micro-op. Uses the identity (~b)&c == ((b^c)&c), which
    needs no all-ones constant and no extra register, so this costs exactly one
    additional operation per NOTAND and nothing else. Goldmont native code has no
    ANDN either (it implements neither BMI1 nor BMI2), so this variant measures
    what the microcode-only primitive is worth."""
    for y in range(5):
        r=[5*y+x for x in range(5)]
        for x in range(5):
            b,c = REG[r[(x+1)%5]], REG[r[(x+2)%5]]
            emit(ops,"XOR", SCR5[x], b, c)          # b^c
            emit(ops,"AND", SCR5[x], SCR5[x], c)    # (b^c)&c == (~b)&c
        for x in range(5):
            emit(ops,"XOR", REG[r[x]], REG[r[x]], SCR5[x])

def iota_byteindex(ops):
    """Baseline: the counter lane holds the RC byte index, so iota is LD/LDX/XOR
    with no shift or add on the critical path."""
    emit(ops,"LD",   "TMP14", OFF(COUNTER))
    emit(ops,"LDX",  "TMP13", "TMP14")
    emit(ops,"XOR",  REG[0], REG[0], "TMP13")

def iota_shift(ops):
    """Ablation: the counter lane holds a round number, so the RC address must be
    computed with a shift and an add before the load can start."""
    emit(ops,"LD",   "TMP14", OFF(COUNTER))
    emit(ops,"SHLI", "TMP13", "TMP14", 3)
    emit(ops,"ADDI", "TMP13", "TMP13", (RCTAB-BASE_LANE)*8)
    emit(ops,"LDX",  "TMP13", "TMP13")
    emit(ops,"XOR",  REG[0], REG[0], "TMP13")

# ─────────────────────────── variants ───────────────────────────

def make_body(theta_c=theta_c_serial, theta_dd=theta_d, d_in_reg=True,
              chi=chi_movefree, iota=iota_byteindex):
    def body(ops):
        theta_c(ops); theta_dd(ops)
        apply_rho_pi(ops, d_in_reg=d_in_reg)
        chi(ops); iota(ops)
    return body

# The baseline is the kernel as shipped. Rows below it each change ONE thing.
# balanced_theta is not an ablation but a rejected alternative: it was the shipped
# choice until the ablation showed it costs rather than saves.
VARIANTS = [
    ("baseline",       "none (the kernel as shipped)",
     dict()),
    ("balanced_theta", "serial theta-parity -> balanced tree (rejected alternative)",
     dict(theta_c=theta_c_balanced)),
    ("spill_d",       "resident D -> D reloaded from memory per lane",
     dict(theta_dd=theta_d_spilled, d_in_reg=False)),
    ("savemov_chi",   "move-free chi -> two save-moves per row",
     dict(chi=chi_savemov)),
    ("no_notand",     "NOTAND micro-op -> XOR+AND identity",
     dict(chi=chi_no_notand)),
    ("rc_shift",      "RC byte-index cursor -> round number + shift/add",
     dict(iota=iota_shift)),
]

def emit_variant(name, triads, pro_t, loop_top_addr):
    os.makedirs(OUT, exist_ok=True)
    up=name.upper()
    lines=[]
    for ti,t in enumerate(triads):
        seqw="END_SEQWORD" if ti==len(triads)-1 else "NOP_SEQWORD"
        if t[0][0]=="LOOPTEST":
            _,chk,cnt,thr=t[0]
            s0=f"XOR_DSZ64_DRI({chk}, {cnt}, {thr})"
            s2=f"UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI({chk}, 0x{loop_top_addr:04x})"
            lines.append(f"    {{ {s0}, NOP, {s2}, {seqw} }},")
        else:
            s=[G.op_to_c(o) for o in t]
            while len(s)<3: s.append("NOP")
            lines.append(f"    {{ {s[0]}, {s[1]}, {s[2]}, {seqw} }},")
    open(f"{OUT}/{name}_body.h","w").write("\n".join(lines)+"\n")

def build(name, desc, kw):
    # cursor encoding is the only global a variant may change
    if kw.get("iota") is iota_shift:
        G.COUNTER_INIT, G.COUNTER_STEP, G.LOOP_THRESHOLD = 0, 1, 24
    else:
        G.COUNTER_INIT = (RCTAB-BASE_LANE)*8
        G.COUNTER_STEP = 8
        G.LOOP_THRESHOLD = G.RC_END_IDX
    G.gen_body = make_body(**kw)

    import io as _io, contextlib as _cl
    nf = G.verify_perm(16)
    with _cl.redirect_stdout(_io.StringIO()):
        nk = G.verify_kat()          # published f(0) and f(f(0)) vectors

    pro=[];  G.gen_prologue(pro)
    body=[]; G.gen_body(body)
    lc=[];   G.gen_loopctrl(lc)
    epi=[];  G.gen_epilogue(epi)
    pro_t=G.pack(pro); body_t=G.pack(body+lc); epi_t=G.pack(epi)
    triads=pro_t+body_t+epi_t
    loop_top=0x7c00+len(pro_t)*4
    nmem=sum(1 for o in body+lc if G.is_mem(o))
    fits = len(triads)<=128
    if fits:
        emit_variant(name, triads, pro_t, loop_top)
    return dict(name=name, desc=desc, ok=(nf==0 and nk==0), triads=len(triads),
                body=len(body_t), ops=len(body)+len(lc), mem=nmem,
                fits=fits, counter_init=G.COUNTER_INIT, loop_top=loop_top,
                pro=len(pro_t))

if __name__=="__main__":
    rows=[build(n,d,k) for n,d,k in VARIANTS]
    base=rows[0]
    print()
    print(f"{'variant':14s} {'correct':8s} {'triads':>7s} {'body':>6s} {'ops':>5s} {'mem':>4s} {'fits':>5s}  what was removed")
    for r in rows:
        print(f"{r['name']:14s} {'PASS' if r['ok'] else 'FAIL':8s} {r['triads']:7d} "
              f"{r['body']:6d} {r['ops']:5d} {r['mem']:4d} {'yes' if r['fits'] else 'NO':>5s}  {r['desc']}")
    print()
    # header the harness includes: one entry per variant that fits
    os.makedirs(OUT, exist_ok=True)
    h=["/* AUTO-GENERATED by keccak_ablate.py — do not edit. */"]
    for r in rows:
        if not r['fits']: continue
        u=r['name'].upper()
        h += [f"#define ABL_{u}_TRIADS {r['triads']}",
              f"#define ABL_{u}_COUNTER_INIT {r['counter_init']}",
              f"#define ABL_{u}_LOOPTOP 0x{r['loop_top']:04x}"]
    h.append(f"#define ABL_PROLOGUE_TRIADS {base['pro']}")
    open(f"{OUT}/abl_meta.h","w").write("\n".join(h)+"\n")
    bad=[r['name'] for r in rows if not r['ok']]
    over=[r['name'] for r in rows if not r['fits']]
    if bad: print("INCORRECT VARIANTS:", bad); sys.exit(1)
    print("all variants reproduce the 24-round reference permutation")
    if over: print("capacity results (exceed 128 triads, not benchmarked):", over)
