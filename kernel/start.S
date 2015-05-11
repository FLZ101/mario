#include <multiboot.h>
#include <misc.h>

	.text
	.balign 4
multiboot:
	.long MB_MAGIC
	.long MB_FLAGS
	.long MB_CHECK

	.long __phy(multiboot)
	.long __phy(__text)
	.long __phy(__einit)
	.long __phy(__ebss)
	.long __phy(start)

	.globl start
start:
	cli
	lgdt __phy(gdtr)
	ljmp $KERNEL_CS, $1f - KERNEL_BASE
1:
	movw $KERNEL_DS, %cx
	movw %cx, %ds
	movw %cx, %es
	movw %cx, %fs
	movw %cx, %gs

	/*
	 * initialize swapper_pg_dir (the first page directory)
	 */
	movl $__phy(_pg0)+007, __phy(_swapper_pg_dir)
	movl $__phy(_pg1)+007, __phy(_swapper_pg_dir)+4
	movl $__phy(_pg0)+007, __phy(_swapper_pg_dir)+2048
	movl $__phy(_pg1)+007, __phy(_swapper_pg_dir)+2048+4

	/*
	 * initialize page tables
	 */
	movl $__phy(_pg0), %edi
	movl $007, %ecx	/* 007 means PRESENT+RW+USER */
1:
	movl %ecx, (%edi)
	add $4, %edi
	add $0x1000, %ecx
	cmp $__phy(_empty_zero_page), %edi
	jne 1b

	/*
	 * enable paging
	 */
	movl $__phy(_swapper_pg_dir), %ecx
	movl %ecx, %cr3
	movl %cr0, %ecx
	orl $0x80000000, %ecx
	movl %ecx, %cr0
	jmp 1f
1:
	movl $1f, %ecx
	jmp *%ecx
1:	/*
	 * gdt must lie in kernel space, so we load it again
	 */
	movl $gdt, (gdtr+2)
	lgdt gdtr

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
	.long __phy(gdt)

stack_start:
	.long _init_task_union + 8192
	.word KERNEL_DS

idtr:
	.word 256*8 - 1
	.long _idt

	.bss
ENTRY(swapper_pg_dir)

	.org 0x1000
ENTRY(pg0)

	.org 0x2000
ENTRY(pg1)

	.org 0x3000
ENTRY(empty_zero_page)

	.org 0x4000
ENTRY(empty_bad_page)

	.org 0x5000
ENTRY(empty_bad_page_table)
