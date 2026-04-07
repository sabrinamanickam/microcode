	.file	"test.c"
	.intel_syntax noprefix
	.text
.Ltext0:
	.file 0 "/home/redunlock/code/lib-micro/Sabrina/experiments" "test.c"
	.section	.rodata
	.align 8
.LC0:
	.string	"assign to specific core failed."
	.text
	.globl	install_addshl_vmwrite_patch
	.type	install_addshl_vmwrite_patch, @function
install_addshl_vmwrite_patch:
.LFB11:
	.file 1 "test.c"
	.loc 1 8 41
	.cfi_startproc
	endbr64
	push	rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	mov	rbp, rsp
	.cfi_def_cfa_register 6
	sub	rsp, 208
	.loc 1 8 41
	mov	rax, QWORD PTR fs:40
	mov	QWORD PTR -8[rbp], rax
	xor	eax, eax
	.loc 1 9 13
	movabs	rax, 313532743682
	mov	QWORD PTR -176[rbp], rax
	mov	QWORD PTR -168[rbp], 0
	mov	QWORD PTR -160[rbp], 0
	mov	QWORD PTR -152[rbp], 50331890
	mov	DWORD PTR -204[rbp], 0
.LBB10:
.LBB11:
	.file 2 "../../include/misc.h"
	.loc 2 52 5
	lea	rax, -144[rbp]
	mov	rsi, rax
	mov	eax, 0
	mov	edx, 16
	mov	rdi, rsi
	mov	rcx, rdx
	rep stosq
.LBB12:
	.loc 2 53 5
	mov	eax, DWORD PTR -204[rbp]
	cdqe
	mov	QWORD PTR -192[rbp], rax
	cmp	QWORD PTR -192[rbp], 1023
	ja	.L3
	mov	rax, QWORD PTR -192[rbp]
	shr	rax, 6
	lea	rdx, 0[0+rax*8]
	lea	rcx, -144[rbp]
	add	rdx, rcx
	mov	rsi, QWORD PTR [rdx]
	mov	rdx, QWORD PTR -192[rbp]
	and	edx, 63
	mov	edi, 1
	mov	ecx, edx
	sal	rdi, cl
	mov	rdx, rdi
	lea	rcx, 0[0+rax*8]
	lea	rax, -144[rbp]
	add	rax, rcx
	or	rdx, rsi
	mov	QWORD PTR [rax], rdx
.L3:
.LBE12:
	.loc 2 54 9
	call	getpid@PLT
	mov	ecx, eax
	lea	rax, -144[rbp]
	mov	rdx, rax
	mov	esi, 128
	mov	edi, ecx
	call	sched_setaffinity@PLT
	.loc 2 54 8
	test	eax, eax
	je	.L7
	mov	DWORD PTR -200[rbp], 1
	mov	DWORD PTR -196[rbp], -1
	lea	rax, .LC0[rip]
	mov	QWORD PTR -184[rbp], rax
.LBB13:
.LBB14:
	.file 3 "/usr/include/x86_64-linux-gnu/bits/error.h"
	.loc 3 42 5
	mov	rdx, QWORD PTR -184[rbp]
	mov	ecx, DWORD PTR -196[rbp]
	mov	eax, DWORD PTR -200[rbp]
	mov	esi, ecx
	mov	edi, eax
	mov	eax, 0
	call	error@PLT
	.loc 3 43 1
	nop
.L7:
.LBE14:
.LBE13:
	.loc 2 57 1
	nop
.LBE11:
.LBE10:
	.loc 1 22 5
	mov	eax, 0
	call	do_fix_IN_patch@PLT
	.loc 1 25 5
	lea	rax, -176[rbp]
	mov	edx, 1
	mov	rsi, rax
	mov	edi, 31820
	call	patch_ucode@PLT
	.loc 1 28 5
	mov	edx, 31820
	mov	esi, 3288
	mov	edi, 0
	call	hook_match_and_patch@PLT
	.loc 1 29 1
	nop
	mov	rax, QWORD PTR -8[rbp]
	sub	rax, QWORD PTR fs:40
	je	.L5
	call	__stack_chk_fail@PLT
.L5:
	leave
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
.LFE11:
	.size	install_addshl_vmwrite_patch, .-install_addshl_vmwrite_patch
	.section	.rodata
.LC1:
	.string	"After patch:"
.LC2:
	.string	"RAX after patch = 0x%016lx\n"
.LC3:
	.string	"RDX after patch = 0x%016lx\n"
.LC4:
	.string	"RBX after patch = 0x%016lx\n"
	.text
	.globl	main
	.type	main, @function
main:
.LFB12:
	.loc 1 30 16
	.cfi_startproc
	endbr64
	push	rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	mov	rbp, rsp
	.cfi_def_cfa_register 6
	push	rbx
	sub	rsp, 40
	.cfi_offset 3, -24
	.loc 1 31 14
	mov	QWORD PTR -40[rbp], 1
	.loc 1 32 14
	mov	QWORD PTR -32[rbp], 2
	.loc 1 33 14
	mov	QWORD PTR -24[rbp], 3
	.loc 1 35 5
	lea	rax, .LC1[rip]
	mov	rdi, rax
	call	puts@PLT
	.loc 1 36 5
	call	install_addshl_vmwrite_patch
	.loc 1 39 5
#APP
# 39 "test.c" 1
	movq %rax, $1; movq %rbx, $2; movq %rdx, $3
vmwrite %rax, %rdx
	
# 0 "" 2
#NO_APP
	mov	rcx, rbx
	mov	QWORD PTR -40[rbp], rax
	mov	QWORD PTR -24[rbp], rcx
	mov	QWORD PTR -32[rbp], rdx
	.loc 1 46 5
	mov	rax, QWORD PTR -40[rbp]
	mov	rsi, rax
	lea	rax, .LC2[rip]
	mov	rdi, rax
	mov	eax, 0
	call	printf@PLT
	.loc 1 47 5
	mov	rax, QWORD PTR -32[rbp]
	mov	rsi, rax
	lea	rax, .LC3[rip]
	mov	rdi, rax
	mov	eax, 0
	call	printf@PLT
	.loc 1 48 5
	mov	rax, QWORD PTR -24[rbp]
	mov	rsi, rax
	lea	rax, .LC4[rip]
	mov	rdi, rax
	mov	eax, 0
	call	printf@PLT
	.loc 1 49 12
	mov	eax, 0
	.loc 1 50 1
	mov	rbx, QWORD PTR -8[rbp]
	leave
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
.LFE12:
	.size	main, .-main
.Letext0:
	.file 4 "/usr/lib/gcc/x86_64-linux-gnu/11/include/stddef.h"
	.file 5 "/usr/include/x86_64-linux-gnu/bits/types.h"
	.file 6 "/usr/include/x86_64-linux-gnu/bits/stdint-uintn.h"
	.file 7 "/usr/include/x86_64-linux-gnu/bits/cpu-set.h"
	.file 8 "/usr/include/stdio.h"
	.file 9 "/usr/include/sched.h"
	.file 10 "../../include/patch.h"
	.file 11 "/usr/include/unistd.h"
	.section	.debug_info,"",@progbits
.Ldebug_info0:
	.long	0x376
	.value	0x5
	.byte	0x1
	.byte	0x8
	.long	.Ldebug_abbrev0
	.uleb128 0x12
	.long	.LASF40
	.byte	0x1d
	.long	.LASF0
	.long	.LASF1
	.quad	.Ltext0
	.quad	.Letext0-.Ltext0
	.long	.Ldebug_line0
	.uleb128 0x3
	.long	.LASF9
	.byte	0x4
	.byte	0xd1
	.byte	0x17
	.long	0x3a
	.uleb128 0x2
	.byte	0x8
	.byte	0x7
	.long	.LASF2
	.uleb128 0x2
	.byte	0x4
	.byte	0x7
	.long	.LASF3
	.uleb128 0x2
	.byte	0x1
	.byte	0x8
	.long	.LASF4
	.uleb128 0x2
	.byte	0x2
	.byte	0x7
	.long	.LASF5
	.uleb128 0x2
	.byte	0x1
	.byte	0x6
	.long	.LASF6
	.uleb128 0x2
	.byte	0x2
	.byte	0x5
	.long	.LASF7
	.uleb128 0x13
	.byte	0x4
	.byte	0x5
	.string	"int"
	.uleb128 0x2
	.byte	0x8
	.byte	0x5
	.long	.LASF8
	.uleb128 0x3
	.long	.LASF10
	.byte	0x5
	.byte	0x2d
	.byte	0x1b
	.long	0x3a
	.uleb128 0x3
	.long	.LASF11
	.byte	0x5
	.byte	0x9a
	.byte	0x19
	.long	0x64
	.uleb128 0x2
	.byte	0x1
	.byte	0x6
	.long	.LASF12
	.uleb128 0xa
	.long	0x8a
	.uleb128 0x8
	.long	0x91
	.uleb128 0x3
	.long	.LASF13
	.byte	0x6
	.byte	0x1b
	.byte	0x14
	.long	0x72
	.uleb128 0x3
	.long	.LASF14
	.byte	0x7
	.byte	0x20
	.byte	0x19
	.long	0x3a
	.uleb128 0xb
	.byte	0x80
	.byte	0x7
	.byte	0x27
	.long	0xc9
	.uleb128 0x4
	.long	.LASF20
	.byte	0x7
	.byte	0x29
	.byte	0xe
	.long	0xc9
	.byte	0
	.byte	0
	.uleb128 0xc
	.long	0xa7
	.long	0xd9
	.uleb128 0xd
	.long	0x3a
	.byte	0xf
	.byte	0
	.uleb128 0x3
	.long	.LASF15
	.byte	0x7
	.byte	0x2a
	.byte	0x3
	.long	0xb3
	.uleb128 0xa
	.long	0xd9
	.uleb128 0x2
	.byte	0x8
	.byte	0x5
	.long	.LASF16
	.uleb128 0x2
	.byte	0x8
	.byte	0x7
	.long	.LASF17
	.uleb128 0x14
	.string	"u64"
	.byte	0x2
	.byte	0xc
	.byte	0x12
	.long	0x9b
	.uleb128 0x2
	.byte	0x10
	.byte	0x5
	.long	.LASF18
	.uleb128 0x2
	.byte	0x10
	.byte	0x7
	.long	.LASF19
	.uleb128 0xb
	.byte	0x20
	.byte	0x2
	.byte	0x22
	.long	0x14f
	.uleb128 0x4
	.long	.LASF21
	.byte	0x2
	.byte	0x23
	.byte	0x9
	.long	0xf8
	.byte	0
	.uleb128 0x4
	.long	.LASF22
	.byte	0x2
	.byte	0x24
	.byte	0x9
	.long	0xf8
	.byte	0x8
	.uleb128 0x4
	.long	.LASF23
	.byte	0x2
	.byte	0x25
	.byte	0x9
	.long	0xf8
	.byte	0x10
	.uleb128 0x4
	.long	.LASF24
	.byte	0x2
	.byte	0x26
	.byte	0x9
	.long	0xf8
	.byte	0x18
	.byte	0
	.uleb128 0x3
	.long	.LASF25
	.byte	0x2
	.byte	0x27
	.byte	0x3
	.long	0x112
	.uleb128 0x15
	.long	.LASF26
	.byte	0x8
	.value	0x164
	.byte	0xc
	.long	0x64
	.long	0x173
	.uleb128 0x1
	.long	0x96
	.uleb128 0x5
	.byte	0
	.uleb128 0x16
	.long	.LASF27
	.byte	0x3
	.byte	0x18
	.byte	0xd
	.long	.LASF35
	.long	0x194
	.uleb128 0x1
	.long	0x64
	.uleb128 0x1
	.long	0x64
	.uleb128 0x1
	.long	0x96
	.uleb128 0x5
	.byte	0
	.uleb128 0x17
	.long	.LASF28
	.byte	0x9
	.byte	0x82
	.byte	0xc
	.long	0x64
	.long	0x1b4
	.uleb128 0x1
	.long	0x7e
	.uleb128 0x1
	.long	0x2e
	.uleb128 0x1
	.long	0x1b4
	.byte	0
	.uleb128 0x8
	.long	0xe5
	.uleb128 0x18
	.long	.LASF41
	.byte	0xb
	.value	0x28a
	.byte	0x10
	.long	0x7e
	.uleb128 0xe
	.long	.LASF29
	.byte	0xa
	.long	0x1e0
	.uleb128 0x1
	.long	0xf8
	.uleb128 0x1
	.long	0xf8
	.uleb128 0x1
	.long	0xf8
	.byte	0
	.uleb128 0xe
	.long	.LASF30
	.byte	0x8
	.long	0x1fa
	.uleb128 0x1
	.long	0xf8
	.uleb128 0x1
	.long	0x1fa
	.uleb128 0x1
	.long	0x64
	.byte	0
	.uleb128 0x8
	.long	0x14f
	.uleb128 0x19
	.long	.LASF34
	.byte	0xa
	.byte	0xc
	.byte	0x6
	.long	0x20d
	.uleb128 0x5
	.byte	0
	.uleb128 0x1a
	.long	.LASF42
	.byte	0x1
	.byte	0x1e
	.byte	0x5
	.long	0x64
	.quad	.LFB12
	.quad	.LFE12-.LFB12
	.uleb128 0x1
	.byte	0x9c
	.long	0x257
	.uleb128 0x9
	.string	"rax"
	.byte	0x1f
	.long	0x9b
	.uleb128 0x2
	.byte	0x91
	.sleb128 -56
	.uleb128 0x9
	.string	"rdx"
	.byte	0x20
	.long	0x9b
	.uleb128 0x2
	.byte	0x91
	.sleb128 -48
	.uleb128 0x9
	.string	"rbx"
	.byte	0x21
	.long	0x9b
	.uleb128 0x2
	.byte	0x91
	.sleb128 -40
	.byte	0
	.uleb128 0x1b
	.long	.LASF43
	.byte	0x1
	.byte	0x8
	.byte	0x6
	.quad	.LFB11
	.quad	.LFE11-.LFB11
	.uleb128 0x1
	.byte	0x9c
	.long	0x308
	.uleb128 0x1c
	.long	.LASF31
	.byte	0x1
	.byte	0x9
	.byte	0xd
	.long	0x308
	.uleb128 0x3
	.byte	0x91
	.sleb128 -192
	.uleb128 0xf
	.long	0x318
	.quad	.LBB10
	.quad	.LBE10-.LBB10
	.byte	0x1
	.byte	0x15
	.byte	0x5
	.uleb128 0x6
	.long	0x325
	.uleb128 0x3
	.byte	0x91
	.sleb128 -220
	.uleb128 0x10
	.long	0x331
	.uleb128 0x3
	.byte	0x91
	.sleb128 -160
	.uleb128 0x1d
	.long	0x33c
	.quad	.LBB12
	.quad	.LBE12-.LBB12
	.long	0x2d2
	.uleb128 0x10
	.long	0x33d
	.uleb128 0x3
	.byte	0x91
	.sleb128 -208
	.byte	0
	.uleb128 0xf
	.long	0x34a
	.quad	.LBB13
	.quad	.LBE13-.LBB13
	.byte	0x2
	.byte	0x37
	.byte	0x9
	.uleb128 0x6
	.long	0x36b
	.uleb128 0x3
	.byte	0x91
	.sleb128 -200
	.uleb128 0x6
	.long	0x35f
	.uleb128 0x3
	.byte	0x91
	.sleb128 -212
	.uleb128 0x6
	.long	0x353
	.uleb128 0x3
	.byte	0x91
	.sleb128 -216
	.byte	0
	.byte	0
	.byte	0
	.uleb128 0xc
	.long	0x14f
	.long	0x318
	.uleb128 0xd
	.long	0x3a
	.byte	0
	.byte	0
	.uleb128 0x1e
	.long	.LASF44
	.byte	0x2
	.byte	0x32
	.byte	0x14
	.byte	0x3
	.long	0x34a
	.uleb128 0x7
	.long	.LASF36
	.byte	0x2
	.byte	0x32
	.byte	0x27
	.long	0x64
	.uleb128 0x11
	.long	.LASF32
	.byte	0x33
	.byte	0xf
	.long	0xd9
	.uleb128 0x1f
	.uleb128 0x11
	.long	.LASF33
	.byte	0x35
	.byte	0x5
	.long	0x2e
	.byte	0
	.byte	0
	.uleb128 0x20
	.long	.LASF35
	.byte	0x3
	.byte	0x25
	.byte	0x1
	.byte	0x3
	.uleb128 0x7
	.long	.LASF37
	.byte	0x3
	.byte	0x25
	.byte	0xc
	.long	0x64
	.uleb128 0x7
	.long	.LASF38
	.byte	0x3
	.byte	0x25
	.byte	0x1a
	.long	0x64
	.uleb128 0x7
	.long	.LASF39
	.byte	0x3
	.byte	0x25
	.byte	0x30
	.long	0x96
	.uleb128 0x5
	.byte	0
	.byte	0
	.section	.debug_abbrev,"",@progbits
