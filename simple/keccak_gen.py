#!/usr/bin/env python3
"""
keccak_gen.py — Generate + software-verify Keccak-f[1600] in microcode.

Two products:
  * single round  (keccak_round.h / _body.h)  — Phase 3b, kept for reference
  * full 24-round permutation, LOOPED inside one vmwrite (keccak_perm.h / _body.h)

The full permutation:
  - prologue: load 25 state lanes into 13 GPR + 12 TMP            (once)
  - loop_top: theta + rho + pi + chi + iota (RC[counter] lookup)  (x24)
              counter++ ; {XOR sets ZF, UJMPCC CONDNZ -> loop_top}
  - epilogue: store 25 state lanes back                           (once)
  State stays resident in registers across all 24 rounds; only the prologue and
  epilogue touch the 25 state lanes in memory. theta still spills C->regs / D->buf.

Hardware facts (all verified on this Goldmont — see memory keccak-io-harness-seg
and microcode-control-flow):
  - LDZX/STAD: SEG_DS (0x18); offset immediate 8-bit SIGNED (-128..+127) -> base
    must be centered; index-register form [base+index] has no offset limit
    (used for the RC table + counter beyond immediate reach).
  - Intra-triad fully sequential incl 3-deep RAW; STLF works; <=1 mem op/triad.
  - ROL_DSZ64 works; NOTAND(d,a,b)=(~a)&b; immediate field 16-bit.
  - Loop: UJMPCC reads ARCH RFLAGS (ignores reg operand); ZF=0 at entry; an XOR
    in the SAME triad sets ZF; count UP (ADD), compare via XOR; backward UJMPCC
    CONDNZ works (ujmp_test P/Q). The XOR and UJMPCC MUST share a triad.

Buffer g_keccak_buf[56], base RCX = &buf[16] (centered):
    0..24  state   (offset (i-16)*8 = -128..+64, immediate)
    25..29 D scratch                              (+72..+104, immediate)
    30     loop counter                           (+112, immediate)
    31     theta-D borrow-save scratch            (+120, immediate)
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
C_REG=["RAX","TMP12","TMP13","TMP14","TMP15"]   # theta C[0..4]
BORROW="TMP11"                                   # =REG[24], theta-D rol temp (save/restore)
assert BORROW==REG[24]
T_SAVE="RAX"; T_DTMP="TMP12"; T_N="TMP13"        # apply/chi temps
def col(i): return i%5

# layout
BASE_LANE=16
STATE0=0; DSCR=25; COUNTER=30; BORROWSAVE=31; RCTAB=32
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
    for i in range(25):
        emit(ops,"LD", REG[i], OFF(i))

def gen_body(ops):
    """theta + rho + pi + chi + iota(RC[counter]). State stays in REG[]."""
    # theta parity C -> C_REG
    for x in range(5):
        emit(ops,"XOR", C_REG[x], REG[x], REG[x+5])
        emit(ops,"XOR", C_REG[x], C_REG[x], REG[x+10])
        emit(ops,"XOR", C_REG[x], C_REG[x], REG[x+15])
        emit(ops,"XOR", C_REG[x], C_REG[x], REG[x+20])
    # theta D -> buffer 25..29 ; borrow REG[24], save/restore via dedicated scratch lane
    emit(ops,"ST", BORROW, OFF(BORROWSAVE))            # save lane 24 (this round's value)
    for x in range(5):
        emit(ops,"ROL", BORROW, C_REG[(x+1)%5], 1)
        emit(ops,"XOR", BORROW, C_REG[(x+4)%5], BORROW)
        emit(ops,"ST",  BORROW, OFF(DSCR+x))
    emit(ops,"LD", BORROW, OFF(BORROWSAVE))            # restore lane 24
    # theta-apply + rho + pi, in place, backward pi-cycles. D loaded per-lane from
    # buffer. NOTE: preloading D into registers does NOT help (measured: per-lane
    # 2056, preload+base-rebuild 2166, preload+save-spill 2074). The D-loads
    # overlap within the iteration; the round is bound by the ALU dependency chain,
    # not D memory. Per-lane is the best (RCX stays base, max load parallelism).
    for cyc in pi_cycles():
        if len(cyc)==1:
            i=cyc[0]
            emit(ops,"LD", T_DTMP, OFF(DSCR+col(i)))
            emit(ops,"XOR", REG[i], REG[i], T_DTMP)
            if RHO[i]: emit(ops,"ROL", REG[i], REG[i], RHO[i])
            continue
        L=len(cyc)
        emit(ops,"MOV", T_SAVE, REG[cyc[L-1]])
        for k in range(L-1,0,-1):
            dst,src=cyc[k],cyc[k-1]
            emit(ops,"LD", T_DTMP, OFF(DSCR+col(src)))
            emit(ops,"XOR", REG[dst], REG[src], T_DTMP)
            if RHO[src]: emit(ops,"ROL", REG[dst], REG[dst], RHO[src])
        src=cyc[L-1]
        emit(ops,"LD", T_DTMP, OFF(DSCR+col(src)))
        emit(ops,"XOR", REG[cyc[0]], T_SAVE, T_DTMP)
        if RHO[src]: emit(ops,"ROL", REG[cyc[0]], REG[cyc[0]], RHO[src])
    # chi per row in place
    for y in range(5):
        r=[5*y+x for x in range(5)]
        emit(ops,"MOV", T_SAVE, REG[r[0]])
        emit(ops,"MOV", T_DTMP, REG[r[1]])
        emit(ops,"NOTAND", T_N, REG[r[1]], REG[r[2]]); emit(ops,"XOR", REG[r[0]], REG[r[0]], T_N)
        emit(ops,"NOTAND", T_N, REG[r[2]], REG[r[3]]); emit(ops,"XOR", REG[r[1]], REG[r[1]], T_N)
        emit(ops,"NOTAND", T_N, REG[r[3]], REG[r[4]]); emit(ops,"XOR", REG[r[2]], REG[r[2]], T_N)
        emit(ops,"NOTAND", T_N, REG[r[4]], T_SAVE);    emit(ops,"XOR", REG[r[3]], REG[r[3]], T_N)
        emit(ops,"NOTAND", T_N, T_SAVE, T_DTMP);       emit(ops,"XOR", REG[r[4]], REG[r[4]], T_N)
    # iota: RC[counter] via index addressing. counter in buf[COUNTER].
    #   TMP15=counter; TMP14=counter<<3 + (RCTAB-BASE_LANE)*8; RC=mem[base+TMP14]; lane0^=RC
    emit(ops,"LD",   "TMP15", OFF(COUNTER))
    emit(ops,"SHLI", "TMP14", "TMP15", 3)
    emit(ops,"ADDI", "TMP14", "TMP14", (RCTAB-BASE_LANE)*8)
    emit(ops,"LDX",  "TMP13", "TMP14")
    emit(ops,"XOR",  REG[0], REG[0], "TMP13")

def gen_loopctrl(ops):
    """counter++ ; store ; {XOR sets ZF, UJMPCC CONDNZ -> loop_top}. TMP15=counter."""
    emit(ops,"ADDI", "TMP15", "TMP15", 1)
    emit(ops,"ST",   "TMP15", OFF(COUNTER))
    # the XOR + UJMPCC must be the SAME triad -> emitted as a forced triad later.
    emit(ops,"LOOPTEST", "TMP12", "TMP15", 24)   # XOR TMP12=TMP15^24 ; UJMPCC CONDNZ(TMP12,loop_top)

def gen_epilogue(ops):
    for i in range(25):
        emit(ops,"ST", REG[i], OFF(i))

# ---- simulator (models full looped execution + signed/index addressing) ----
def simulate_perm(init_state):
    rf={}; buf=[0]*BUFLEN
    buf[0:25]=list(init_state)
    buf[COUNTER]=0
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
            elif k=="MOV":  rf[op[1]]=rf[op[2]]&MASK
            elif k=="ADDI": rf[op[1]]=(rf[op[2]]+op[3])&MASK
            elif k=="SHLI": rf[op[1]]=(rf[op[2]]<<op[3])&MASK
            elif k=="LOOPTEST": pass     # XOR sets flags + branch; Python drives the loop
            elif k in ("BASEHI","BASESHL","BASELO"): pass   # rebuild RCX=base; sim addresses by offset
            else: raise ValueError(k)
    pro=[]; gen_prologue(pro); run(pro)
    body=[]; gen_body(body)
    lc=[];   gen_loopctrl(lc)
    for _ in range(24):
        run(body); run(lc)
    epi=[]; gen_epilogue(epi); run(epi)
    return buf[0:25]

# ---- packer (ordered; <=3 ops/triad; <=1 mem; no load-to-use in a triad;
#       LOOPTEST forced into its own triad) ----
def is_mem(op): return op[0] in ("LD","ST","LDX","STX")
def reads_of(op):
    if op[0] in ("XOR","NOTAND"): return (op[2],op[3])
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
    elif k=="MOV":  return f"ZEROEXT_DSZ64_DR({op[1]}, {op[2]})"
    elif k=="LDI_IDX": return f"ZEROEXT_DSZ32_DI({op[1]}, {op[2]})"
    elif k=="BASEHI":  return f"ZEROEXT_DSZ32_DI({op[1]}, (((uintptr_t)(g_keccak_buf+{BASE_LANE}))>>16)&0xffff)"
    elif k=="BASESHL": return f"SHL_DSZ32_DRI({op[1]}, {op[1]}, 16)"
    elif k=="BASELO":  return f"XOR_DSZ64_DRI({op[1]}, {op[1]}, ((uintptr_t)(g_keccak_buf+{BASE_LANE}))&0xffff)"
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

# ====================================================================
# Separate-call round (Phase 4 fix): one round per vmwrite, RC from a buffer
# slot (C supplies RC[round]). 24 separate vmwrites OVERLAP in the OoO engine
# (~0.49 cyc/triad), unlike the in-microcode loop which serializes (~1.3).
# Layout: base RCX=&buf[15]; 0..24 state, 25..29 D scratch, 30 RC slot. BUFLEN 31.
# ====================================================================
SC_BASE=15; SC_DSCR=25
# RC slot on a SEPARATE PAGE from the state (lane 600 = +4680 B from base, > 4KB
# past the state region) so the RC store can't alias the state loads/stores and
# serialize consecutive vmwrites. Addressed via index register (full width).
SC_RC=600; SC_BUFLEN=608
SC_RC_BYTES=(SC_RC-SC_BASE)*8     # index value (bytes) for the far RC slot
def SCOFF(lane): return (lane-SC_BASE)*8
for _l in range(0,31): assert -128 <= SCOFF(_l) <= 127, _l

def gen_scround_ops():
    ops=[]
    # RC arrives in RDX (=REG[3]) from C. Save it to the FAR RC slot (separate
    # page, index-addressed) via a microcode store, BEFORE the prologue overwrites
    # RDX with state lane 3. RAX is free here (theta-C hasn't started).
    emit(ops,"LDI_IDX", "RAX", SC_RC_BYTES)     # RAX = byte offset of far RC slot
    emit(ops,"STX", "RDX", "RAX")               # mem[base+RAX] = RC
    for i in range(25): emit(ops,"LD", REG[i], SCOFF(i))            # prologue (LD RDX<-lane3)
    for x in range(5):                                              # theta C -> C_REG
        emit(ops,"XOR", C_REG[x], REG[x], REG[x+5])
        emit(ops,"XOR", C_REG[x], C_REG[x], REG[x+10])
        emit(ops,"XOR", C_REG[x], C_REG[x], REG[x+15])
        emit(ops,"XOR", C_REG[x], C_REG[x], REG[x+20])
    for x in range(5):                                              # theta D -> buf, borrow REG[24]
        emit(ops,"ROL", BORROW, C_REG[(x+1)%5], 1)
        emit(ops,"XOR", BORROW, C_REG[(x+4)%5], BORROW)
        emit(ops,"ST",  BORROW, SCOFF(SC_DSCR+x))
    emit(ops,"LD", BORROW, SCOFF(24))                               # restore lane 24 from input
    for cyc in pi_cycles():                                         # apply in place
        if len(cyc)==1:
            i=cyc[0]
            emit(ops,"LD", T_DTMP, SCOFF(SC_DSCR+col(i)))
            emit(ops,"XOR", REG[i], REG[i], T_DTMP)
            if RHO[i]: emit(ops,"ROL", REG[i], REG[i], RHO[i])
            continue
        L=len(cyc)
        emit(ops,"MOV", T_SAVE, REG[cyc[L-1]])
        for k in range(L-1,0,-1):
            dst,src=cyc[k],cyc[k-1]
            emit(ops,"LD", T_DTMP, SCOFF(SC_DSCR+col(src)))
            emit(ops,"XOR", REG[dst], REG[src], T_DTMP)
            if RHO[src]: emit(ops,"ROL", REG[dst], REG[dst], RHO[src])
        src=cyc[L-1]
        emit(ops,"LD", T_DTMP, SCOFF(SC_DSCR+col(src)))
        emit(ops,"XOR", REG[cyc[0]], T_SAVE, T_DTMP)
        if RHO[src]: emit(ops,"ROL", REG[cyc[0]], REG[cyc[0]], RHO[src])
    for y in range(5):                                              # chi in place
        r=[5*y+x for x in range(5)]
        emit(ops,"MOV", T_SAVE, REG[r[0]]); emit(ops,"MOV", T_DTMP, REG[r[1]])
        emit(ops,"NOTAND", T_N, REG[r[1]], REG[r[2]]); emit(ops,"XOR", REG[r[0]], REG[r[0]], T_N)
        emit(ops,"NOTAND", T_N, REG[r[2]], REG[r[3]]); emit(ops,"XOR", REG[r[1]], REG[r[1]], T_N)
        emit(ops,"NOTAND", T_N, REG[r[3]], REG[r[4]]); emit(ops,"XOR", REG[r[2]], REG[r[2]], T_N)
        emit(ops,"NOTAND", T_N, REG[r[4]], T_SAVE);    emit(ops,"XOR", REG[r[3]], REG[r[3]], T_N)
        emit(ops,"NOTAND", T_N, T_SAVE, T_DTMP);       emit(ops,"XOR", REG[r[4]], REG[r[4]], T_N)
    # iota: load RC from the far slot (index-addressed). RAX free here (post-chi).
    emit(ops,"LDI_IDX", "RAX", SC_RC_BYTES)
    emit(ops,"LDX", T_N, "RAX"); emit(ops,"XOR", REG[0], REG[0], T_N)
    for i in range(25): emit(ops,"ST", REG[i], SCOFF(i))            # epilogue
    return ops

def simulate_scround(state, rc):
    rf={"RDX":rc}; buf=[0]*SC_BUFLEN; buf[0:25]=list(state)   # RC arrives in RDX
    def at(off): assert -128<=off<=127,off; return SC_BASE+off//8
    for op in gen_scround_ops():
        k=op[0]
        if   k=="LD":  rf[op[1]]=buf[at(op[2])]
        elif k=="ST":  buf[at(op[2])]=rf[op[1]]&MASK
        elif k=="LDI_IDX": rf[op[1]]=op[2]                      # load byte-offset immediate
        elif k=="LDX": rf[op[1]]=buf[SC_BASE + rf[op[2]]//8]    # mem[base+idxreg]
        elif k=="STX": buf[SC_BASE + rf[op[2]]//8]=rf[op[1]]&MASK
        elif k=="XOR": rf[op[1]]=(rf[op[2]]^rf[op[3]])&MASK
        elif k=="ROL": rf[op[1]]=rol(rf[op[2]],op[3])
        elif k=="NOTAND": rf[op[1]]=((~rf[op[2]])&rf[op[3]])&MASK
        elif k=="MOV": rf[op[1]]=rf[op[2]]&MASK
        else: raise ValueError(k)
    return buf[0:25]

def verify_scround(n=64):
    import random; random.seed(7); fails=0
    for t in range(n):
        st=[random.getrandbits(64) for _ in range(25)]
        # full 24-round chain via 24 separate scround calls
        s=list(st)
        for r in range(24): s=simulate_scround(s, RC[r])
        if s!=keccak_perm_ref(st):
            fails+=1; print(f"SCROUND chain trial {t} MISMATCH"); break
    return fails

def emit_scround_c(path):
    triads=pack(gen_scround_ops())
    head=[f"/* AUTO-GENERATED by keccak_gen.py — do not edit. */",
          f"/* Separate-call Keccak round, RC from buf slot. {len(triads)} triads. */",
          f"#define KECCAK_SC_TRIADS {len(triads)}",
          f"#define KECCAK_SC_BUFLEN {SC_BUFLEN}",
          f"#define KECCAK_SC_BASE_LANE {SC_BASE}",
          f"#define KECCAK_SC_RC_LANE {SC_RC}"]
    body=[]
    for ti,t in enumerate(triads):
        s=[op_to_c(o) for o in t]
        while len(s)<3: s.append("NOP")
        seqw="END_SEQWORD" if ti==len(triads)-1 else "NOP_SEQWORD"
        body.append(f"    {{ {s[0]}, {s[1]}, {s[2]}, {seqw} }},")
    open(path,"w").write("\n".join(head)+"\n")
    open(path.replace(".h","_body.h"),"w").write("\n".join(body)+"\n")
    return len(triads)

if __name__=="__main__":
    nf=verify_perm(64)
    if nf: print("PERM SIM FAILED"); sys.exit(1)
    print("PERM SIM OK: 64/64 random permutations match 24-round reference.")
    nt,npro,nbody,nepi=emit_perm_c("keccak_perm.h")
    print(f"perm triads={nt}  (prologue {npro} + body+loop {nbody} + epilogue {nepi})  cap 128")
    print("Wrote keccak_perm.h + keccak_perm_body.h")
    nf=verify_scround(64)
    if nf: print("SCROUND SIM FAILED"); sys.exit(1)
    print("SCROUND SIM OK: 64/64 random 24-round chains match reference.")
    nsc=emit_scround_c("keccak_scround.h")
    print(f"scround triads={nsc}  (one round, separate-call)")
    print("Wrote keccak_scround.h + keccak_scround_body.h")
