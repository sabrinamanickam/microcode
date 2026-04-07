SECTION .text
	GLOBAL fiat_curve25519_carry_square
fiat_curve25519_carry_square:
imul rax, [ rsi + 0x20 ], 0x26; x2 <- arg1[4] * 0x26
mov r10, rax; spill x2 to r10
mov r11, r10; spill x2 to r11
mov [ rsp - 0x80 ], rbx; spilling calSv-rbx to mem
mov rbx, r11; spill x2 to rbx
mov [ rsp - 0x78 ], rbp; spilling calSv-rbp to mem
mov rbp, r14; spill calSv-r14 to rbp
mov rcx, [rsi + 0x10]; mac3 src_a = arg1[2]
mov rdx, rbx; mac3 src_b = x2
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[2] * x2 (+ 0 + 0)
imul r10, [ rsi + 0x18 ], 0x13; x4 <- arg1[3] * 0x13
mov r11, rax; spill x12_0 to r11
mov r9, r8; spill x12_1 to r9
mov r14, r9; spill x12_1 to r14
mov [ rsp - 0x70 ], r12; spilling calSv-r12 to mem
mov r12, r10; spill x4 to r12
mov [ rsp - 0x68 ], r13; spilling calSv-r13 to mem
mov r13, r11; spill x12_0 to r13
mov [ rsp - 0x60 ], rbp; spilling calSv-r14 to mem
mov rbp, r14; spill x12_1 to rbp
mov rcx, [rsi + 0x8]; mac3 src_a = arg1[1]
mov rdx, rbx; mac3 src_b = x2
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[1] * x2 (+ 0 + 0)
mov r10, rax; spill x15_0 to r10
mov r11, r8; spill x15_1 to r11
mov r14, r10; spill x15_0 to r14
mov [ rsp - 0x58 ], r15; spilling calSv-r15 to mem
mov r15, r11; spill x15_1 to r15
mov [ rsp - 0x200 ], r14; spill x15_0 to stack
mov rcx, [rsi + 0x18]; mac3 src_a = arg1[3]
mov rdx, rbx; mac3 src_b = x2
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[3] * x2 (+ 0 + 0)
mov r10, [ rsi + 0x18 ]; load m64 arg1[3] to register64
mov r11, r10; load m64 x6 to register64
shl r11, 0x1; x6 <- arg1[3] * 0x2
mov r10, rax; spill x10_0 to r10
mov r9, r8; spill x10_1 to r9
mov r14, r9; spill x10_1 to r14
mov rbx, r10; spill x10_0 to rbx
mov [ rsp - 0x208 ], r11; spill x6 to stack
mov [ rsp - 0x210 ], r14; spill x10_1 to stack
mov rcx, [rsi + 0x0]; mac3 src_a = arg1[0]
mov rdx, [ rsp - 0x208 ]; mac3 src_b = x6
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[0] * x6 (+ 0 + 0)
mov r10, rax; spill x20_0 to r10
mov r11, r8; spill x20_1 to r11
mov r14, r10; spill x20_0 to r14
mov [ rsp - 0x218 ], r11; spill x20_1 to stack
mov [ rsp - 0x220 ], r14; spill x20_0 to stack
mov rcx, [rsi + 0x0]; mac3 src_a = arg1[0]
mov rdx, [rsi + 0x0]; mac3 src_b = arg1[0]
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[0] * arg1[0] (+ 0 + 0)
mov r10, rax; spill x23_0 to r10
mov r11, r8; spill x23_1 to r11
mov r14, r10; spill x23_0 to r14
mov [ rsp - 0x228 ], r11; spill x23_1 to stack
mov [ rsp - 0x230 ], r14; spill x23_0 to stack
mov rcx, [rsi + 0x10]; mac3 src_a = arg1[2]
mov rdx, [rsi + 0x10]; mac3 src_b = arg1[2]
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[2] * arg1[2] (+ 0 + 0)
mov r10, rax; spill x14_0 to r10
mov r11, r8; spill x14_1 to r11
mov r14, r10; spill x14_0 to r14
mov [ rsp - 0x238 ], r11; spill x14_1 to stack
mov [ rsp - 0x240 ], r14; spill x14_0 to stack
mov rcx, [rsi + 0x8]; mac3 src_a = arg1[1]
mov rdx, [rsi + 0x8]; mac3 src_b = arg1[1]
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[1] * arg1[1] (+ 0 + 0)
mov r10, [ rsi + 0x8 ]; load m64 arg1[1] to register64
lea r11, [r10 + r10]; x8 <- arg1[1] * 2 
imul r10, [ rsi + 0x20 ], 0x13; x1 <- arg1[4] * 0x13
add rbx, rax; could be done better, if r0 has been u8 as well
adc r8, [ rsp - 0x210 ]
mov r9, r8; spill x10003_1 to r9
mov r14, r9; spill x10003_1 to r14
mov [ rsp - 0x248 ], r11; spill x8 to stack
mov [ rsp - 0x250 ], r14; spill x10003_1 to stack
mov rcx, [rsi + 0x20]; mac3 src_a = arg1[4]
mov rdx, r10; mac3 src_b = x1
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[4] * x1 (+ 0 + 0)
imul r10, [ rsi + 0x18 ], 0x26; x5 <- arg1[3] * 0x26
mov r11, rax; spill x9_0 to r11
mov r9, r8; spill x9_1 to r9
mov r14, r9; spill x9_1 to r14
mov [ rsp - 0x258 ], r11; spill x9_0 to stack
mov [ rsp - 0x260 ], r14; spill x9_1 to stack
mov rcx, [rsi + 0x10]; mac3 src_a = arg1[2]
mov rdx, r10; mac3 src_b = x5
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[2] * x5 (+ 0 + 0)
add rax, [ rsp - 0x200 ]; could be done better, if r0 has been u8 as well
adc r15, r8
add rax, [ rsp - 0x230 ]; could be done better, if r0 has been u8 as well
adc r15, [ rsp - 0x228 ]
mov r10, rax;
shrd r10, r15, 51; x25 <- x24_1||x24_0 >> 51
mov r11, rax; spill x24_0 to r11
mov r14, r10; spill x25 to r14
mov r15, r11; spill x24_0 to r15
mov [ rsp - 0x268 ], r14; spill x25 to stack
mov rcx, [rsi + 0x18]; mac3 src_a = arg1[3]
mov rdx, r12; mac3 src_b = x4
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[3] * x4 (+ 0 + 0)
add rax, r13; could be done better, if r0 has been u8 as well
adc rbp, r8
imul r10, [ rsi + 0x10 ], 0x2; x7 <- arg1[2] * 0x2
mov r11, rax; spill x10004_0 to r11
mov r14, r10; spill x7 to r14
mov r13, r11; spill x10004_0 to r13
mov r12, r14; spill x7 to r12
mov rcx, [rsi + 0x0]; mac3 src_a = arg1[0]
mov rdx, r12; mac3 src_b = x7
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[0] * x7 (+ 0 + 0)
add rbx, rax; could be done better, if r0 has been u8 as well
adc r8, [ rsp - 0x250 ]
mov r10, r8; spill x29_1 to r10
mov r11, r10; spill x29_1 to r11
mov r14, r11; spill x29_1 to r14
mov [ rsp - 0x270 ], r14; spill x29_1 to stack
mov rcx, [rsi + 0x0]; mac3 src_a = arg1[0]
mov rdx, [ rsp - 0x248 ]; mac3 src_b = x8
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[0] * x8 (+ 0 + 0)
add r13, rax; could be done better, if r0 has been u8 as well
adc r8, rbp
add r13, [ rsp - 0x268 ]; could be done better, if r0 has been u8 as well
adc r8, 0x0; add CF to r0's alloc
mov r10, r13;
shrd r10, r8, 51; x32 <- x31_1||x31_0 >> 51
mov r11, 2251799813685247 ; moving imm to reg
and r13, r11; keep low 0x33 bits
add rbx, r10; could be done better, if r0 has been u8 as well
mov rdx, [ rsp - 0x270 ];
adc rdx, 0x0; add CF to r0's alloc
mov rcx, rbx;
shrd rcx, rdx, 51; x35 <- x34_1||x34_0 >> 51
mov r9, rcx; spill x35 to r9
mov r14, r9; spill x35 to r14
mov rbp, r14; spill x35 to rbp
mov rcx, [rsi + 0x8]; mac3 src_a = arg1[1]
mov rdx, r12; mac3 src_b = x7
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[1] * x7 (+ 0 + 0)
mov r10, rax;
add r10, [ rsp - 0x258 ]; could be done better, if r0 has been u8 as well
adc r8, [ rsp - 0x260 ]
mov r11, r8; spill x10002_1 to r11
mov r14, r10; spill x10002_0 to r14
mov r12, r11; spill x10002_1 to r12
mov [ rsp - 0x278 ], r14; spill x10002_0 to stack
mov rcx, [rsi + 0x8]; mac3 src_a = arg1[1]
mov rdx, [ rsp - 0x208 ]; mac3 src_b = x6
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[1] * x6 (+ 0 + 0)
mov r10, [ rsp - 0x278 ]; load m64 x10002_0 to register64
add r10, [ rsp - 0x220 ]; could be done better, if r0 has been u8 as well
adc r12, [ rsp - 0x218 ]
add r10, rbp; could be done better, if r0 has been u8 as well
adc r12, 0x0; add CF to r0's alloc
mov r11, 0x7ffffffffffff ; moving imm to reg
mov rdx, r10;
and rdx, r11; x39 <- x37_0&0x7ffffffffffff
and rbx, r11; x36 <- x34_0&0x7ffffffffffff
mov [ rdi + 0x18 ], rdx; out1[3] = x39
imul rcx, [ rsi + 0x20 ], 0x2; x3 <- arg1[4] * 0x2
mov r9, rax; spill x16_0 to r9
mov r14, r8; spill x16_1 to r14
mov rbp, r9; spill x16_0 to rbp
mov [ rsp - 0x278 ], r10; spill x37_0 to stack
mov [ rsp - 0x280 ], r11; spill 0x7ffffffffffff to stack
mov [ rsp - 0x288 ], r14; spill x16_1 to stack
mov rdx, rcx; mac3 src_b = x3
mov rcx, [rsi + 0x0]; mac3 src_a = arg1[0]
xor r9, r9; mac3 zero pair 2a
xor r10, r10; mac3 zero pair 2b
xor r11, r11; mac3 zero pair 3a
xor r14, r14; mac3 zero pair 3b
xor rax, rax; mac3: zero acc_lo
xor r8, r8; zero acc_hi
vmwrite rcx, rdx; MAC3: RAX:R8 += arg1[0] * x3 (+ 0 + 0)
mov r10, rbp;
add r10, [ rsp - 0x240 ]; could be done better, if r0 has been u8 as well
mov r11, [ rsp - 0x238 ]; load m64 x14_1 to register64
adc r11, [ rsp - 0x288 ]
mov rdx, [ rsp - 0x278 ]; load m64 x37_0 to register64
shrd rdx, r12, 51; x38 <- x37_1||x37_0 >> 51
add r10, rax; could be done better, if r0 has been u8 as well
adc r8, r11
add r10, rdx; could be done better, if r0 has been u8 as well
adc r8, 0x0; add CF to r0's alloc
mov rcx, r10;
and rcx, [ rsp - 0x280 ]; x42 <- x40_0&0x7ffffffffffff
shrd r10, r8, 51; x41 <- x40_1||x40_0 >> 51
lea r9, [r10 + 8 * r10]; TMP <- x41 * 9 
lea r14, [r10 + 2 * r9]; x43 <- x41*19: x41 + 2 * TMP = x41 + 2 * 9 * x41
and r15, [ rsp - 0x280 ]; x26 <- x24_0&0x7ffffffffffff
lea r15, [ r15 + r14 ]
mov r9, r15;
shr r9, 51; x45 <- x44>> 51
lea r9, [ r9 + r13 ]
mov r13, r9;
shr r13, 51; x48 <- x47>> 51
lea r13, [ r13 + rbx ]
and r9, [ rsp - 0x280 ]; x49 <- x47&0x7ffffffffffff
and r15, [ rsp - 0x280 ]; x46 <- x44&0x7ffffffffffff
mov [ rdi + 0x8 ], r9; out1[1] = x49
mov [ rdi + 0x0 ], r15; out1[0] = x46
mov [ rdi + 0x10 ], r13; out1[2] = x50
mov [ rdi + 0x20 ], rcx; out1[4] = x42
mov rbx, [ rsp - 0x80 ]; pop
mov rbp, [ rsp - 0x78 ]; pop
mov r12, [ rsp - 0x70 ]; pop
mov r13, [ rsp - 0x68 ]; pop
mov r14, [ rsp - 0x60 ]; pop
mov r15, [ rsp - 0x58 ]; pop
ret
; cpu Intel(R) Celeron(R) CPU N3350 @ 1.10GHz
; ratio 0.3298
; seed 3384081396412672 
; CC / CFLAGS gcc / -march=native -mtune=native -O3 
; cyclegoal; 10000
; using counter; RDTSCP
; framePointer omit
; memoryConstraints none
; time needed: 269373 ms on 8000 evaluations.
; Time spent for assembling and measuring (initial batch_size=158, initial num_batches=31): 29674 ms
; number of used evaluations: 8000
; Ratio (time for assembling + measure)/(total runtime for 8000 evals): 0.11015951858575285
; number reverted permutation / tried permutation: 2436 / 3967 =61.407%
; number reverted decision / tried decision: 2531 / 4032 =62.773%
