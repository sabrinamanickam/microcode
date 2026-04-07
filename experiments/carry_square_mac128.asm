SECTION .text
	GLOBAL carry_square_mac128
carry_square_mac128:
; ═══════════════════════════════════════════════════════════════════
;  Curve25519 carry_square using MAC128 microcode acceleration
;
;  MAC128 trigger: vmwrite rcx, rdx
;  Effect: R8:RAX += RCX × RDX  (128-bit multiply-accumulate)
;          RCX and RDX are clobbered
;          RAX (acc_lo) and R8 (acc_hi) are updated in-place
;
;  Requires: MAC128 microcode patch installed at ucode entry 0x0cd8
;
;  Calling convention: System V AMD64
;    rdi = out1 (pointer to uint64_t[5])
;    rsi = arg1 (pointer to uint64_t[5])
; ═══════════════════════════════════════════════════════════════════

; ── Save callee-saved registers ──────────────────────────────────
mov [rsp - 0x08], rbx
mov [rsp - 0x10], rbp
mov [rsp - 0x18], r12
mov [rsp - 0x20], r13
mov [rsp - 0x28], r14
mov [rsp - 0x30], r15
mov [rsp - 0x38], rdi              ; save out1 pointer

; ── Precompute scalar multipliers ────────────────────────────────
; These are small constants times limbs, NOT widening multiplies.
; All fit in 64 bits. Stay in registers throughout the MAC phase.
;
;   r10 = x2 = arg1[4] * 38       (2*19)
;   r9  = x5 = arg1[3] * 38       (2*19)
;   r11 = x1 = arg1[4] * 19
;   r12 = x3 = arg1[4] * 2
;   rbx = x4 = arg1[3] * 19
;   r13 = x6 = arg1[3] * 2
;   r14 = x7 = arg1[2] * 2
;   r15 = x8 = arg1[1] * 2
;   rbp = 0x7FFFFFFFFFFFF         (51-bit mask)

imul r10, [rsi + 0x20], 0x26      ; x2 = arg1[4] * 38
imul r9,  [rsi + 0x18], 0x26      ; x5 = arg1[3] * 38
imul r11, [rsi + 0x20], 0x13      ; x1 = arg1[4] * 19
imul r12, [rsi + 0x20], 0x2       ; x3 = arg1[4] * 2
imul rbx, [rsi + 0x18], 0x13      ; x4 = arg1[3] * 19
mov  r13, [rsi + 0x18]
shl  r13, 1                        ; x6 = arg1[3] * 2
mov  r14, [rsi + 0x10]
shl  r14, 1                        ; x7 = arg1[2] * 2
mov  r15, [rsi + 0x08]
shl  r15, 1                        ; x8 = arg1[1] * 2
mov  rbp, 0x7FFFFFFFFFFFF          ; 51-bit mask

; ═══════════════════════════════════════════════════════════════════
;  LIMB 0:  x24 = arg1[0]² + arg1[1]*x2 + arg1[2]*x5
; ═══════════════════════════════════════════════════════════════════
xor  rax, rax                      ; acc_lo = 0
xor  r8, r8                        ; acc_hi = 0

mov  rcx, [rsi + 0x00]             ; arg1[0]
mov  rdx, rcx                      ; arg1[0]  (self-square)
vmwrite rcx, rdx                   ; acc += arg1[0]²

mov  rcx, [rsi + 0x08]             ; arg1[1]
mov  rdx, r10                      ; x2
vmwrite rcx, rdx                   ; acc += arg1[1] * x2

mov  rcx, [rsi + 0x10]             ; arg1[2]
mov  rdx, r9                       ; x5
vmwrite rcx, rdx                   ; acc += arg1[2] * x5

; Extract limb 0 value + carry
mov  rdi, rax
and  rdi, rbp                      ; x26 = low 51 bits
mov  [rsp - 0x40], rdi             ; save x26
shrd rax, r8, 51                   ; carry = full_128 >> 51
xor  r8, r8                        ; reset acc_hi

; ═══════════════════════════════════════════════════════════════════
;  LIMB 1:  x31 = carry + arg1[0]*x8 + arg1[2]*x2 + arg1[3]*x4
; ═══════════════════════════════════════════════════════════════════
; RAX = carry from limb 0, R8 = 0 → accumulator seeded with carry

mov  rcx, [rsi + 0x00]             ; arg1[0]
mov  rdx, r15                      ; x8
vmwrite rcx, rdx                   ; acc += arg1[0] * x8

mov  rcx, [rsi + 0x10]             ; arg1[2]
mov  rdx, r10                      ; x2
vmwrite rcx, rdx                   ; acc += arg1[2] * x2

mov  rcx, [rsi + 0x18]             ; arg1[3]
mov  rdx, rbx                      ; x4
vmwrite rcx, rdx                   ; acc += arg1[3] * x4

; Extract limb 1
mov  rdi, rax
and  rdi, rbp                      ; x33
mov  [rsp - 0x48], rdi
shrd rax, r8, 51
xor  r8, r8

; ═══════════════════════════════════════════════════════════════════
;  LIMB 2:  x34 = carry + arg1[0]*x7 + arg1[1]² + arg1[3]*x2
; ═══════════════════════════════════════════════════════════════════

