#include <misc.h>

	.text

#define SAVE_ALL \
	cld; \
	pushl %es; \
	pushl %ds; \
	pushl %eax; \
	pushl %ebp; \
	pushl %edi; \
	pushl %esi; \
	pushl %edx; \
	pushl %ecx; \
	pushl %ebx; \
	movw $KERNEL_DS, %ax; \
	movw %ax, %es; \
	movw %ax, %ds

#define RESTORE_ALL \
	popl %ebx; \
	popl %ecx; \
	popl %edx; \
	popl %esi; \
	popl %edi; \
	popl %ebp; \
	popl %eax; \
	popl %ds; \
	popl %es; \
	addl $4, %esp; \
	iret

/****************
 * IRQ handlers *
 ****************/
.globl _irq0, _irq1, _irq2, _irq3, _irq4, _irq5, _irq6, _irq7
.globl _irq8, _irq9, _irqa, _irqb, _irqc, _irqd, _irqe, _irqf

/*
 * send EOI to master PIC
 */
#define EOI_MASTER \
	movb $0x20, %al; \
	outb %al, $0x20

/*
 * send EOI to slave PIC
 */
#define EOI_SLAVE \
	movb $0x20, %al; \
	outb %al, $0xa0

_irq0:
	pushl $0
	SAVE_ALL
	call _irq_PIT
	EOI_MASTER
	RESTORE_ALL

_irq1:
	pushl $0
	SAVE_ALL
	EOI_MASTER
	RESTORE_ALL

_irq2:
_irq3:
_irq4:
_irq5:
_irq6:
	pushl %eax
	EOI_MASTER
	popl %eax
	iret

_irq7:
	pushl %eax
	movb $0x0b, %al		/* read ISR of master PIC */
	outb %al, $0x20
	inb $0x20, %al
	andb $0x80, %al		/* check whether IRQ7 is a spurious IRQ */
	jz 1f
	EOI_MASTER
1:	popl %eax
	iret

_irq8:
_irq9:
_irqa:
_irqb:
_irqc:
_irqd:
_irqe:
	pushl %eax
	EOI_MASTER
	EOI_SLAVE
	popl %eax
	iret
	
_irqf:
	pushl %eax
	movb $0x0b, %al		/* read ISR of slave PIC */
	outb %al, $0xa0
	inb $0xa0, %al
	andb $0x80, %al		/* check whether IRQ15 is a spurious IRQ */
	jz 1f
	EOI_SLAVE
1:	EOI_MASTER
	popl %eax
	iret

