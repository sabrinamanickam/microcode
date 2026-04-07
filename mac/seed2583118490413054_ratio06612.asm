SECTION .text
	GLOBAL fiat_curve25519_carry_square
fiat_curve25519_carry_square:
imul rax, [ rsi + 0x18 ], 0x26; x5 <- arg1[3] * 0x26
imul r10, [ rsi + 0x10 ], 0x2; x7 <- arg1[2] * 0x2
imul r11, [ rsi + 0x18 ], 0x2; x6 <- arg1[3] * 0x2
mov rdx, [ rsi + 0x20 ]; load m64 arg1[4] to register64
lea rcx, [rdx + 8 * rdx]; TMP <- arg1[4] * 9 
lea r8, [rdx + 2 * rcx]; x1 <- arg1[4]*19: arg1[4] + 2 * TMP = arg1[4] + 2 * 9 * arg1[4]
imul rdx, [ rsi + 0x20 ], 0x2; x3 <- arg1[4] * 0x2
mov [ rsp - 0x210 ], r11; mac3 batch: pin x6 to stack
imul r11, [ rsi + 0x20 ], 0x26; x2 <- arg1[4] * 0x26
mov [ rsp - 0x248 ], r8; mac3 batch: pin x1 to stack
mov [ rsp - 0x270 ], rax; mac3 batch: pin x5 to stack
mov [ rsp - 0x288 ], r11; mac3 batch: pin x2 to stack
mov r11, rdx; spill x3 to r11
mov [ rsp - 0x80 ], rbx; spilling calSv-rbx to mem
mov rbx, r10; spill x7 to rbx
mov [ rsp - 0x78 ], rbp; spilling calSv-rbp to mem
mov rbp, r11; spill x3 to rbp
mov [ rsp - 0x70 ], r12; spilling calSv-r12 to mem
mov r12, r14; spill calSv-r14 to r12
mov rcx, [rsi + 0x0]; mac3 pair1_a = arg1[0]
mov rdx, [rsi + 0x0]; mac3 pair1_b = arg1[0]
mov r9, [rsi + 0x10]; mac3 pair2_a = arg1[2]
mov r10, [ rsp - 0x270 ]; mac3 pair2_b = x5
mov r11, [rsi + 0x8]; mac3 pair3_a = arg1[1]
mov r14, [ rsp - 0x288 ]; mac3 pair3_b = x2
xor rax, rax; mac3: zero acc_lo
xor r8, r8; mac3: zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[0]*arg1[0] + arg1[2]*x5 + arg1[1]*x2
imul r10, [ rsi + 0x18 ], 0x13; x4 <- arg1[3] * 0x13
mov [ rsp - 0x2b0 ], r10; mac3 batch: pin x4 to stack
mov r10, rax;
shrd r10, r8, 51; x25 <- x24_1||x24_0 >> 51
mov [ rsp - 0x2c8 ], rbp; mac3 batch: pin x3 to stack
mov r11, rax; spill x24_0 to r11
mov rbp, r10; spill x25 to rbp
mov r14, r11; spill x24_0 to r14
mov [ rsp - 0x68 ], r13; spilling calSv-r13 to mem
mov r13, r14; spill x24_0 to r13
mov rcx, [rsi + 0x10]; mac3 pair1_a = arg1[2]
mov rdx, [rsi + 0x10]; mac3 pair1_b = arg1[2]
mov r9, [rsi + 0x8]; mac3 pair2_a = arg1[1]
mov r10, [ rsp - 0x210 ]; mac3 pair2_b = x6
mov r11, [rsi + 0x0]; mac3 pair3_a = arg1[0]
mov r14, [ rsp - 0x2c8 ]; mac3 pair3_b = x3
xor rax, rax; mac3: zero acc_lo
xor r8, r8; mac3: zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[2]*arg1[2] + arg1[1]*x6 + arg1[0]*x3
mov r10, [ rsi + 0x8 ]; load m64 arg1[1] to register64
mov r11, r10; load m64 x8 to register64
shl r11, 0x1; x8 <- arg1[1] * 0x2
mov [ rsp - 0x2f0 ], rbx; mac3 batch: pin x7 to stack
mov r10, rax; spill x27_0 to r10
mov r9, r8; spill x27_1 to r9
mov rbx, r9; spill x27_1 to rbx
mov r14, r10; spill x27_0 to r14
mov [ rsp - 0x60 ], r12; spilling calSv-r14 to mem
mov r12, r11; spill x8 to r12
mov [ rsp - 0x58 ], r15; spilling calSv-r15 to mem
mov r15, r14; spill x27_0 to r15
mov rcx, [rsi + 0x8]; mac3 pair1_a = arg1[1]
mov rdx, [rsi + 0x8]; mac3 pair1_b = arg1[1]
mov r9, [rsi + 0x18]; mac3 pair2_a = arg1[3]
mov r10, [ rsp - 0x288 ]; mac3 pair2_b = x2
mov r11, [rsi + 0x0]; mac3 pair3_a = arg1[0]
mov r14, [ rsp - 0x2f0 ]; mac3 pair3_b = x7
xor rax, rax; mac3: zero acc_lo
xor r8, r8; mac3: zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[1]*arg1[1] + arg1[3]*x2 + arg1[0]*x7
mov [ rsp - 0x348 ], r12; mac3 batch: pin x8 to stack
mov r10, rax; spill x29_0 to r10
mov r11, r8; spill x29_1 to r11
mov r12, r10; spill x29_0 to r12
mov r14, r11; spill x29_1 to r14
mov [ rsp - 0x360 ], r14; spill x29_1 to stack
mov rcx, [rsi + 0x10]; mac3 pair1_a = arg1[2]
mov rdx, [ rsp - 0x288 ]; mac3 pair1_b = x2
mov r9, [rsi + 0x18]; mac3 pair2_a = arg1[3]
mov r10, [ rsp - 0x2b0 ]; mac3 pair2_b = x4
mov r11, [rsi + 0x0]; mac3 pair3_a = arg1[0]
mov r14, [ rsp - 0x348 ]; mac3 pair3_b = x8
xor rax, rax; mac3: zero acc_lo
xor r8, r8; mac3: zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[2]*x2 + arg1[3]*x4 + arg1[0]*x8
mov r10, rax; spill x30_0 to r10
mov r11, r8; spill x30_1 to r11
mov r14, r10; spill x30_0 to r14
mov [ rsp - 0x378 ], r11; spill x30_1 to stack
mov [ rsp - 0x380 ], r14; spill x30_0 to stack
mov rcx, [rsi + 0x0]; mac3 pair1_a = arg1[0]
mov rdx, [ rsp - 0x210 ]; mac3 pair1_b = x6
mov r9, [rsi + 0x20]; mac3 pair2_a = arg1[4]
mov r10, [ rsp - 0x248 ]; mac3 pair2_b = x1
mov r11, [rsi + 0x8]; mac3 pair3_a = arg1[1]
mov r14, [ rsp - 0x2f0 ]; mac3 pair3_b = x7
xor rax, rax; mac3: zero acc_lo
xor r8, r8; mac3: zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[0]*x6 + arg1[4]*x1 + arg1[1]*x7
mov r10, rbp;
add r10, [ rsp - 0x380 ]; could be done better, if r0 has been u8 as well
mov r11, [ rsp - 0x378 ];
adc r11, 0x0; add CF to r0's alloc
mov rdx, r10;
shrd rdx, r11, 51; x32 <- x31_1||x31_0 >> 51
add r12, rdx; could be done better, if r0 has been u8 as well
mov rcx, [ rsp - 0x360 ];
adc rcx, 0x0; add CF to r0's alloc
mov r9, 2251799813685247 ; moving imm to reg
mov r14, r12;
and r14, r9; keep low 0x33 bits
shrd r12, rcx, 51; x35 <- x34_1||x34_0 >> 51
add rax, r12; could be done better, if r0 has been u8 as well
adc r8, 0x0; add CF to r0's alloc
mov rbp, rax;
shrd rbp, r8, 51; x38 <- x37_1||x37_0 >> 51
and rax, r9; keep low 0x33 bits
add r15, rbp; could be done better, if r0 has been u8 as well
adc rbx, 0x0; add CF to r0's alloc
mov [ rdi + 0x18 ], rax; out1[3] = x39
mov r11, r15;
and r11, r9; keep low 0x33 bits
mov [ rdi + 0x20 ], r11; out1[4] = x42
shrd r15, rbx, 51; x41 <- x40_1||x40_0 >> 51
mov rdx, 0x7ffffffffffff ; moving imm to reg
and r13, rdx; x26 <- x24_0&0x7ffffffffffff
imul rcx, r15, 0x13; x43 <- x41 * 0x13
lea r13, [ r13 + rcx ]
mov r12, r13;
and r12, rdx; x46 <- x44&0x7ffffffffffff
and r10, rdx; x33 <- x31_0&0x7ffffffffffff
shr r13, 51; x45 <- x44>> 51
lea r13, [ r13 + r10 ]
mov r8, r13;
and r8, rdx; x49 <- x47&0x7ffffffffffff
mov [ rdi + 0x0 ], r12; out1[0] = x46
shr r13, 51; x48 <- x47>> 51
lea r13, [ r13 + r14 ]
mov [ rdi + 0x10 ], r13; out1[2] = x50
mov [ rdi + 0x8 ], r8; out1[1] = x49
mov rbx, [ rsp - 0x80 ]; pop
mov rbp, [ rsp - 0x78 ]; pop
mov r12, [ rsp - 0x70 ]; pop
mov r13, [ rsp - 0x68 ]; pop
mov r14, [ rsp - 0x60 ]; pop
mov r15, [ rsp - 0x58 ]; pop
ret
; cpu Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
; ratio 0.6612
; seed 2583118490413054 
; CC / CFLAGS gcc / -march=native -mtune=native -O3 
; cyclegoal; 10000
; using counter; RDTSCP
; framePointer omit
; memoryConstraints none
; time needed: 6244 ms on 200 evaluations.
; Time spent for assembling and measuring (initial batch_size=157, initial num_batches=31): 486 ms
; number of used evaluations: 200
; Ratio (time for assembling + measure)/(total runtime for 200 evals): 0.07783472133247918
; number reverted permutation / tried permutation: 66 / 106 =62.264%
; number reverted decision / tried decision: 47 / 93 =50.538%