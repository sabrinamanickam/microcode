SECTION .text
	GLOBAL fiat_curve25519_carry_square
fiat_curve25519_carry_square:
imul rax, [ rsi + 0x20 ], 0x26; x2 <- arg1[4] * 0x26
mov r10, rax; spill x2 to r10
mov rax, [rsi + 0x10]; arg1[2] -> rax
mul rax; x14_1:x14_0 <- arg1[2] * arg1[2]
mov r11, [ rsi + 0x8 ]; load m64 arg1[1] to register64
lea rcx, [r11 + r11]; x8 <- arg1[1] * 2 
mov r11, rax; spill x14_0 to r11
mov r8, rdx; spill x14_1 to r8
mov rax, [rsi + 0x8]; arg1[1] -> rax
mul rax; x18_1:x18_0 <- arg1[1] * arg1[1]
imul r9, [ rsi + 0x18 ], 0x26; x5 <- arg1[3] * 0x26
mov [ rsp - 0x80 ], rbx; spilling calSv-rbx to mem
mov rbx, rax; spill x18_0 to rbx
mov [ rsp - 0x78 ], rbp; spilling calSv-rbp to mem
mov rbp, rdx; spill x18_1 to rbp
mov rax, [rsi + 0x0]; arg1[0] -> rax
mul rax; x23_1:x23_0 <- arg1[0] * arg1[0]
mov [ rsp - 0x70 ], r12; spilling calSv-r12 to mem
imul r12, [ rsi + 0x20 ], 0x2; x3 <- arg1[4] * 0x2
mov [ rsp - 0x68 ], r13; spilling calSv-r13 to mem
mov r13, rax; spill x23_0 to r13
mov [ rsp - 0x60 ], r14; spilling calSv-r14 to mem
mov r14, rdx; spill x23_1 to r14
mov rax, [rsi + 0x0]; arg1[0] -> rax
mul r12; x19_1:x19_0 <- arg1[0] * x3
mov r12, rax; spill x19_0 to r12
mov [ rsp - 0x58 ], r15; spilling calSv-r15 to mem
mov r15, rdx; spill x19_1 to r15
mov rax, [rsi + 0x0]; arg1[0] -> rax
mul rcx; x22_1:x22_0 <- arg1[0] * x8
mov rcx, rax; spill x22_0 to rcx
mov [ rsp - 0x200 ], rdx; spill x22_1 to stack
mov rax, [rsi + 0x10]; arg1[2] -> rax
mul r9; x13_1:x13_0 <- arg1[2] * x5
imul r9, [ rsi + 0x18 ], 0x13; x4 <- arg1[3] * 0x13
mov [ rsp - 0x50 ], rdi; spilling out1 to mem
mov rdi, [ rsi + 0x10 ]; load m64 arg1[2] to register64
mov [ rsp - 0x48 ], r15; spilling x19_1 to mem
mov r15, rdi; load m64 x7 to register64
shl r15, 0x1; x7 <- arg1[2] * 0x2
mov rdi, rax; spill x13_0 to rdi
mov [ rsp - 0x208 ], rdx; spill x13_1 to stack
mov rax, [rsi + 0x8]; arg1[1] -> rax
mul r15; x17_1:x17_0 <- arg1[1] * x7
mov [ rsp - 0x40 ], r12; spilling x19_0 to mem
imul r12, [ rsi + 0x20 ], 0x13; x1 <- arg1[4] * 0x13
mov [ rsp - 0x38 ], r14; spilling x23_1 to mem
mov r14, [ rsi + 0x18 ]; load m64 arg1[3] to register64
mov [ rsp - 0x30 ], r13; spilling x23_0 to mem
mov r13, r14; load m64 x6 to register64
shl r13, 0x1; x6 <- arg1[3] * 0x2
mov r14, rax; spill x17_0 to r14
mov [ rsp - 0x210 ], rdx; spill x17_1 to stack
mov rax, [rsi + 0x20]; arg1[4] -> rax
mul r12; x9_1:x9_0 <- arg1[4] * x1
add rax, r14; could be done better, if r0 has been u8 as well
adc rdx, [ rsp - 0x210 ]
mov r14, rax; spill x10002_0 to r14
mov r12, rdx; spill x10002_1 to r12
mov rax, [rsi + 0x8]; arg1[1] -> rax
mul r13; x16_1:x16_0 <- arg1[1] * x6
mov [ rsp - 0x218 ], rax; spill x16_0 to stack
mov [ rsp - 0x220 ], rdx; spill x16_1 to stack
mov rax, [rsi + 0x10]; arg1[2] -> rax
mul r10; x12_1:x12_0 <- arg1[2] * x2
add r11, [ rsp - 0x218 ]; could be done better, if r0 has been u8 as well
adc r8, [ rsp - 0x220 ]
mov [ rsp - 0x228 ], rax; spill x12_0 to stack
mov [ rsp - 0x230 ], rdx; spill x12_1 to stack
mov rax, [rsi + 0x18]; arg1[3] -> rax
mul r9; x11_1:x11_0 <- arg1[3] * x4
mov r9, rax; spill x11_0 to r9
mov [ rsp - 0x238 ], rdx; spill x11_1 to stack
mov rax, [rsi + 0x0]; arg1[0] -> rax
mul r13; x20_1:x20_0 <- arg1[0] * x6
mov r13, rax; spill x20_0 to r13
mov [ rsp - 0x240 ], rdx; spill x20_1 to stack
mov rax, [rsi + 0x18]; arg1[3] -> rax
mul r10; x10_1:x10_0 <- arg1[3] * x2
add rax, rbx; could be done better, if r0 has been u8 as well
adc rbp, rdx
mov rbx, rax; spill x10003_0 to rbx
mov rax, [rsi + 0x8]; arg1[1] -> rax
mul r10; x15_1:x15_0 <- arg1[1] * x2
add r14, r13; could be done better, if r0 has been u8 as well
adc r12, [ rsp - 0x240 ]
add r9, [ rsp - 0x228 ]; could be done better, if r0 has been u8 as well
mov r10, [ rsp - 0x230 ]; load m64 x12_1 to register64
adc r10, [ rsp - 0x238 ]
add rdi, rax; could be done better, if r0 has been u8 as well
adc rdx, [ rsp - 0x208 ]
add r9, rcx; could be done better, if r0 has been u8 as well
adc r10, [ rsp - 0x200 ]
add rdi, [ rsp - 0x30 ]; could be done better, if r0 has been u8 as well
adc rdx, [ rsp - 0x38 ]
mov rcx, rdx; spill x24_1 to rcx
mov rax, [rsi + 0x0]; arg1[0] -> rax
mul r15; x21_1:x21_0 <- arg1[0] * x7
mov r15, 2251799813685247 ; moving imm to reg
mov r13, rdi;
and r13, r15; keep low 0x33 bits
shrd rdi, rcx, 51; x25 <- x24_1||x24_0 >> 51
add r9, rdi; could be done better, if r0 has been u8 as well
adc r10, 0x0; add CF to r0's alloc
add rbx, rax; could be done better, if r0 has been u8 as well
adc rdx, rbp
mov rbp, r9;
shrd rbp, r10, 51; x32 <- x31_1||x31_0 >> 51
add rbx, rbp; could be done better, if r0 has been u8 as well
adc rdx, 0x0; add CF to r0's alloc
add r11, [ rsp - 0x40 ]; could be done better, if r0 has been u8 as well
adc r8, [ rsp - 0x48 ]
mov rcx, rbx;
shrd rcx, rdx, 51; x35 <- x34_1||x34_0 >> 51
add r14, rcx; could be done better, if r0 has been u8 as well
adc r12, 0x0; add CF to r0's alloc
mov rax, r14;
shrd rax, r12, 51; x38 <- x37_1||x37_0 >> 51
mov rdi, 0x7ffffffffffff ; moving imm to reg
and r14, rdi; x39 <- x37_0&0x7ffffffffffff
add r11, rax; could be done better, if r0 has been u8 as well
adc r8, 0x0; add CF to r0's alloc
mov r10, [ rsp - 0x50 ]; load m64 out1 to register64
mov [ r10 + 0x18 ], r14; out1[3] = x39
mov rbp, r11;
and rbp, rdi; x42 <- x40_0&0x7ffffffffffff
shrd r11, r8, 51; x41 <- x40_1||x40_0 >> 51
imul rdx, r11, 0x13; x43 <- x41 * 0x13
and r9, rdi; x33 <- x31_0&0x7ffffffffffff
lea r13, [ r13 + rdx ]
mov [ r10 + 0x20 ], rbp; out1[4] = x42
mov rcx, r13;
shr rcx, 51; x45 <- x44>> 51
lea rcx, [ rcx + r9 ]
mov r12, rcx;
shr r12, 51; x48 <- x47>> 51
and rbx, rdi; x36 <- x34_0&0x7ffffffffffff
and rcx, rdi; x49 <- x47&0x7ffffffffffff
mov [ r10 + 0x8 ], rcx; out1[1] = x49
and r13, rdi; x46 <- x44&0x7ffffffffffff
lea r12, [ r12 + rbx ]
mov [ r10 + 0x0 ], r13; out1[0] = x46
mov [ r10 + 0x10 ], r12; out1[2] = x50
mov rbx, [ rsp - 0x80 ]; pop
mov rbp, [ rsp - 0x78 ]; pop
mov r12, [ rsp - 0x70 ]; pop
mov r13, [ rsp - 0x68 ]; pop
mov r14, [ rsp - 0x60 ]; pop
mov r15, [ rsp - 0x58 ]; pop
ret
; cpu Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
; ratio 1.0479
; seed 3884720830881002 
; CC / CFLAGS gcc / -march=native -mtune=native -O3 
; cyclegoal; 10000
; using counter; PMC
; framePointer omit
; memoryConstraints none
; time needed: 5110 ms on 200 evaluations.
; Time spent for assembling and measuring (initial batch_size=51, initial num_batches=31): 361 ms
; number of used evaluations: 200
; Ratio (time for assembling + measure)/(total runtime for 200 evals): 0.07064579256360078
; number reverted permutation / tried permutation: 50 / 94 =53.191%
; number reverted decision / tried decision: 56 / 105 =53.333%