mov  rcx, [rsi + 0x00]             ; arg1[0]
mov  rdx, r14                      ; x7
vmwrite rcx, rdx                   ; acc += arg1[0] * x7

mov  rcx, [rsi + 0x08]             ; arg1[1]
mov  rdx, rcx                      ; arg1[1]  (self-square)
vmwrite rcx, rdx                   ; acc += arg1[1]²

mov  rcx, [rsi + 0x18]             ; arg1[3]
mov  rdx, r10                      ; x2
vmwrite rcx, rdx                   ; acc += arg1[3] * x2

; Extract limb 2
mov  rdi, rax
and  rdi, rbp                      ; x36
mov  [rsp - 0x50], rdi
shrd rax, r8, 51
xor  r8, r8

; ═══════════════════════════════════════════════════════════════════
;  LIMB 3:  x37 = carry + arg1[0]*x6 + arg1[1]*x7 + arg1[4]*x1
; ═══════════════════════════════════════════════════════════════════

mov  rcx, [rsi + 0x00]             ; arg1[0]
mov  rdx, r13                      ; x6
vmwrite rcx, rdx                   ; acc += arg1[0] * x6

mov  rcx, [rsi + 0x08]             ; arg1[1]
mov  rdx, r14                      ; x7
vmwrite rcx, rdx                   ; acc += arg1[1] * x7

mov  rcx, [rsi + 0x20]             ; arg1[4]
mov  rdx, r11                      ; x1
vmwrite rcx, rdx                   ; acc += arg1[4] * x1

; Extract limb 3
mov  rdi, rax
and  rdi, rbp                      ; x39 → out1[3]
mov  [rsp - 0x58], rdi
shrd rax, r8, 51
xor  r8, r8

; ═══════════════════════════════════════════════════════════════════
;  LIMB 4:  x40 = carry + arg1[0]*x3 + arg1[1]*x6 + arg1[2]²
; ═══════════════════════════════════════════════════════════════════

mov  rcx, [rsi + 0x00]             ; arg1[0]
mov  rdx, r12                      ; x3
vmwrite rcx, rdx                   ; acc += arg1[0] * x3

mov  rcx, [rsi + 0x08]             ; arg1[1]
mov  rdx, r13                      ; x6
vmwrite rcx, rdx                   ; acc += arg1[1] * x6

mov  rcx, [rsi + 0x10]             ; arg1[2]
mov  rdx, rcx                      ; arg1[2]  (self-square)
vmwrite rcx, rdx                   ; acc += arg1[2]²

; Extract limb 4
mov  rdi, rax
and  rdi, rbp                      ; x42 → out1[4]
shrd rax, r8, 51                   ; x41 = final carry

; ═══════════════════════════════════════════════════════════════════
;  FINAL CARRY CHAIN — Reduce mod 2^255-19
; ═══════════════════════════════════════════════════════════════════

; x43 = x41 * 19
imul rax, rax, 0x13

; x44 = x26 + x43
add  rax, [rsp - 0x40]
mov  rcx, rax
shr  rcx, 51                       ; x45
and  rax, rbp                      ; x46 → out1[0]

; x47 = x45 + x33
add  rcx, [rsp - 0x48]
mov  rdx, rcx
shr  rdx, 51                       ; x48
and  rcx, rbp                      ; x49 → out1[1]

; x50 = x48 + x36
add  rdx, [rsp - 0x50]             ; x50 → out1[2]

; ── Store results ────────────────────────────────────────────────
mov  r8, [rsp - 0x38]              ; restore out1 pointer
mov  [r8 + 0x00], rax              ; out1[0] = x46
mov  [r8 + 0x08], rcx              ; out1[1] = x49
mov  [r8 + 0x10], rdx              ; out1[2] = x50
mov  rax, [rsp - 0x58]
mov  [r8 + 0x18], rax              ; out1[3] = x39
mov  [r8 + 0x20], rdi              ; out1[4] = x42

; ── Restore callee-saved registers ───────────────────────────────
mov  rbx, [rsp - 0x08]
mov  rbp, [rsp - 0x10]
mov  r12, [rsp - 0x18]
mov  r13, [rsp - 0x20]
mov  r14, [rsp - 0x28]
mov  r15, [rsp - 0x30]
ret

; ═══════════════════════════════════════════════════════════════════
;  INSTRUCTION COUNT COMPARISON:
;
;  Original CryptOpt (ratio 1.0479):
;    15 widening mul (implicit rax clobber)
;    ~20 stack spill/reload for intermediate products
;    ~15 mov-to-rax (MUL source prep)
;    ~153 total instructions
;
;  MAC128 version:
;    15 vmwrite (MAC128 trigger, 6 triads each)
;    5 stack stores (limb values only)
;    3 stack loads (final carry chain)
;    0 mov-to-rax (source comes from RCX)
;    ~115 total instructions
;
;  Key structural differences:
;    - All 5 limb accumulators computed serially (carry flows through)
;    - No intermediate product spills — each product immediately accumulated
;    - 8 precomputed values live in registers throughout
;    - Carry chain integrated into limb extraction
; ═══════════════════════════════════════════════════════════════════
