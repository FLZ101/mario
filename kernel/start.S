#include <multiboot.h>
#include <misc.h>

	.section .multiboot
	.balign 4
multiboot:
	.long MB_MAGIC
	.long MB_FLAGS
	.long MB_CHECK
	
	.long multiboot
	.long __text
	.long __einit
	.long __ebss
	.long start
	
	.text
	.globl start
start:
	cli
	lgdt gdtr
	ljmp $KERNEL_CS, $1f
1:
	movw $KERNEL_DS, %cx
	movw %cx, %ds
	movw %cx, %es
	movw %cx, %fs
	movw %cx, %gs

	lss stack_start, %esp

	pushl $0
	popfl

	/*
	 * A multiboot-compliant bootloader sets eax to MB_MAGIC_EAX and 
	 * ebx to the physical address of the Multiboot information 
	 * structure before it jumps to the kernel.
	 */
	cmpl $MB_MAGIC_EAX, %eax
	jne .
	pushl %ebx

	/*
	 * Make all 256 entries in idt point to ignore_int and then lidt
	 */
	lea ignore_int, %edx
	movl $(KERNEL_CS << 16), %eax
	movw %dx, %ax
	movw $0x8e00, %dx	/* interrupt gate, dpl=0 */

	lea _idt, %edi
	movl $256, %ecx
set_idt:
	movl %eax, (%edi)
	movl %edx, 4(%edi)
	addl $8, %edi
	loopne set_idt

	lidt idtr
	
	/*
	 * Load tss, is it too early?
	 */
	movl $_init_tss, %eax
	xorl %ebx, %ebx
	xorl %ecx, %ecx
	roll $16, %eax
	xchgb %al, %bl
	xchgw %ax, %cx
	shll $16, %ecx
	addl %ecx, %ebx
	addl %eax, gdt + 8
	addl %ebx, gdt + 12
	movw $8, %ax
	ltr %ax
	
	call _mario
	jmp .

/*
 * We just don't want some unexpected events to make the kernel crash
 */
ignore_int:
	iret
	
	.data
	.balign 8
gdt:
	.quad 0x0000000000000000	/* NULL descriptor */
	.quad 0x0000890000000067	/* 0x08, 104-byte tss */
	.quad 0x00cf9a000000ffff	/* 0x10, 0 ~ 4GB kernel code */
	.quad 0x00cf92000000ffff	/* 0x18, 0 ~ 4GB kernel data */
	.quad 0x00cffa000000ffff	/* 0x23, 0 ~ 4GB user code */
	.quad 0x00cff2000000ffff	/* 0x2b, 0 ~ 4GB user data */

gdtr:
	.word 6*8 - 1
	.long gdt

stack_start:
	.long _init_task_union + 8192
	.word KERNEL_DS

idtr:
	.word 256*8 - 1
	.long _idt
