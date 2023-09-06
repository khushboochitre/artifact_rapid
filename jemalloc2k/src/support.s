.text
.globl switch_stack
.extern san_stackalloc

# .comm name, size, alignment


#ARGS: rdi, rsi, rdx, rcx, r8, r9
#CALLEE-SAVED: RBX, R12-R15, RBP

switch_stack:
	push %rdi
	push %rsi
	push %rdx
	call san_stackalloc
	pop %rdx
	pop %rsi
	pop %rdi
	mov %rsp, %r10
	and $63, %r10
	add $(1ULL << 20), %rax
	sub $512, %rax
	or %r10, %rax
	mov (%rsp), %r10
	mov %r10, (%rax)
	mov %rax, %rsp
	ret