.Ldebug_abbrev0:
	.uleb128 0x1
	.uleb128 0x5
	.byte	0
	.uleb128 0x49
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x2
	.uleb128 0x24
	.byte	0
	.uleb128 0xb
	.uleb128 0xb
	.uleb128 0x3e
	.uleb128 0xb
	.uleb128 0x3
	.uleb128 0xe
	.byte	0
	.byte	0
	.uleb128 0x3
	.uleb128 0x16
	.byte	0
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x49
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x4
	.uleb128 0xd
	.byte	0
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x38
	.uleb128 0xb
	.byte	0
	.byte	0
	.uleb128 0x5
	.uleb128 0x18
	.byte	0
	.byte	0
	.byte	0
	.uleb128 0x6
	.uleb128 0x5
	.byte	0
	.uleb128 0x31
	.uleb128 0x13
	.uleb128 0x2
	.uleb128 0x18
	.byte	0
	.byte	0
	.uleb128 0x7
	.uleb128 0x5
	.byte	0
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x49
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x8
	.uleb128 0xf
	.byte	0
	.uleb128 0xb
	.uleb128 0x21
	.sleb128 8
	.uleb128 0x49
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x9
	.uleb128 0x34
	.byte	0
	.uleb128 0x3
	.uleb128 0x8
	.uleb128 0x3a
	.uleb128 0x21
	.sleb128 1
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0x21
	.sleb128 14
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x2
	.uleb128 0x18
	.byte	0
	.byte	0
	.uleb128 0xa
	.uleb128 0x26
	.byte	0
	.uleb128 0x49
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0xb
	.uleb128 0x13
	.byte	0x1
	.uleb128 0xb
	.uleb128 0xb
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0x21
	.sleb128 9
	.uleb128 0x1
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0xc
	.uleb128 0x1
	.byte	0x1
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x1
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0xd
	.uleb128 0x21
	.byte	0
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x2f
	.uleb128 0xb
	.byte	0
	.byte	0
	.uleb128 0xe
	.uleb128 0x2e
	.byte	0x1
	.uleb128 0x3f
	.uleb128 0x19
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0x21
	.sleb128 10
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0x21
	.sleb128 6
	.uleb128 0x27
	.uleb128 0x19
	.uleb128 0x3c
	.uleb128 0x19
	.uleb128 0x1
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0xf
	.uleb128 0x1d
	.byte	0x1
	.uleb128 0x31
	.uleb128 0x13
	.uleb128 0x11
	.uleb128 0x1
	.uleb128 0x12
	.uleb128 0x7
	.uleb128 0x58
	.uleb128 0xb
	.uleb128 0x59
	.uleb128 0xb
	.uleb128 0x57
	.uleb128 0xb
	.byte	0
	.byte	0
	.uleb128 0x10
	.uleb128 0x34
	.byte	0
	.uleb128 0x31
	.uleb128 0x13
	.uleb128 0x2
	.uleb128 0x18
	.byte	0
	.byte	0
	.uleb128 0x11
	.uleb128 0x34
	.byte	0
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0x21
	.sleb128 2
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x49
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x12
	.uleb128 0x11
	.byte	0x1
	.uleb128 0x25
	.uleb128 0xe
	.uleb128 0x13
	.uleb128 0xb
	.uleb128 0x3
	.uleb128 0x1f
	.uleb128 0x1b
	.uleb128 0x1f
	.uleb128 0x11
	.uleb128 0x1
	.uleb128 0x12
	.uleb128 0x7
	.uleb128 0x10
	.uleb128 0x17
	.byte	0
	.byte	0
	.uleb128 0x13
	.uleb128 0x24
	.byte	0
	.uleb128 0xb
	.uleb128 0xb
	.uleb128 0x3e
	.uleb128 0xb
	.uleb128 0x3
	.uleb128 0x8
	.byte	0
	.byte	0
	.uleb128 0x14
	.uleb128 0x16
	.byte	0
	.uleb128 0x3
	.uleb128 0x8
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x49
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x15
	.uleb128 0x2e
	.byte	0x1
	.uleb128 0x3f
	.uleb128 0x19
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0x5
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x27
	.uleb128 0x19
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x3c
	.uleb128 0x19
	.uleb128 0x1
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x16
	.uleb128 0x2e
	.byte	0x1
	.uleb128 0x3f
	.uleb128 0x19
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x6e
	.uleb128 0xe
	.uleb128 0x27
	.uleb128 0x19
	.uleb128 0x3c
	.uleb128 0x19
	.uleb128 0x1
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x17
	.uleb128 0x2e
	.byte	0x1
	.uleb128 0x3f
	.uleb128 0x19
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x27
	.uleb128 0x19
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x3c
	.uleb128 0x19
	.uleb128 0x1
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x18
	.uleb128 0x2e
	.byte	0
	.uleb128 0x3f
	.uleb128 0x19
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0x5
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x27
	.uleb128 0x19
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x3c
	.uleb128 0x19
	.byte	0
	.byte	0
	.uleb128 0x19
	.uleb128 0x2e
	.byte	0x1
	.uleb128 0x3f
	.uleb128 0x19
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x3c
	.uleb128 0x19
	.uleb128 0x1
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x1a
	.uleb128 0x2e
	.byte	0x1
	.uleb128 0x3f
	.uleb128 0x19
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x27
	.uleb128 0x19
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x11
	.uleb128 0x1
	.uleb128 0x12
	.uleb128 0x7
	.uleb128 0x40
	.uleb128 0x18
	.uleb128 0x7c
	.uleb128 0x19
	.uleb128 0x1
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x1b
	.uleb128 0x2e
	.byte	0x1
	.uleb128 0x3f
	.uleb128 0x19
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x27
	.uleb128 0x19
	.uleb128 0x11
	.uleb128 0x1
	.uleb128 0x12
	.uleb128 0x7
	.uleb128 0x40
	.uleb128 0x18
	.uleb128 0x7c
	.uleb128 0x19
	.uleb128 0x1
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x1c
	.uleb128 0x34
	.byte	0
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x49
	.uleb128 0x13
	.uleb128 0x2
	.uleb128 0x18
	.byte	0
	.byte	0
	.uleb128 0x1d
	.uleb128 0xb
	.byte	0x1
	.uleb128 0x31
	.uleb128 0x13
	.uleb128 0x11
	.uleb128 0x1
	.uleb128 0x12
	.uleb128 0x7
	.uleb128 0x1
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x1e
	.uleb128 0x2e
	.byte	0x1
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x27
	.uleb128 0x19
	.uleb128 0x20
	.uleb128 0xb
	.uleb128 0x1
	.uleb128 0x13
	.byte	0
	.byte	0
	.uleb128 0x1f
	.uleb128 0xb
	.byte	0x1
	.byte	0
	.byte	0
	.uleb128 0x20
	.uleb128 0x2e
	.byte	0x1
	.uleb128 0x3f
	.uleb128 0x19
	.uleb128 0x3
	.uleb128 0xe
	.uleb128 0x3a
	.uleb128 0xb
	.uleb128 0x3b
	.uleb128 0xb
	.uleb128 0x39
	.uleb128 0xb
	.uleb128 0x27
	.uleb128 0x19
	.uleb128 0x20
	.uleb128 0xb
	.byte	0
	.byte	0
	.byte	0
	.section	.debug_aranges,"",@progbits
	.long	0x2c
	.value	0x2
	.long	.Ldebug_info0
	.byte	0x8
	.byte	0
	.value	0
	.value	0
	.quad	.Ltext0
	.quad	.Letext0-.Ltext0
	.quad	0
	.quad	0
	.section	.debug_line,"",@progbits
