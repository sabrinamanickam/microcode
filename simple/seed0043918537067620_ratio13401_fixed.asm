SECTION .text
	GLOBAL fiat_p224_square
fiat_p224_square:
mov rax, [rsi + 0x0]; arg1[0] -> rax
mul rax; x12:x11 <- arg1[0] * arg1[0]
mov r10, rax; spill x11 to r10
mov r11, rdx; spill x12 to r11
mov rax, [rsi + 0x18]; arg1[3] -> rax
mul qword [rsi + 0x0]; x148:x147 <- arg1[3] * arg1[0]
mov rcx, rax; spill x147 to rcx
mov r8, rdx; spill x148 to r8
mov rax, [rsi + 0x18]; arg1[3] -> rax
mul qword [rsi + 0x8]; x146:x145 <- arg1[3] * arg1[1]
mov r9, rax; spill x145 to r9
mov [ rsp - 0x80 ], rbx; spilling calSv-rbx to mem
mov rbx, rdx; spill x146 to rbx
mov rax, [rsi + 0x8]; arg1[1] -> rax
mul qword [rsi + 0x0]; x50:x49 <- arg1[1] * arg1[0]
mov [ rsp - 0x78 ], rbp; spilling calSv-rbp to mem
mov rbp, rax; spill x49 to rbp
mov [ rsp - 0x70 ], r12; spilling calSv-r12 to mem
mov r12, rdx; spill x50 to r12
mov rax, [rsi + 0x0]; arg1[0] -> rax
mul qword [rsi + 0x10]; x8:x7 <- arg1[0] * arg1[2]
add r9, r8; could be done better, if r0 has been u8 as well
mov r8, rax; spill x7 to r8
mov [ rsp - 0x68 ], r13; spilling calSv-r13 to mem
mov r13, rdx; spill x8 to r13
mov rax, [rsi + 0x10]; arg1[2] -> rax
setc byte [ rsp - 0x200 ]; save CF (x150) to stack before mul
mul qword [rsi + 0x0]; x99:x98 <- arg1[2] * arg1[0]
mov [ rsp - 0x60 ], r14; spilling calSv-r14 to mem
mov r14, rax; spill x98 to r14
mov [ rsp - 0x58 ], r15; spilling calSv-r15 to mem
mov r15, rdx; spill x99 to r15
mov rax, [rsi + 0x10]; arg1[2] -> rax
mul qword [rsi + 0x18]; x93:x92 <- arg1[2] * arg1[3]
mov [ rsp - 0x208 ], rax; spill x92 to stack
mov [ rsp - 0x210 ], rdx; spill x93 to stack
mov rax, [rsi + 0x8]; arg1[1] -> rax
mul qword [rsi + 0x10]; x46:x45 <- arg1[1] * arg1[2]
mov [ rsp - 0x218 ], rax; spill x45 to stack
mov [ rsp - 0x220 ], rdx; spill x46 to stack
mov rax, r10; x11 -> rax
mov rdx, 0xffffffffffffffff; load immediate for mul
mul rdx; _:x20 <- x11 * 0xffffffffffffffff
mov rdx, rax;
add rdx, r10; could be done better, if r0 has been u8 as well
mov r10, rax; copy x20 before mul
setc byte [ rsp - 0x228 ]; save CF (x34) to stack before mul
mov rdx, 0xffffffffffffffff; load immediate for mul
mul rdx; x25:x24 <- x20 * 0xffffffffffffffff
mov [ rsp - 0x230 ], rax; spill x24 to stack
mov [ rsp - 0x238 ], rdx; spill x25 to stack
mov rax, r10; x20 -> rax
mov rdx, 0xffffffff; load immediate for mul
mul rdx; x23:x22 <- x20 * 0xffffffff
mov [ rsp - 0x240 ], rax; spill x22 to stack
mov [ rsp - 0x248 ], rdx; spill x23 to stack
mov rax, [rsi + 0x0]; arg1[0] -> rax
mul qword [rsi + 0x8]; x10:x9 <- arg1[0] * arg1[1]
mov [ rsp - 0x250 ], rax; spill x9 to stack
mov [ rsp - 0x258 ], rdx; spill x10 to stack
mov rax, r10; x20 -> rax
mov rdx, 0xffffffff00000000; load immediate for mul
mul rdx; x27:x26 <- x20 * 0xffffffff00000000
mov r10, rax; spill x26 to r10
mov [ rsp - 0x260 ], rdx; spill x27 to stack
mov rax, [rsi + 0x18]; arg1[3] -> rax
mul qword [rsi + 0x10]; x144:x143 <- arg1[3] * arg1[2]
add r11, [ rsp - 0x250 ]; could be done better, if r0 has been u8 as well
adc r8, [ rsp - 0x258 ]
mov [ rsp - 0x268 ], rax; spill x143 to stack
mov [ rsp - 0x270 ], rdx; spill x144 to stack
mov rax, [rsi + 0x8]; arg1[1] -> rax
setc byte [ rsp - 0x278 ]; save CF (x16) to stack before mul
mul qword [rsi + 0x18]; x44:x43 <- arg1[1] * arg1[3]
add byte [ rsp - 0x200 ], 0xFF; load cin→CF
adc rbx, [ rsp - 0x268 ]
mov [ rsp - 0x280 ], rax; spill x43 to stack
mov [ rsp - 0x288 ], rdx; spill x44 to stack
mov rax, [rsi + 0x10]; arg1[2] -> rax
setc byte [ rsp - 0x290 ]; save CF (x152) to stack before mul
mul qword [rsi + 0x8]; x97:x96 <- arg1[2] * arg1[1]
add rax, r15; could be done better, if r0 has been u8 as well
mov r15, rax; spill x100 to r15
mov [ rsp - 0x298 ], rdx; spill x97 to stack
mov rax, [rsi + 0x8]; arg1[1] -> rax
setc byte [ rsp - 0x2a0 ]; save CF (x101) to stack before mul
mul rax; x48:x47 <- arg1[1] * arg1[1]
add byte [ rsp - 0x228 ], 0xFF; load cin→CF
adc r11, r10
setc r10b;
add rax, r12; could be done better, if r0 has been u8 as well
mov r12, rax; spill x51 to r12
mov [ rsp - 0x2a8 ], rdx; spill x48 to stack
mov rax, [rsi + 0x18]; arg1[3] -> rax
setc byte [ rsp - 0x2b0 ]; save CF (x52) to stack before mul
mul rax; x142:x141 <- arg1[3] * arg1[3]
mov [ rsp - 0x50 ], rdi; spilling out1 to mem
mov rdi, [ rsp - 0x230 ]; load m64 x24 to register64
add rdi, [ rsp - 0x260 ]; could be done better, if r0 has been u8 as well
mov [ rsp - 0x48 ], rbx; spilling x151 to mem
setc bl;
add byte [ rsp - 0x290 ], 0xFF; load cin→CF
adc rax, [ rsp - 0x270 ]
mov [ rsp - 0x40 ], rax; spilling x153 to mem
setc al;
add rbp, r11; could be done better, if r0 has been u8 as well
movzx r11, al; spill x154 to r11 (was u1, now u64)
mov [ rsp - 0x230 ], rdx; spill x142 to stack
mov rax, rbp; x58 -> rax
setc byte [ rsp - 0x2b8 ]; save CF (x59) to stack before mul
mov rdx, 0xffffffffffffffff; load immediate for mul
mul rdx; _:x68 <- x58 * 0xffffffffffffffff
mov [ rsp - 0x2c0 ], rax; copy x68 to stack before mul
mov rdx, 0xffffffff00000000; load immediate for mul
mul rdx; x75:x74 <- x68 * 0xffffffff00000000
mov [ rsp - 0x2c8 ], rax; spill x74 to stack
mov [ rsp - 0x2d0 ], rdx; spill x75 to stack
mov rax, [ rsp - 0x2c0 ]; x68 -> rax
mov rdx, 0xffffffffffffffff; load immediate for mul
mul rdx; x73:x72 <- x68 * 0xffffffffffffffff
add rax, [ rsp - 0x2d0 ]; could be done better, if r0 has been u8 as well
mov [ rsp - 0x2d8 ], rax; spill x76 to stack
mov [ rsp - 0x2e0 ], rdx; spill x73 to stack
mov rax, [ rsp - 0x2c0 ]; x68 -> rax
setc byte [ rsp - 0x2e8 ]; save CF (x77) to stack before mul
mov rdx, 0xffffffff; load immediate for mul
mul rdx; x71:x70 <- x68 * 0xffffffff
mov [ rsp - 0x2f0 ], rax; spill x70 to stack
mov [ rsp - 0x2f8 ], rdx; spill x71 to stack
mov rax, [rsi + 0x0]; arg1[0] -> rax
mul qword [rsi + 0x18]; x6:x5 <- arg1[0] * arg1[3]
mov [ rsp - 0x38 ], r11; spilling x154 to mem
mov r11, [ rsp - 0x238 ]; load m64 x25 to register64
add bl, 0xFF; load cin→CF
adc r11, [ rsp - 0x240 ]
mov rbx, rax; spill x5 to rbx
mov [ rsp - 0x238 ], rdx; spill x6 to stack
mov rax, [rsi + 0x10]; arg1[2] -> rax
setc byte [ rsp - 0x300 ]; save CF (x31) to stack before mul
mul rax; x95:x94 <- arg1[2] * arg1[2]
add byte [ rsp - 0x278 ], 0xFF; load cin→CF
adc r13, rbx
movzx rbx, byte [ rsp - 0x300 ];
mov [ rsp - 0x30 ], r9; spilling x149 to mem
mov r9, [ rsp - 0x248 ]; load m64 x23 to register64
lea rbx, [ rbx + r9 ]; r8/64 + m8
setc r9b;
add r10b, 0xFF; load cin→CF
adc r8, rdi
setc r10b;
add rbp, [ rsp - 0x2c0 ]; could be done better, if r0 has been u8 as well
mov rbp, [ rsp - 0x2a8 ]; load m64 x48 to register64
setc dil;
add byte [ rsp - 0x2b0 ], 0xFF; load cin→CF
adc rbp, [ rsp - 0x218 ]
mov [ rsp - 0x28 ], rcx; spilling x147 to mem
mov rcx, [ rsp - 0x280 ]; load m64 x43 to register64
adc rcx, [ rsp - 0x220 ]
mov [ rsp - 0x20 ], r15; spilling x100 to mem
setc r15b;
add r10b, 0xFF; load cin→CF
adc r13, r11
movzx r11, r9b;
mov r10, [ rsp - 0x238 ]; load m64 x6 to register64
lea r11, [ r11 + r10 ]; r8/64 + m8
adc rbx, r11
setc r10b;
add byte [ rsp - 0x2a0 ], 0xFF; load cin→CF
adc rax, [ rsp - 0x298 ]
adc rdx, [ rsp - 0x208 ]
mov r9, [ rsp - 0x210 ];
adc r9, 0x0; add CF to r0's alloc
add byte [ rsp - 0x2b8 ], 0xFF; load cin→CF
adc r8, r12
setc r12b;
add dil, 0xFF; load cin→CF
adc r8, [ rsp - 0x2c8 ]
setc dil;
add r12b, 0xFF; load cin→CF
adc r13, rbp
setc bpl;
add dil, 0xFF; load cin→CF
adc r13, [ rsp - 0x2d8 ]
mov r11, [ rsp - 0x2f0 ]; load m64 x70 to register64
setc r12b;
add byte [ rsp - 0x2e8 ], 0xFF; load cin→CF
adc r11, [ rsp - 0x2e0 ]
mov rdi, [ rsp - 0x2f8 ];
adc rdi, 0x0; add CF to r0's alloc
add bpl, 0xFF; load cin→CF
adc rbx, rcx
movzx rcx, r15b;
mov rbp, [ rsp - 0x288 ]; load m64 x44 to register64
lea rcx, [ rcx + rbp ]; r8/64 + m8
setc bpl;
add r14, r8; could be done better, if r0 has been u8 as well
setc r15b;
movzx r10, r10b
add bpl, 0xFF; load cin→CF
adc rcx, r10
mov r10, rax; spill x102 to r10
mov r8, rdx; spill x104 to r8
mov rax, r14; x107 -> rax
setc byte [ rsp - 0x238 ]; save CF (x67) to stack before mul
mov rbp, 0xffffffffffffffff; load immediate for mul
mul rbp; _:x117 <- x107 * 0xffffffffffffffff
mov rdx, rax;
add rdx, r14; could be done better, if r0 has been u8 as well
mov rbp, rax; copy x117 before mul
setc byte [ rsp - 0x248 ]; save CF (x131) to stack before mul
mov r14, 0xffffffff; load immediate for mul
mul r14; x120:x119 <- x117 * 0xffffffff
add r12b, 0xFF; load cin→CF
adc rbx, r11
adc rdi, rcx
mov r14, rax; spill x119 to r14
mov r12, rdx; spill x120 to r12
mov rax, rbp; x117 -> rax
setc byte [ rsp - 0x280 ]; save CF (x90) to stack before mul
mov r11, 0xffffffff00000000; load immediate for mul
mul r11; x124:x123 <- x117 * 0xffffffff00000000
mov r11, rax; spill x123 to r11
mov rcx, rdx; spill x124 to rcx
mov rax, rbp; x117 -> rax
mov rdx, 0xffffffffffffffff; load immediate for mul
mul rdx; x122:x121 <- x117 * 0xffffffffffffffff
add rax, rcx; could be done better, if r0 has been u8 as well
setc bpl;
add r15b, 0xFF; load cin→CF
adc r13, [ rsp - 0x20 ]
setc r15b;
add bpl, 0xFF; load cin→CF
adc rdx, r14
adc r12, 0x0; add CF to r0's alloc
add byte [ rsp - 0x248 ], 0xFF; load cin→CF
adc r13, r11
setc r14b;
add r13, [ rsp - 0x28 ]; could be done better, if r0 has been u8 as well
mov rcx, rax; spill x125 to rcx
mov r11, rdx; spill x127 to r11
mov rax, r13; x156 -> rax
setc byte [ rsp - 0x288 ]; save CF (x157) to stack before mul
mov rbp, 0xffffffffffffffff; load immediate for mul
mul rbp; _:x166 <- x156 * 0xffffffffffffffff
mov rbp, rax; copy x166 before mul
mov rdx, 0xffffffff; load immediate for mul
mul rdx; x169:x168 <- x166 * 0xffffffff
add r15b, 0xFF; load cin→CF
adc rbx, r10
adc r8, rdi
mov r10, rax; spill x168 to r10
mov rdi, rdx; spill x169 to rdi
mov rax, rbp; x166 -> rax
setc byte [ rsp - 0x2a8 ]; save CF (x114) to stack before mul
mov r15, 0xffffffffffffffff; load immediate for mul
mul r15; x171:x170 <- x166 * 0xffffffffffffffff
mov r15, rax; spill x170 to r15
mov [ rsp - 0x2f0 ], rdx; spill x171 to stack
mov rax, rbp; x166 -> rax
mov rdx, 0xffffffff00000000; load immediate for mul
mul rdx; x173:x172 <- x166 * 0xffffffff00000000
add r15, rdx; could be done better, if r0 has been u8 as well
setc dl;
add r14b, 0xFF; load cin→CF
adc rbx, rcx
setc cl;
add byte [ rsp - 0x288 ], 0xFF; load cin→CF
adc rbx, [ rsp - 0x30 ]
mov r14, [ rsp - 0x38 ];
mov [ rsp - 0x18 ], rdi; spilling x169 to mem
mov rdi, [ rsp - 0x230 ]; load m64 x142 to register64
lea r14, [ r14 + rdi ]; r8/64 + m8
setc dil;
add rbp, r13; could be done better, if r0 has been u8 as well
movzx rbp, byte [ rsp - 0x280 ];
movzx r13, byte [ rsp - 0x238 ]; load byte memx67 to register64
lea rbp, [ rbp + r13 ]; r64+m8
adc rax, rbx
setc r13b;
add cl, 0xFF; load cin→CF
adc r8, r11
setc r11b;
add byte [ rsp - 0x2a8 ], 0xFF; load cin→CF
adc rbp, r9
setc r9b;
add r11b, 0xFF; load cin→CF
adc rbp, r12
setc r12b;
add dil, 0xFF; load cin→CF
adc r8, [ rsp - 0x48 ]
setc cl;
add r13b, 0xFF; load cin→CF
adc r8, r15
setc r15b;
add dl, 0xFF; load cin→CF
adc r10, [ rsp - 0x2f0 ]
movzx rdx, r12b;
movzx r9, r9b
lea rdx, [ rdx + r9 ]
mov rbx, [ rsp - 0x18 ];
adc rbx, 0x0; add CF to r0's alloc
add cl, 0xFF; load cin→CF
adc rbp, [ rsp - 0x40 ]
setc dil;
add r15b, 0xFF; load cin→CF
adc rbp, r10
setc r13b;
mov r11, rax;
sub r11, 0x1
mov r9, 0xffffffff00000000 ; moving imm to reg
mov r12, r8;
sbb r12, r9
setc cl;
add dil, 0xFF; load cin→CF
adc rdx, r14
setc r14b;
add r13b, 0xFF; load cin→CF
adc rdx, rbx
setc r15b;
mov r10, -0x1 ; moving imm to reg
add r10b, cl; load to CF<-x193
mov r10, 0xffffffffffffffff ; moving imm to reg
mov rbx, rbp;
sbb rbx, r10
movzx rdi, r15b;
movzx r14, r14b
lea rdi, [ rdi + r14 ]
mov r13, 0xffffffff ; moving imm to reg
mov rcx, rdx;
sbb rcx, r13
mov r14, 0x0 ; moving imm to reg
sbb rdi, r14
cmovc rcx, rdx; if CF, x203<- x187 (nzVar)
cmovc r12, r8; if CF, x201<- x183 (nzVar)
mov rdi, [ rsp - 0x50 ]; load m64 out1 to register64
mov [ rdi + 0x8 ], r12; out1[1] = x201
cmovc r11, rax; if CF, x200<- x181 (nzVar)
mov [ rdi + 0x0 ], r11; out1[0] = x200
mov [ rdi + 0x18 ], rcx; out1[3] = x203
cmovc rbx, rbp; if CF, x202<- x185 (nzVar)
mov [ rdi + 0x10 ], rbx; out1[2] = x202
mov rbx, [ rsp - 0x80 ]; pop
mov rbp, [ rsp - 0x78 ]; pop
mov r12, [ rsp - 0x70 ]; pop
mov r13, [ rsp - 0x68 ]; pop
mov r14, [ rsp - 0x60 ]; pop
mov r15, [ rsp - 0x58 ]; pop
ret
; cpu Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
; ratio 1.3401
; seed 0043918537067620 
; CC / CFLAGS gcc / -march=native -mtune=native -O3 
; cyclegoal; 10000
; using counter; PMC
; framePointer omit
; memoryConstraints none
; time needed: 12961 ms on 200 evaluations.
; Time spent for assembling and measuring (initial batch_size=18, initial num_batches=31): 475 ms
; number of used evaluations: 200
; Ratio (time for assembling + measure)/(total runtime for 200 evals): 0.036648406758737755
; number reverted permutation / tried permutation: 36 / 98 =36.735%
; number reverted decision / tried decision: 47 / 101 =46.535%