	.text
	.globl start

start:
	movl %esp, %eax
	xorl %ebp, %ebp
	pushl %ebp
	pushl %ebp
	pushl %eax
	call __libc_init
1:
	hlt
	jmp 1b
