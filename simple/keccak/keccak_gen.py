#!/usr/bin/env python3
"""
keccak_gen.py — Generate + software-verify Keccak-f[1600] in microcode.

One product: the full 24-round permutation, LOOPED inside a single vmwrite
(keccak_perm.h / keccak_perm_body.h). The 25 state lanes stay resident in
registers across all 24 rounds — 13 GPR + 12 TMP hold the lanes, and RSP is
borrowed as a 32nd data register (saved at the prologue, restored at the
epilogue) so theta's 5 D mixing lanes can stay in registers too.

The full permutation:
  - prologue: save RSP; load 25 state lanes into 13 GPR + 12 TMP    (once)
  - loop_top: theta + rho + pi + chi + iota (RC[counter] lookup)    (x24)
              counter++ ; {XOR sets ZF, UJMPCC CONDNZ -> loop_top}
  - epilogue: store 25 state lanes back; restore RSP                (once)
  Only the prologue and epilogue touch the 25 state lanes in memory. theta
  spills the column parities C to the buffer, but keeps D in registers.

Hardware facts (all verified on this Goldmont — see memory keccak-io-harness-seg
and microcode-control-flow):
  - LDZX/STAD: SEG_DS (0x18); offset immediate 8-bit SIGNED (-128..+127) -> base
    must be centered; index-register form [base+index] has no offset limit
    (used for the RC table + counter beyond immediate reach).
  - Intra-triad fully sequential incl 3-deep RAW; STLF works; <=1 mem op/triad.
    (probe_memops, 2026-06-29: a SINGLE isolated 2-mem triad — two LD, two ST,
    LD+ST, ST+LD-same-addr — PASSES, but packing MANY 2-mem triads back-to-back
    in the prologue/epilogue CRASHED the NUC. So the packer stays at 1 mem/triad
    until the resource limit is understood. See keccak-two-mem-ops-per-triad.)
  - ROL_DSZ64 works; NOTAND(d,a,b)=(~a)&b; immediate field 16-bit.
  - Loop: UJMPCC reads ARCH RFLAGS (ignores reg operand); ZF=0 at entry; an XOR
    in the SAME triad sets ZF; count UP (ADD), compare via XOR; backward UJMPCC
    CONDNZ works (ujmp_test P/Q). The XOR and UJMPCC MUST share a triad.

Buffer g_keccak_buf[56], base RCX = &buf[16] (centered):
    0..24  state   (offset (i-16)*8 = -128..+64, immediate)
    25..29 D scratch                              (+72..+104, immediate)
    30     loop counter                           (+112, immediate)
    31     saved RSP (RSP reused as 32nd data reg) (+120, immediate)
    32..55 RC[0..23] table          (index-register addressing only)
The C wrapper fills state[0..24], counter(30)=0, RC table(32..55) before firing.

Coordinates: state index i = 5*y + x.
"""

import sys

RC = [
    0x0000000000000001, 0x0000000000008082, 0x800000000000808a, 0x8000000080008000,
    0x000000000000808b, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
    0x000000000000008a, 0x0000000000000088, 0x0000000080008009, 0x000000008000000a,
    0x000000008000808b, 0x800000000000008b, 0x8000000000008089, 0x8000000000008003,
    0x8000000000008002, 0x8000000000000080, 0x000000000000800a, 0x800000008000000a,
    0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008,
]
RHO = [ 0,1,62,28,27, 36,44,6,55,20, 3,10,43,25,39, 41,45,15,21,8, 18,2,61,56,14 ]
MASK = (1 << 64) - 1
def rol(v, n):
    n &= 63
    return ((v << n) | (v >> (64 - n))) & MASK if n else (v & MASK)

def keccak_round_ref(A, rc):
    C = [A[x]^A[x+5]^A[x+10]^A[x+15]^A[x+20] for x in range(5)]
    D = [C[(x+4)%5]^rol(C[(x+1)%5],1) for x in range(5)]
    B = [0]*25
    for y in range(5):
        for x in range(5):
            B[y+5*((2*x+3*y)%5)] = rol(A[x+5*y]^D[x], RHO[x+5*y])
    out=[0]*25
    for y in range(5):
        for x in range(5):
            out[x+5*y]=B[x+5*y]^((~B[(x+1)%5+5*y])&B[(x+2)%5+5*y]&MASK)
    out[0]^=rc
    return out

def keccak_perm_ref(A):
    s=list(A)
    for r in range(24):
        s=keccak_round_ref(s, RC[r])
    return s

def pi_dest(i):
    x,y=i%5,i//5
    return y+5*((2*x+3*y)%5)
def pi_cycles():
    seen=[False]*25; cyc=[]
    for s in range(25):
        if seen[s]: continue
        c=[]; j=s
        while not seen[j]:
            seen[j]=True; c.append(j); j=pi_dest(j)
        cyc.append(c)
    return cyc

# registers
GPRS=["RDI","RSI","RBX","RDX","RBP","R8","R9","R10","R11","R12","R13","R14","R15"]
TMPS=[f"TMP{i}" for i in range(12)]
REG=GPRS+TMPS
assert len(REG)==25
BASE="RCX"
def col(i): return i%5    # lane index -> column x  (i = x + 5y, so i%5 = x)

# Fully-resident D: theta writes the 5 column parities C to the buffer, but keeps
# the 5 D mixing lanes in registers (D_REG), so the apply reads D from registers
# with NO per-lane loads. This needs a 32nd register — RSP, borrowed as a data
# reg (probe_rsp confirmed it works when saved/restored). The pi cycle-walk's
# save-temp is also RSP, and the base RCX is never rebuilt. RSP is saved at the
# prologue and restored at the epilogue (once per permutation).
D_REG = ["RAX","TMP12","TMP13","TMP14","TMP15"]   # D[0..4], resident across the apply
RSPSAVE = 31                                      # buffer lane holding the saved RSP

# layout
BASE_LANE=16
DSCR=25; COUNTER=30; RCTAB=32
BUFLEN=56
def OFF(lane): return (lane-BASE_LANE)*8
for _l in range(0,32):
    assert -128 <= OFF(_l) <= 127, (_l, OFF(_l))

# ---- op IR ----
#  LD d,off        d = mem[base+off]            (immediate, signed-8)
#  ST s,off        mem[base+off] = s
#  LDX d,idxreg    d = mem[base+idxreg]         (index register, no limit)
#  STX s,idxreg    mem[base+idxreg] = s
#  XOR/NOTAND/ROL/MOV/XORI  as before
#  ADDI d,s,imm    d = s + imm
#  SHLI d,s,imm    d = s << imm
#  UJMPCC chk,target   conditional backward branch (sim: no-op)

def emit(ops,*o): ops.append(o)

def gen_prologue(ops):
    emit(ops,"ST", "RSP", OFF(RSPSAVE))   # save stack pointer (RSP reused as 32nd data reg)
    for i in range(25):
        emit(ops,"LD", REG[i], OFF(i))

def gen_body(ops):
    """theta + rho + pi + chi + iota(RC[counter]). State stays in REG[]."""
    # theta-C -> buffer (Cscr = DSCR lanes 25..29). SERIAL reduction, depth 4.
    # We previously used a balanced XOR tree (depth 3, RSP as a second
    # accumulator) on the reasoning that C heads the round's dependency chain.
    # The ablation (keccak_ablate.py / bench_ablation.c) MEASURED that choice and
    # it was a pessimisation: the balanced form cost 1.2% (1915 vs 1893 cyc/perm,
    # non-overlapping p10-p90) at identical triad and operation counts. The
    # shorter chain was not the binding constraint, and occupying a second
    # accumulator across each column constrained the packer. Serial it is.
    for x in range(5):
        emit(ops,"XOR", "RAX", REG[x], REG[x+5])
        emit(ops,"XOR", "RAX", "RAX", REG[x+10])
        emit(ops,"XOR", "RAX", "RAX", REG[x+15])
        emit(ops,"XOR", "RAX", "RAX", REG[x+20])
        emit(ops,"ST",  "RAX", OFF(DSCR+x))
    # theta-D -> 5 REGISTERS (D_REG), reading C from buffer. RSP = rol temp.
    # D[x] = C[x-1] ^ rol(C[x+1],1).  No in-place hazard (C is in memory).
    for x in range(5):
        emit(ops,"LD",  "RSP", OFF(DSCR+(x+1)%5))   # C[x+1]
        emit(ops,"ROL", "RSP", "RSP", 1)
        emit(ops,"LD",  D_REG[x], OFF(DSCR+(x+4)%5)) # C[x-1]
        emit(ops,"XOR", D_REG[x], D_REG[x], "RSP")   # D[x]
    # theta-apply + rho + pi, IN PLACE, D read from REGISTERS (no per-lane LD!).
    # save-temp = RSP (free after theta-D). RCX stays = base (no rebuild).
    for cyc in pi_cycles():
        if len(cyc)==1:
            i=cyc[0]
            emit(ops,"XOR", REG[i], REG[i], D_REG[col(i)])
            if RHO[i]: emit(ops,"ROL", REG[i], REG[i], RHO[i])
            continue
        L=len(cyc)
        emit(ops,"MOV", "RSP", REG[cyc[L-1]])
        for k in range(L-1,0,-1):
            dst,src=cyc[k],cyc[k-1]
            emit(ops,"XOR", REG[dst], REG[src], D_REG[col(src)])
            if RHO[src]: emit(ops,"ROL", REG[dst], REG[dst], RHO[src])
        src=cyc[L-1]
        emit(ops,"XOR", REG[cyc[0]], "RSP", D_REG[col(src)])
        if RHO[src]: emit(ops,"ROL", REG[cyc[0]], REG[cyc[0]], RHO[src])
    # chi per row, in place. The 5 D registers are dead now, so use them as
    # scratch: compute ALL 5 NOTANDs of the row FIRST (the B values are still
    # intact), then do 5 in-place XORs. This avoids the 2 save-MOVs/row (10
    # MOVs/round) that register-starved native code is forced into — we have the
    # headroom because the whole state is register-resident.
    SCR5 = ["RAX","TMP12","TMP13","TMP14","TMP15"]
    for y in range(5):
        r=[5*y+x for x in range(5)]
        for x in range(5):
            emit(ops,"NOTAND", SCR5[x], REG[r[(x+1)%5]], REG[r[(x+2)%5]])
        for x in range(5):
            emit(ops,"XOR", REG[r[x]], REG[r[x]], SCR5[x])
    # iota: RC[counter] via index addressing. The counter lane holds the RC
    # BYTE-INDEX directly (init = (RCTAB-BASE_LANE)*8, += 8 per round), so iota is
    # just LD idx; LDX RC; XOR — no SHL/ADD on the iota->RC->lane0 critical path.
    emit(ops,"LD",   "TMP14", OFF(COUNTER))     # idx (bytes)
    emit(ops,"LDX",  "TMP13", "TMP14")          # RC = mem[base+idx]
    emit(ops,"XOR",  REG[0], REG[0], "TMP13")

RC_END_IDX = (RCTAB-BASE_LANE)*8 + 24*8         # byte-index just past RC[23]
# Cursor encoding. Baseline: the counter lane holds the RC BYTE-INDEX, stepped by
# 8, so iota needs no shift/add. An ablation may instead hold a round number,
# stepped by 1 and compared against 24; these three globals capture that choice.
COUNTER_INIT    = (RCTAB-BASE_LANE)*8
COUNTER_STEP    = 8
LOOP_THRESHOLD  = RC_END_IDX

def gen_loopctrl(ops):
    """cursor += step ; store ; {XOR sets ZF, UJMPCC CONDNZ -> loop_top}. TMP14=cursor."""
    emit(ops,"ADDI", "TMP14", "TMP14", COUNTER_STEP)
    emit(ops,"ST",   "TMP14", OFF(COUNTER))
    # the XOR + UJMPCC must be the SAME triad -> emitted as a forced triad later.
    emit(ops,"LOOPTEST", "TMP12", "TMP14", LOOP_THRESHOLD)

def gen_epilogue(ops):
    for i in range(25):
        emit(ops,"ST", REG[i], OFF(i))
    emit(ops,"LD", "RSP", OFF(RSPSAVE))   # restore stack pointer before returning

