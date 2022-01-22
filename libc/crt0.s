	.text
	.globl start, ___main

start:
	movl %esp, %eax
	xorl %ebp, %ebp
	pushl %ebp
	pushl %ebp
	pushl %eax
	call ___libc_init
1:
	hlt
	jmp 1b

___main:
	ret
