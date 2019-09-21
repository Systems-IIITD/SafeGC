.text
.globl mymalloc
.globl runGC
.extern _mymalloc
.extern _runGC

mymalloc:
# nuke caller-saved registers except argument(s)
	xor %rax, %rax
	xor %rcx, %rcx
	xor %rdx, %rdx
	xor %rsi, %rsi
	xor %r8, %r8
	xor %r9, %r9
	xor %r10, %r10
	xor %r11, %r11
	push %rbp
	mov %rsp, %rbp
# move possible register roots on stack
	push %rbx
	push %r12
	push %r13
	push %r14
	push %r15
# put marker on stack
	push $0x12abcdef
	sub $16, %rsp
	call _mymalloc
	mov %rbp, %rsp
	pop %rbp
	ret

runGC:
# nuke all caller-saved registers
	xor %rax, %rax
	xor %rcx, %rcx
	xor %rdx, %rdx
	xor %rsi, %rsi
	xor %rdi, %rdi
	xor %r8, %r8
	xor %r9, %r9
	xor %r10, %r10
	xor %r11, %r11
	push %rbp
	mov %rsp, %rbp
# move possible register roots on stack
	push %rbx
	push %r12
	push %r13
	push %r14
	push %r15
# put marker on stack
	push $0x12abcdef
	sub $16, %rsp
	call _runGC
	mov %rbp, %rsp
	pop %rbp
	ret
