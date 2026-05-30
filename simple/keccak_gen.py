#!/usr/bin/env python3
"""
keccak_gen.py — Generate + software-verify one Keccak-f[1600] round as a packed
microcode triad array, emitted as C (keccak_round.h + keccak_round_body.h).

Hardware constraints (all confirmed on this Goldmont, 2026-05-30):
  - LDZX/STAD offset field is 8-bit SIGNED: usable -128..+127 bytes (probe_offset).
    => base register reaches only +-16 lanes; we CENTER the base.
  - Intra-triad execution is fully sequential incl. 3-deep RAW chains (probe_triad).
    => pack the ordered op-list 3/triad; only constraint is <=1 mem op/triad and
       (conservatively) no load-to-use within a single triad.
  - Store-to-load forwarding within a patch works (probe_stlf).
  - ROL_DSZ64_DRI works (probe_rol); NOTAND_DSZ64_DRR(d,a,b) = (~a)&b.
  - Immediate field is 16-bit.
  - SEG_DS (0x18); -no-pie so buffer is in low 4GB for ASZ32.

Layout (g_keccak_buf[30], base RCX = &g_keccak_buf[14] set by the C wrapper):
    lanes 0..24  = state   (offsets (i-14)*8 = -112..+80)
    lanes 25..29 = D[0..4] scratch (offsets +88..+120)
  All offsets within +-127. RCX stays = base the WHOLE round (never clobbered).

Round:
  - 25 lanes resident in REG[] (13 GPR + 12 TMP).
  - theta C[0..4] computed into 5 scratch regs (RAX,TMP12..TMP15).
  - theta D[x]=C[x-1]^ROL(C[x+1],1) computed (borrowing REG[24], buffer-backed,
    reloaded after) and STORED to buffer lanes 25..29.
  - theta-apply + rho + pi IN PLACE following pi cycles backward (save temp RAX),
    D[col] loaded per-lane from the buffer into TMP12.
  - chi per row in place (RAX,TMP12 = saves, TMP13 = NOTAND temp); iota on (0,0).
  - epilogue stores 25 lanes back. RCX intact throughout.

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
C_REG=["RAX","TMP12","TMP13","TMP14","TMP15"]   # theta C[0..4]
T_DTMP="TMP12"   # D-load temp in apply / chi notand
T_SAVE="RAX"     # apply cycle save / chi save0
T_S1="TMP12"     # chi save1
T_N="TMP13"      # chi notand temp
BORROW="TMP11"   # = REG[24]; borrowed as theta-D rol temp, reloaded after
assert BORROW==REG[24]

# layout
BASE_LANE=14
BUFLEN=30
def ST_OFF(i): return (i-BASE_LANE)*8       # state lane i
def D_OFF(x):  return (25+x-BASE_LANE)*8    # D[x] at lane 25+x
for _i in range(25):
    assert -128 <= ST_OFF(_i) <= 127, _i
for _x in range(5):
    assert -128 <= D_OFF(_x) <= 127, _x

ops=[]
def emit(*o): ops.append(o)
def col(i): return i%5

def gen_round(rc):
    ops.clear()
    # prologue
    for i in range(25):
        emit("LD", REG[i], ST_OFF(i))
    # theta C -> C_REG
    for x in range(5):
        emit("XOR", C_REG[x], REG[x], REG[x+5])
        emit("XOR", C_REG[x], C_REG[x], REG[x+10])
        emit("XOR", C_REG[x], C_REG[x], REG[x+15])
        emit("XOR", C_REG[x], C_REG[x], REG[x+20])
    # theta D[x] = C[x-1]^ROL(C[x+1],1) -> buffer lanes 25..29 (borrow REG[24])
    for x in range(5):
        emit("ROL", BORROW, C_REG[(x+1)%5], 1)
        emit("XOR", BORROW, C_REG[(x+4)%5], BORROW)
        emit("ST",  BORROW, D_OFF(x))
    emit("LD", BORROW, ST_OFF(24))          # restore borrowed lane 24
    # theta-apply + rho + pi, in place, backward cycle (save=RAX, Dtmp=TMP12)
    for cyc in pi_cycles():
        if len(cyc)==1:
            i=cyc[0]
            emit("LD", T_DTMP, D_OFF(col(i)))
            emit("XOR", REG[i], REG[i], T_DTMP)
            if RHO[i]: emit("ROL", REG[i], REG[i], RHO[i])
            continue
        L=len(cyc)
        emit("MOV", T_SAVE, REG[cyc[L-1]])
        for k in range(L-1,0,-1):
            dst,src=cyc[k],cyc[k-1]
            emit("LD", T_DTMP, D_OFF(col(src)))
            emit("XOR", REG[dst], REG[src], T_DTMP)
            if RHO[src]: emit("ROL", REG[dst], REG[dst], RHO[src])
        src=cyc[L-1]
        emit("LD", T_DTMP, D_OFF(col(src)))
        emit("XOR", REG[cyc[0]], T_SAVE, T_DTMP)
        if RHO[src]: emit("ROL", REG[cyc[0]], REG[cyc[0]], RHO[src])
    # chi per row in place; iota on (0,0)
    for y in range(5):
        r=[5*y+x for x in range(5)]
        emit("MOV", T_SAVE, REG[r[0]])
        emit("MOV", T_S1,   REG[r[1]])
        emit("NOTAND", T_N, REG[r[1]], REG[r[2]]); emit("XOR", REG[r[0]], REG[r[0]], T_N)
        emit("NOTAND", T_N, REG[r[2]], REG[r[3]]); emit("XOR", REG[r[1]], REG[r[1]], T_N)
        emit("NOTAND", T_N, REG[r[3]], REG[r[4]]); emit("XOR", REG[r[2]], REG[r[2]], T_N)
        emit("NOTAND", T_N, REG[r[4]], T_SAVE);   emit("XOR", REG[r[3]], REG[r[3]], T_N)
        emit("NOTAND", T_N, T_SAVE, T_S1);        emit("XOR", REG[r[4]], REG[r[4]], T_N)
        if y==0:
            emit("XORI", REG[0], REG[0], rc)
    # epilogue
    for i in range(25):
        emit("ST", REG[i], ST_OFF(i))
    return list(ops)

# ---- simulator (models signed 8-bit offset, 30-lane buffer) ----
def simulate(oplist, init_state):
    rf={}
    buf=list(init_state)+[0]*(BUFLEN-25)
    def idx(off):
        assert -128<=off<=127, f"offset {off} out of signed-8 range"
        return BASE_LANE + off//8
    for op in oplist:
        k=op[0]
        if   k=="LD":   rf[op[1]]=buf[idx(op[2])]
        elif k=="ST":   buf[idx(op[2])]=rf[op[1]]&MASK
        elif k=="XOR":  rf[op[1]]=(rf[op[2]]^rf[op[3]])&MASK
        elif k=="XORI": rf[op[1]]=(rf[op[2]]^op[3])&MASK
        elif k=="ROL":  rf[op[1]]=rol(rf[op[2]],op[3])
        elif k=="NOTAND": rf[op[1]]=((~rf[op[2]])&rf[op[3]])&MASK
        elif k=="MOV":  rf[op[1]]=rf[op[2]]&MASK
        else: raise ValueError(k)
    return buf[:25]

# ---- packer ----
def is_mem(op): return op[0] in ("LD","ST")
def reads_of(op):
    if op[0] in ("XOR","NOTAND"): return (op[2],op[3])
    if op[0] in ("XORI","ROL","MOV"): return (op[2],)
    if op[0]=="ST": return (op[1],)
    return ()
def pack(oplist):
    triads,cur,cur_mem,ld=[],[],0,set()
    def flush():
        nonlocal cur,cur_mem,ld
        if cur: triads.append(cur)
        cur,cur_mem,ld=[],0,set()
    for op in oplist:
        mem=is_mem(op)
        hz=any(r in ld for r in reads_of(op))
        if len(cur)==3 or (mem and cur_mem==1) or hz: flush()
        cur.append(op)
        if mem: cur_mem+=1
        if op[0]=="LD": ld.add(op[1])
    flush()
    return triads

# ---- C emission ----
def op_to_c(op):
    k=op[0]
    if   k=="LD":   return f"LDZX_DSZ64_ASZ32_SC1_DRI({op[1]}, {BASE}, {op[2]}, SEG_DS)"
    elif k=="ST":   return f"STAD_DSZ64_ASZ32_SC1_RRI({op[1]}, {BASE}, {op[2]}, SEG_DS)"
    elif k=="XOR":  return f"XOR_DSZ64_DRR({op[1]}, {op[2]}, {op[3]})"
    elif k=="XORI": return f"XOR_DSZ64_DRI({op[1]}, {op[2]}, {hex(op[3])})"
    elif k=="ROL":  return f"ROL_DSZ64_DRI({op[1]}, {op[2]}, {op[3]})"
    elif k=="NOTAND": return f"NOTAND_DSZ64_DRR({op[1]}, {op[2]}, {op[3]})"
    elif k=="MOV":  return f"ZEROEXT_DSZ64_DR({op[1]}, {op[2]})"
    raise ValueError(k)
def emit_c(triads, path):
    head=[f"/* AUTO-GENERATED by keccak_gen.py — do not edit. */",
          f"/* Single Keccak round (round 0, RC=1). {len(triads)} triads. */",
          f"/* Buffer g_keccak_buf[{BUFLEN}]; base RCX = &g_keccak_buf[{BASE_LANE}]. */",
          f"#define KECCAK_ROUND_TRIADS {len(triads)}",
          f"#define KECCAK_BUFLEN {BUFLEN}",
          f"#define KECCAK_BASE_LANE {BASE_LANE}"]
    body=[]
    for ti,t in enumerate(triads):
        s=[op_to_c(o) for o in t]
        while len(s)<3: s.append("NOP")
        seqw="END_SEQWORD" if ti==len(triads)-1 else "NOP_SEQWORD"
        body.append(f"    {{ {s[0]}, {s[1]}, {s[2]}, {seqw} }},")
    open(path,"w").write("\n".join(head)+"\n")
    open(path.replace(".h","_body.h"),"w").write("\n".join(body)+"\n")

def verify(n=24):
    import random; random.seed(1234); fails=0
    for t in range(n):
        st=[random.getrandbits(64) for _ in range(25)]
        ref=keccak_round_ref(st,RC[t])
        got=simulate(gen_round(RC[t]),st)
        if got!=ref:
            fails+=1; print(f"ROUND {t} MISMATCH")
            for i in range(25):
                if got[i]!=ref[i]: print(f"  lane[{i}] got={got[i]:016x} ref={ref[i]:016x}")
            if fails>1: break
    return fails

if __name__=="__main__":
    if verify(24): print("SIM FAILED"); sys.exit(1)
    print("SIM OK: 24/24 rounds match reference (real regs, signed-offset model).")
    op=gen_round(RC[0]); tr=pack(op)
    print(f"ops={len(op)} mem={sum(1 for o in op if is_mem(o))} triads={len(tr)} (cap 128)")
    emit_c(tr,"keccak_round.h")
    print("Wrote keccak_round.h + keccak_round_body.h")