.Ldebug_line0:
	.section	.debug_str,"MS",@progbits,1
.LASF41:
	.string	"getpid"
.LASF19:
	.string	"__int128 unsigned"
.LASF15:
	.string	"cpu_set_t"
.LASF9:
	.string	"size_t"
.LASF43:
	.string	"install_addshl_vmwrite_patch"
.LASF34:
	.string	"do_fix_IN_patch"
.LASF21:
	.string	"uop0"
.LASF22:
	.string	"uop1"
.LASF23:
	.string	"uop2"
.LASF25:
	.string	"ucode_t"
.LASF13:
	.string	"uint64_t"
.LASF30:
	.string	"patch_ucode"
.LASF16:
	.string	"long long int"
.LASF4:
	.string	"unsigned char"
.LASF24:
	.string	"seqw"
.LASF39:
	.string	"__format"
.LASF37:
	.string	"__status"
.LASF36:
	.string	"core_id"
.LASF5:
	.string	"short unsigned int"
.LASF18:
	.string	"__int128"
.LASF8:
	.string	"long int"
.LASF27:
	.string	"__error_alias"
.LASF2:
	.string	"long unsigned int"
.LASF11:
	.string	"__pid_t"
.LASF33:
	.string	"__cpu"
.LASF3:
	.string	"unsigned int"
.LASF17:
	.string	"long long unsigned int"
.LASF20:
	.string	"__bits"
.LASF6:
	.string	"signed char"
.LASF32:
	.string	"cpuset"
.LASF38:
	.string	"__errnum"
.LASF31:
	.string	"ucode_patch"
.LASF35:
	.string	"error"
.LASF14:
	.string	"__cpu_mask"
.LASF12:
	.string	"char"
.LASF44:
	.string	"assign_to_core"
.LASF26:
	.string	"printf"
.LASF7:
	.string	"short int"
.LASF10:
	.string	"__uint64_t"
.LASF29:
	.string	"hook_match_and_patch"
.LASF28:
	.string	"sched_setaffinity"
.LASF40:
	.string	"GNU C17 11.4.0 -masm=intel -march=x86-64-v2 -mtune=generic -g -ggdb -O0 -fPIE -fasynchronous-unwind-tables -fstack-protector-strong -fstack-clash-protection -fcf-protection"
.LASF42:
	.string	"main"
	.section	.debug_line_str,"MS",@progbits,1
.LASF1:
	.string	"/home/redunlock/code/lib-micro/Sabrina/experiments"
.LASF0:
	.string	"test.c"
	.ident	"GCC: (Ubuntu 11.4.0-1ubuntu1~22.04.2) 11.4.0"
	.section	.note.GNU-stack,"",@progbits
	.section	.note.gnu.property,"a"
	.align 8
	.long	1f - 0f
	.long	4f - 1f
	.long	5
0:
	.string	"GNU"
1:
	.align 8
	.long	0xc0000002
	.long	3f - 2f
2:
	.long	0x3
3:
	.align 8
4:
