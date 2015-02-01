#include <multiboot.h>
#include <mm.h>

	.section .multiboot
	.balign 4
multiboot:
	.long MB_MAGIC
	.long MB_FLAGS
	.long MB_CHECK
	
	.long multiboot
	.long __text
	.long __edata
	.long __ebss
	.long start
	
	.text
	.globl start
start:
	cli
	lgdt _gdtr
	ljmp $KERNEL_CS, $1f
1:
	movw $KERNEL_DS, %cx
	movw %cx, %ds
	movw %cx, %es
	movw %cx, %fs
	movw %cx, %gs

	movw %cx, %ss
	movl $_sys_stack, %esp

	cmpl $MB_MAGIC_EAX, %eax
	jne .

	pushl %ebx
	call _mario
	jmp .

	.data
_gdt:
	.quad 0x0000000000000000	/* NULL descriptor */
	.quad 0x0000000000000000	/* tss */
	.quad 0x00cf9a000000ffff	/* 0x10, 0 ~ 4GB kernel code */
	.quad 0x00cf92000000ffff	/* 0x18, 0 ~ 4GB kernel data */
	.quad 0x00cffa000000ffff	/* 0x23, 0 ~ 4GB user code */
	.quad 0x00cff2000000ffff	/* 0x2b, 0 ~ 4GB user data */

_gdtr:
	.word 6*8 - 1
	.long _gdt

	.bss
	.space 8192
_sys_stack:
