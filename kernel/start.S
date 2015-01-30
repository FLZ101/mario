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
	movl $sys_stack, %esp

	pushl %ebx
	pushl %eax

	
	
	call _mario
	jmp .

	.data
_gdt:
	.quad 0x0000000000000000	/* NULL descriptor */
	.quad 0x0000000000000000	/* not used */
	.quad 0x00cf9a000000ffff	/* 0x10 kernel 4GB code at 0x00000000 */
	.quad 0x00cf92000000ffff	/* 0x18 kernel 4GB data at 0x00000000 */
	.quad 0x00cffa000000ffff	/* 0x23 user   4GB code at 0x00000000 */
	.quad 0x00cff2000000ffff	/* 0x2b user   4GB data at 0x00000000 */
	.quad 0x0

_gdtr:
	.word 7 * 8 - 1
	.long _gdt

	.bss
	.space 8192
sys_stack:
