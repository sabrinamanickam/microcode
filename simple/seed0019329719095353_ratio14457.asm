SECTION .text
	GLOBAL fiat_p256_square
fiat_p256_square:
sub rsp, 144
mov rax, [rsi + 0x8]; arg1[1] -> rax
mul rax; x44:x43 <- arg1[1] * arg1[1]
mov r10, rax; spill x43 to r10
mov r11, rdx; spill x44 to r11
mov rax, [rsi + 0x18]; arg1[3] -> rax
mul [rsi + 0x0]; x136:x135 <- arg1[3] * arg1[0]
mov rcx, rax; spill x135 to rcx
mov r8, rdx; spill x136 to r8
mov rax, [rsi + 0x8]; arg1[1] -> rax
mul [rsi + 0x18]; x40:x39 <- arg1[1] * arg1[3]
mov r9, rax; spill x39 to r9
mov [ rsp - 0x80 ], rbx; spilling calSv-rbx to mem
mov rbx, rdx; spill x40 to rbx
mov rax, [rsi + 0x10]; arg1[2] -> rax
mul [rsi + 0x0]; x91:x90 <- arg1[2] * arg1[0]
mov [ rsp - 0x78 ], rbp; spilling calSv-rbp to mem
mov rbp, rax; spill x90 to rbp
mov [ rsp - 0x70 ], r12; spilling calSv-r12 to mem
mov r12, rdx; spill x91 to r12
mov rax, [rsi + 0x18]; arg1[3] -> rax
mul [rsi + 0x8]; x134:x133 <- arg1[3] * arg1[1]
mov [ rsp - 0x68 ], r13; spilling calSv-r13 to mem
mov r13, rax; spill x133 to r13
mov [ rsp - 0x60 ], r14; spilling calSv-r14 to mem
mov r14, rdx; spill x134 to r14
mov rax, [rsi + 0x8]; arg1[1] -> rax
mul [rsi + 0x10]; x42:x41 <- arg1[1] * arg1[2]
mov [ rsp - 0x58 ], r15; spilling calSv-r15 to mem
mov r15, rax; spill x41 to r15
mov [ rsp - 0x200 ], rdx; spill x42 to stack
mov rax, [rsi + 0x18]; arg1[3] -> rax
mul [rsi + 0x10]; x132:x131 <- arg1[3] * arg1[2]
mov [ rsp - 0x208 ], rax; spill x131 to stack
mov [ rsp - 0x210 ], rdx; spill x132 to stack
mov rax, [rsi + 0x10]; arg1[2] -> rax
mul [rsi + 0x18]; x85:x84 <- arg1[2] * arg1[3]
mov [ rsp - 0x218 ], rax; spill x84 to stack
mov [ rsp - 0x220 ], rdx; spill x85 to stack
mov rax, [rsi + 0x18]; arg1[3] -> rax
mul rax; x130:x129 <- arg1[3] * arg1[3]
mov [ rsp - 0x228 ], rax; spill x129 to stack
mov [ rsp - 0x230 ], rdx; spill x130 to stack
mov rax, [rsi + 0x8]; arg1[1] -> rax
mul [rsi + 0x0]; x46:x45 <- arg1[1] * arg1[0]
add r10, rdx; could be done better, if r0 has been u8 as well
mov rdx, rax; spill x45 to rdx
mov [ rsp - 0x238 ], rdx; spill x45 to stack
mov rax, [rsi + 0x0]; arg1[0] -> rax
setc byte [ rsp - 0x240 ]; save CF (x48) to stack before mul
mul rax; x12:x11 <- arg1[0] * arg1[0]
mov [ rsp - 0x248 ], rax; spill x11 to stack
mov [ rsp - 0x250 ], rdx; spill x12 to stack
mov rax, [rsi + 0x0]; arg1[0] -> rax
mul [rsi + 0x8]; x10:x9 <- arg1[0] * arg1[1]
mov [ rsp - 0x258 ], rax; spill x9 to stack
mov [ rsp - 0x260 ], rdx; spill x10 to stack
mov rax, [ rsp - 0x248 ]; x11 -> rax
mov rdx, 0xffffffffffffffff; load immediate for mul
mul rdx; x25:x24 <- x11 * 0xffffffffffffffff
add rax, [ rsp - 0x248 ]; could be done better, if r0 has been u8 as well
mov [ rsp - 0x268 ], rdx; spill x25 to stack
mov rax, [rsi + 0x10]; arg1[2] -> rax
setc byte [ rsp - 0x270 ]; save CF (x30) to stack before mul
mul [rsi + 0x8]; x89:x88 <- arg1[2] * arg1[1]
mov [ rsp - 0x278 ], rax; spill x88 to stack
mov [ rsp - 0x280 ], rdx; spill x89 to stack
mov rax, [ rsp - 0x248 ]; x11 -> rax
mov rdx, 0xffffffff; load immediate for mul
mul rdx; x23:x22 <- x11 * 0xffffffff
mov [ rsp - 0x50 ], rdi; spilling out1 to mem
mov rdi, [ rsp - 0x250 ]; load m64 x12 to register64
add rdi, [ rsp - 0x258 ]; could be done better, if r0 has been u8 as well
mov [ rsp - 0x250 ], rax; spill x22 to stack
mov [ rsp - 0x288 ], rdx; spill x23 to stack
mov rax, [rsi + 0x0]; arg1[0] -> rax
setc byte [ rsp - 0x290 ]; save CF (x14) to stack before mul
mul [rsi + 0x18]; x6:x5 <- arg1[0] * arg1[3]
mov [ rsp - 0x298 ], rax; spill x5 to stack
mov [ rsp - 0x2a0 ], rdx; spill x6 to stack
mov rax, [rsi + 0x0]; arg1[0] -> rax
mul [rsi + 0x10]; x8:x7 <- arg1[0] * arg1[2]
mov [ rsp - 0x2a8 ], rax; spill x7 to stack
mov [ rsp - 0x2b0 ], rdx; spill x8 to stack
mov rax, [rsi + 0x10]; arg1[2] -> rax
mul rax; x87:x86 <- arg1[2] * arg1[2]
mov [ rsp - 0x48 ], rcx; spilling x135 to mem
mov rcx, [ rsp - 0x260 ]; load m64 x10 to register64
add byte [ rsp - 0x290 ], 0xFF; load cin→CF
adc rcx, [ rsp - 0x2a8 ]
mov [ rsp - 0x40 ], rbx; spilling x40 to mem
mov rbx, [ rsp - 0x298 ]; load m64 x5 to register64
adc rbx, [ rsp - 0x2b0 ]
mov [ rsp - 0x38 ], rbp; spilling x90 to mem
mov rbp, [ rsp - 0x2a0 ];
adc rbp, 0x0; add CF to r0's alloc
add r12, [ rsp - 0x278 ]; could be done better, if r0 has been u8 as well
mov [ rsp - 0x30 ], r12; spilling x92 to mem
setc r12b;
add r13, r8; could be done better, if r0 has been u8 as well
mov r8, [ rsp - 0x268 ]; load m64 x25 to register64
mov [ rsp - 0x28 ], r13; spilling x137 to mem
setc r13b;
add r8, [ rsp - 0x250 ]; could be done better, if r0 has been u8 as well
mov [ rsp - 0x20 ], r9; spilling x39 to mem
setc r9b;
add byte [ rsp - 0x270 ], 0xFF; load cin→CF
adc rdi, r8
setc r8b;
add r13b, 0xFF; load cin→CF
adc r14, [ rsp - 0x208 ]
setc r13b;
add rdi, [ rsp - 0x238 ]; could be done better, if r0 has been u8 as well
mov [ rsp - 0x18 ], r14; spilling x139 to mem
movzx r14, r9b;
mov [ rsp - 0x10 ], rdx; spilling x87 to mem
mov rdx, [ rsp - 0x288 ]; load m64 x23 to register64
lea r14, [ r14 + rdx ]; r8/64 + m8
mov rdx, rax; spill x86 to rdx
mov r9, rdx; spill x86 to r9
mov rax, rdi; x54 -> rax
setc byte [ rsp - 0x260 ]; save CF (x55) to stack before mul
mov rdx, 0xffffffff00000001; load immediate for mul
mul rdx; x65:x64 <- x54 * 0xffffffff00000001
mov [ rsp - 0x268 ], rax; spill x64 to stack
mov [ rsp - 0x288 ], rdx; spill x65 to stack
mov rax, [ rsp - 0x248 ]; x11 -> rax
mov rdx, 0xffffffff00000001; load immediate for mul
mul rdx; x21:x20 <- x11 * 0xffffffff00000001
mov [ rsp - 0x8 ], r9; spilling x86 to mem
mov r9, [ rsp - 0x210 ]; load m64 x132 to register64
add r13b, 0xFF; load cin→CF
adc r9, [ rsp - 0x228 ]
setc r13b;
add r8b, 0xFF; load cin→CF
adc rcx, r14
adc rax, rbx
setc bl;
add byte [ rsp - 0x240 ], 0xFF; load cin→CF
adc r11, r15
setc r15b;
add byte [ rsp - 0x260 ], 0xFF; load cin→CF
adc rcx, r10
adc r11, rax
setc r10b;
add bl, 0xFF; load cin→CF
adc rbp, rdx
mov rax, rdi; x54 -> rax
setc byte [ rsp - 0x210 ]; save CF (x38) to stack before mul
mov r8, 0xffffffff; load immediate for mul
mul r8; x67:x66 <- x54 * 0xffffffff
mov r8, rax; spill x66 to r8
mov r14, rdx; spill x67 to r14
mov rax, rdi; x54 -> rax
mov rbx, 0xffffffffffffffff; load immediate for mul
mul rbx; x69:x68 <- x54 * 0xffffffffffffffff
add r8, rdx; could be done better, if r0 has been u8 as well
adc r14, 0x0; add CF to r0's alloc
mov rbx, [ rsp - 0x280 ]; load m64 x89 to register64
add r12b, 0xFF; load cin→CF
adc rbx, [ rsp - 0x8 ]
mov r12, [ rsp - 0x10 ]; load m64 x87 to register64
adc r12, [ rsp - 0x218 ]
setc dl;
add rax, rdi; could be done better, if r0 has been u8 as well
adc r8, rcx
mov rax, [ rsp - 0x200 ]; load m64 x42 to register64
setc dil;
add r15b, 0xFF; load cin→CF
adc rax, [ rsp - 0x20 ]
setc r15b;
add r8, [ rsp - 0x38 ]; could be done better, if r0 has been u8 as well
movzx rcx, r13b;
mov [ rsp + 0x0 ], r9; spilling x141 to mem
mov r9, [ rsp - 0x230 ]; load m64 x130 to register64
lea rcx, [ rcx + r9 ]; r8/64 + m8
mov r9, rax; spill x51 to r9
movzx r13, dl; spill x97 to r13 (was u1, now u64)
mov rax, r8; x99 -> rax
setc byte [ rsp - 0x200 ]; save CF (x100) to stack before mul
mov rdx, 0xffffffff; load immediate for mul
mul rdx; x112:x111 <- x99 * 0xffffffff
mov [ rsp + 0x8 ], rcx; spilling x143 to mem
movzx rcx, r15b;
add rcx, [ rsp - 0x40 ]
mov r15, rax; spill x111 to r15
mov [ rsp - 0x230 ], rdx; spill x112 to stack
mov rax, r8; x99 -> rax
mov rdx, 0xffffffffffffffff; load immediate for mul
mul rdx; x114:x113 <- x99 * 0xffffffffffffffff
mov [ rsp - 0x280 ], rax; spill x113 to stack
mov [ rsp - 0x298 ], rdx; spill x114 to stack
mov rax, r8; x99 -> rax
mov rdx, 0xffffffff00000001; load immediate for mul
mul rdx; x110:x109 <- x99 * 0xffffffff00000001
add r10b, 0xFF; load cin→CF
adc rbp, r9
movzx r10, byte [ rsp - 0x210 ];
adc r10, rcx
setc r9b;
add dil, 0xFF; load cin→CF
adc r11, r14
adc rbp, [ rsp - 0x268 ]
adc r10, [ rsp - 0x288 ]
setc r14b;
add byte [ rsp - 0x200 ], 0xFF; load cin→CF
adc r11, [ rsp - 0x30 ]
adc rbx, rbp
setc dil;
add r8, [ rsp - 0x280 ]; could be done better, if r0 has been u8 as well
setc r8b;
add r15, [ rsp - 0x298 ]; could be done better, if r0 has been u8 as well
setc cl;
add r8b, 0xFF; load cin→CF
adc r11, r15
movzx rbp, cl;
mov r8, [ rsp - 0x230 ]; load m64 x112 to register64
lea rbp, [ rbp + r8 ]; r8/64 + m8
movzx r8, r14b;
movzx r9, r9b
lea r8, [ r8 + r9 ]
setc r9b;
add r11, [ rsp - 0x48 ]; could be done better, if r0 has been u8 as well
setc r14b;
add r9b, 0xFF; load cin→CF
adc rbx, rbp
mov r15, rax; spill x109 to r15
mov rcx, rdx; spill x110 to rcx
mov rax, r11; x144 -> rax
setc byte [ rsp - 0x230 ]; save CF (x123) to stack before mul
mov r9, 0xffffffffffffffff; load immediate for mul
mul r9; x159:x158 <- x144 * 0xffffffffffffffff
mov r9, rax; spill x158 to r9
mov rbp, rdx; spill x159 to rbp
mov rax, r11; x144 -> rax
mov rdx, 0xffffffff; load immediate for mul
mul rdx; x157:x156 <- x144 * 0xffffffff
add rax, rbp; could be done better, if r0 has been u8 as well
setc bpl;
add r9, r11; could be done better, if r0 has been u8 as well
setc r9b;
add r14b, 0xFF; load cin→CF
adc rbx, [ rsp - 0x28 ]
mov r14, [ rsp - 0x220 ]; load m64 x85 to register64
lea r13, [ r13 + r14 ]; r8/64 + m8
setc r14b;
add r9b, 0xFF; load cin→CF
adc rbx, rax
setc al;
add dil, 0xFF; load cin→CF
adc r10, r12
setc r12b;
add byte [ rsp - 0x230 ], 0xFF; load cin→CF
adc r10, r15
setc r15b;
add r12b, 0xFF; load cin→CF
adc r8, r13
movzx rdi, bpl;
lea rdi, [ rdi + rdx ]
setc dl;
add r14b, 0xFF; load cin→CF
adc r10, [ rsp - 0x18 ]
setc bpl;
mov r9, 0xffffffffffffffff ; moving imm to reg
mov r14, rbx;
sub r14, r9
setc r13b;
add al, 0xFF; load cin→CF
adc r10, rdi
setc al;
mov r12, -0x1 ; moving imm to reg
add r12b, r13b; load to CF<-x175
mov r12, 0xffffffff ; moving imm to reg
mov rdi, r10;
sbb rdi, r12
setc r13b;
add r15b, 0xFF; load cin→CF
adc r8, rcx
setc cl;
add bpl, 0xFF; load cin→CF
adc r8, [ rsp + 0x0 ]
movzx r15, al; spill x168 to r15 (was u1, now u64)
movzx rbp, dl; spill x108 to rbp (was u1, now u64)
mov rax, r11; x144 -> rax
setc byte [ rsp - 0x220 ]; save CF (x151) to stack before mul
mov rdx, 0xffffffff00000001; load immediate for mul
mul rdx; x155:x154 <- x144 * 0xffffffff00000001
movzx r11, cl;
lea r11, [ r11 + rbp ]
add r15b, 0xFF; load cin→CF
adc r8, rax
setc bpl;
add byte [ rsp - 0x220 ], 0xFF; load cin→CF
adc r11, [ rsp + 0x8 ]
setc r15b;
add bpl, 0xFF; load cin→CF
adc r11, rdx
setc cl;
mov rdx, -0x1 ; moving imm to reg
add dl, r13b; load to CF<-x177
mov rdx, 0x0 ; moving imm to reg
mov rax, r8;
sbb rax, rdx
movzx r13, cl;
movzx r15, r15b
lea r13, [ r13 + r15 ]
mov rbp, 0xffffffff00000001 ; moving imm to reg
mov r15, r11;
sbb r15, rbp
sbb r13, rdx
cmovc rdi, r10; if CF, x185<- x167 (nzVar)
mov r13, [ rsp - 0x50 ]; load m64 out1 to register64
mov [ r13 + 0x8 ], rdi; out1[1] = x185
cmovc r15, r11; if CF, x187<- x171 (nzVar)
mov [ r13 + 0x18 ], r15; out1[3] = x187
cmovc rax, r8; if CF, x186<- x169 (nzVar)
mov [ r13 + 0x10 ], rax; out1[2] = x186
cmovc r14, rbx; if CF, x184<- x165 (nzVar)
mov [ r13 + 0x0 ], r14; out1[0] = x184
mov rbx, [ rsp - 0x80 ]; pop
mov rbp, [ rsp - 0x78 ]; pop
mov r12, [ rsp - 0x70 ]; pop
mov r13, [ rsp - 0x68 ]; pop
mov r14, [ rsp - 0x60 ]; pop
mov r15, [ rsp - 0x58 ]; pop
add rsp, 144
ret
; cpu Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
; ratio 1.4457
; seed 0019329719095353 
; CC / CFLAGS gcc / -march=native -mtune=native -O3 
; cyclegoal; 10000
; using counter; PMC
; framePointer omit
; memoryConstraints none
; time needed: 12055 ms on 200 evaluations.
; Time spent for assembling and measuring (initial batch_size=20, initial num_batches=31): 451 ms
; number of used evaluations: 200
; Ratio (time for assembling + measure)/(total runtime for 200 evals): 0.03741186229780174
; number reverted permutation / tried permutation: 60 / 107 =56.075%
; number reverted decision / tried decision: 47 / 92 =51.087%