# ---- simulator (models full looped execution + signed/index addressing) ----
def simulate_perm(init_state):
    rf={"RSP":0}; buf=[0]*BUFLEN
    buf[0:25]=list(init_state)
    buf[COUNTER]=COUNTER_INIT               # cursor init (see COUNTER_INIT)
    for r in range(24): buf[RCTAB+r]=RC[r]
    def at(off):
        assert -128<=off<=127, off
        return BASE_LANE + off//8
    def run(ops):
        for op in ops:
            k=op[0]
            if   k=="LD":   rf[op[1]]=buf[at(op[2])]
            elif k=="ST":   buf[at(op[2])]=rf[op[1]]&MASK
            elif k=="LDX":  rf[op[1]]=buf[BASE_LANE + rf[op[2]]//8]
            elif k=="STX":  buf[BASE_LANE + rf[op[2]]//8]=rf[op[1]]&MASK
            elif k=="XOR":  rf[op[1]]=(rf[op[2]]^rf[op[3]])&MASK
            elif k=="XORI": rf[op[1]]=(rf[op[2]]^op[3])&MASK
            elif k=="ROL":  rf[op[1]]=rol(rf[op[2]],op[3])
            elif k=="NOTAND": rf[op[1]]=((~rf[op[2]])&rf[op[3]])&MASK
            elif k=="AND":  rf[op[1]]=(rf[op[2]]&rf[op[3]])&MASK
            elif k=="MOV":  rf[op[1]]=rf[op[2]]&MASK
            elif k=="ADDI": rf[op[1]]=(rf[op[2]]+op[3])&MASK
            elif k=="SHLI": rf[op[1]]=(rf[op[2]]<<op[3])&MASK
            elif k=="LOOPTEST": pass     # XOR sets flags + branch; Python drives the loop
            else: raise ValueError(k)
    pro=[]; gen_prologue(pro); run(pro)
    body=[]; gen_body(body)
    lc=[];   gen_loopctrl(lc)
    for _ in range(24):
        run(body); run(lc)
    epi=[]; gen_epilogue(epi); run(epi)
    return buf[0:25]

# ---- packer (ordered; <=3 ops/triad; <=1 mem (2-mem packing crashed the box,
#       see pack()); no load-to-use in a triad; LOOPTEST forced into its own triad) ----
def is_mem(op): return op[0] in ("LD","ST","LDX","STX")
def reads_of(op):
    if op[0] in ("XOR","NOTAND","AND"): return (op[2],op[3])
    if op[0] in ("XORI","ROL","MOV","ADDI","SHLI"): return (op[2],)
    if op[0] in ("ST","STX"): return (op[1],) + ((op[2],) if op[0]=="STX" else ())
    if op[0]=="LDX": return (op[2],)
    return ()
def pack(oplist):
    triads,cur,cur_mem,ld=[],[],0,set()
    def flush():
        nonlocal cur,cur_mem,ld
        if cur: triads.append(cur);
        cur,cur_mem,ld=[],0,set()
    for op in oplist:
        if op[0]=="LOOPTEST":
            flush(); triads.append([op]); continue
        mem=is_mem(op); hz=any(r in ld for r in reads_of(op))
        # NOTE: cur_mem==1 (one mem op/triad). probe_memops showed a SINGLE 2-mem
        # triad works in isolation, but packing many of them (12-13 back-to-back
        # 2-load / 2-store triads in the prologue/epilogue) CRASHED the NUC hard
        # (2026-06-29). Reverted to 1 mem/triad. See keccak-two-mem-ops-per-triad.
        if len(cur)==3 or (mem and cur_mem==1) or hz: flush()
        cur.append(op)
        if mem: cur_mem+=1
        if op[0] in ("LD","LDX"): ld.add(op[1])
    flush()
    return triads

# ---- C emission ----
def op_to_c(op, loop_top=None):
    k=op[0]
    if   k=="LD":   return f"LDZX_DSZ64_ASZ32_SC1_DRI({op[1]}, {BASE}, {op[2]}, SEG_DS)"
    elif k=="ST":   return f"STAD_DSZ64_ASZ32_SC1_RRI({op[1]}, {BASE}, {op[2]}, SEG_DS)"
    elif k=="LDX":  return f"LDZX_DSZ64_ASZ32_SC1_DRR({op[1]}, {BASE}, {op[2]}, SEG_DS)"
    elif k=="STX":  return f"STAD_DSZ64_ASZ32_SC1_RRR({op[1]}, {BASE}, {op[2]}, SEG_DS)"
    elif k=="XOR":  return f"XOR_DSZ64_DRR({op[1]}, {op[2]}, {op[3]})"
    elif k=="XORI": return f"XOR_DSZ64_DRI({op[1]}, {op[2]}, {hex(op[3])})"
    elif k=="ROL":  return f"ROL_DSZ64_DRI({op[1]}, {op[2]}, {op[3]})"
    elif k=="NOTAND": return f"NOTAND_DSZ64_DRR({op[1]}, {op[2]}, {op[3]})"
    elif k=="AND":  return f"AND_DSZ64_DRR({op[1]}, {op[2]}, {op[3]})"
    elif k=="MOV":  return f"ZEROEXT_DSZ64_DR({op[1]}, {op[2]})"
    elif k=="LDI_IDX": return f"ZEROEXT_DSZ32_DI({op[1]}, {op[2]})"
    elif k=="ADDI": return f"ADD_DSZ64_DRI({op[1]}, {op[2]}, {op[3]})"
    elif k=="SHLI": return f"SHL_DSZ64_DRI({op[1]}, {op[2]}, {op[3]})"
    raise ValueError(k)

def emit_perm_c(path):
    # build phases
    pro=[]; gen_prologue(pro)
    body=[]; gen_body(body)
    lc=[];   gen_loopctrl(lc)
    epi=[]; gen_epilogue(epi)
    pro_t=pack(pro)
    loop_top_addr=0x7c00 + len(pro_t)*4
    body_t=pack(body+lc)
    epi_t=pack(epi)
    triads=pro_t+body_t+epi_t

    lines=[]
    def tri_c(t, seqw):
        # LOOPTEST expands to a XOR(slot0) + NOP + UJMPCC(slot2) triad
        if t[0][0]=="LOOPTEST":
            _,chk,cnt,thr=t[0]
            s0=f"XOR_DSZ64_DRI({chk}, {cnt}, {thr})"
            s2=f"UJMPCC_DIRECT_NOTTAKEN_CONDNZ_RI({chk}, 0x{loop_top_addr:04x})"
            return f"    {{ {s0}, NOP, {s2}, {seqw} }},"
        s=[op_to_c(o) for o in t]
        while len(s)<3: s.append("NOP")
        return f"    {{ {s[0]}, {s[1]}, {s[2]}, {seqw} }},"

    head=[f"/* AUTO-GENERATED by keccak_gen.py — do not edit. */",
          f"/* Keccak-f[1600] full 24-round permutation, looped. {len(triads)} triads. */",
          f"/* loop_top = U{loop_top_addr:04x} (after {len(pro_t)}-triad prologue). */",
          f"#define KECCAK_PERM_TRIADS {len(triads)}",
          f"#define KECCAK_BUFLEN {BUFLEN}",
          f"#define KECCAK_BASE_LANE {BASE_LANE}",
          f"#define KECCAK_COUNTER_LANE {COUNTER}",
          f"#define KECCAK_RCTAB_LANE {RCTAB}"]
    body_lines=[]
    for ti,t in enumerate(triads):
        seqw="END_SEQWORD" if ti==len(triads)-1 else "NOP_SEQWORD"
        body_lines.append(tri_c(t, seqw))
    open(path,"w").write("\n".join(head)+"\n")
    open(path.replace(".h","_body.h"),"w").write("\n".join(body_lines)+"\n")
    return len(triads), len(pro_t), len(body_t), len(epi_t)

# ---- verify + emit ----
def verify_perm(n=64):
    import random; random.seed(99); fails=0
    for t in range(n):
        st=[random.getrandbits(64) for _ in range(25)]
        ref=keccak_perm_ref(st)
        got=simulate_perm(st)
        if got!=ref:
            fails+=1; print(f"PERM trial {t} MISMATCH")
            for i in range(25):
                if got[i]!=ref[i]: print(f"  lane[{i}] got={got[i]:016x} ref={ref[i]:016x}")
            if fails>1: break
    return fails

# Independent published KAT (Keccak team's KeccakF-1600-IntermediateValues.txt).
# Full 25-lane vectors, NOT recomputed from keccak_perm_ref -> the SIMULATOR (which
# is what lowers to the actual patch) is asserted against fixed published constants.
#   KAT_F0   = f(0)     (zero input; lane0 is the canonical 0xf1258f7940e1dde7)
#   KAT_F0F0 = f(f(0))  (its INPUT, KAT_F0, is fully non-zero -> a non-trivial KAT)
KAT_F0 = [
    0xF1258F7940E1DDE7, 0x84D5CCF933C0478A, 0xD598261EA65AA9EE, 0xBD1547306F80494D, 0x8B284E056253D057,
    0xFF97A42D7F8E6FD4, 0x90FEE5A0A44647C4, 0x8C5BDA0CD6192E76, 0xAD30A6F71B19059C, 0x30935AB7D08FFC64,
    0xEB5AA93F2317D635, 0xA9A6E6260D712103, 0x81A57C16DBCF555F, 0x43B831CD0347C826, 0x01F22F1A11A5569F,
    0x05E5635A21D9AE61, 0x64BEFEF28CC970F2, 0x613670957BC46611, 0xB87C5A554FD00ECB, 0x8C3EE88A1CCF32C8,
    0x940C7922AE3A2614, 0x1841F924A2C509E4, 0x16F53526E70465C2, 0x75F644E97F30A13B, 0xEAF1FF7B5CECA249,
]
KAT_F0F0 = [
    0x2D5C954DF96ECB3C, 0x6A332CD07057B56D, 0x093D8D1270D76B6C, 0x8A20D9B25569D094, 0x4F9C4F99E5E7F156,
    0xF957B9A2DA65FB38, 0x85773DAE1275AF0D, 0xFAF4F247C3D810F7, 0x1F1B9EE6F79A8759, 0xE4FECC0FEE98B425,
    0x68CE61B6B9CE68A1, 0xDEEA66C4BA8F974F, 0x33C43D836EAFB1F5, 0xE00654042719DBD9, 0x7CF8A9F009831265,
    0xFD5449A6BF174743, 0x97DDAD33D8994B40, 0x48EAD5FC5D0BE774, 0xE3B8C8EE55B7B03C, 0x91A0226E649E42E9,
    0x900E3129E7BADD7B, 0x202A9EC5FAA3CCE8, 0x5B3402464E1C3DB6, 0x609F4E62A44C1059, 0x20D06CD26A8FBF5C,
]

def verify_kat():
    """Assert the simulator reproduces the published f(0) and f(f(0)) vectors.
    The second feeds the NON-ZERO state f(0) as input. Returns # of bad lanes."""
    fails=0
    for label,inp,exp in (("f(0)      [zero in]   ", [0]*25, KAT_F0),
                          ("f(f(0))   [nonzero in]", KAT_F0,  KAT_F0F0)):
        got=simulate_perm(inp)
        bad=[i for i in range(25) if got[i]!=exp[i]]
        for i in bad[:4]:
            print(f"  KAT {label} lane[{i}] got={got[i]:016x} exp={exp[i]:016x}")
        print(f"KAT {label}: {'FAIL' if bad else 'OK'} ({25-len(bad)}/25)")
        fails+=len(bad)
    return fails

if __name__=="__main__":
    nf=verify_perm(64)
    if nf: print("PERM SIM FAILED"); sys.exit(1)
    print("PERM SIM OK: 64/64 random permutations match 24-round reference.")
    nk=verify_kat()
    if nk: print("KAT SIM FAILED"); sys.exit(1)
    print("KAT SIM OK: simulator matches published f(0) + f(f(0)) vectors.")
    nt,npro,nbody,nepi=emit_perm_c("keccak_perm.h")
    print(f"perm triads={nt}  (prologue {npro} + body+loop {nbody} + epilogue {nepi})  cap 128")
    print("Wrote keccak_perm.h + keccak_perm_body.h")
