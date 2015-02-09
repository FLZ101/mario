#include <misc.h>

/*
 * Stack layout when 'trap_ret':
 * 
 *   0(%esp) - ebx
 *   4(%esp) - ecx
 *   8(%esp) - edx
 *   c(%esp) - esi
 *  10(%esp) - edi
 *  14(%esp) - ebp
 *  18(%esp) - eax
 *  1c(%esp) - ods
 *  20(%esp) - oes
 *  24(%esp) - error_code
 *  28(%esp) - eip
 *  2c(%esp) - ocs
 *  30(%esp) - eflags
 *  34(%esp) - esp
 *  38(%esp) - oss
 */

EBX		= 0x00
ECX		= 0x04
EDX		= 0x08
ESI		= 0x0c
EDI		= 0x10
EBP		= 0x14
EAX		= 0x18
ODS		= 0x1c
OES		= 0x20
ERROR_CODE	= 0x24
EIP		= 0x28
OCS		= 0x2c
EFLAGS		= 0x30
ESP		= 0x34
OSS		= 0x38
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
 
ENTRY(divide_error)
	pushl $0
	pushl $_do_divide_error
	jmp trap_common
ENTRY(debug)
	pushl $0
	pushl $_do_debug
	jmp trap_common
ENTRY(nmi)
	pushl $0
	pushl $_do_nmi
	jmp trap_common
ENTRY(int3)
	pushl $0
	pushl $_do_int3
	jmp trap_common
ENTRY(overflow)
	pushl $0
	pushl $_do_overflow
	jmp trap_common
ENTRY(bounds)
	pushl $0
	pushl $_do_bounds
	jmp trap_common
ENTRY(invalid_op)
	pushl $0
	pushl $_do_invalid_op
	jmp trap_common
ENTRY(device_not_available)
	pushl $0
	pushl $_do_device_not_available
	jmp trap_common
ENTRY(double_fault)
	pushl $_do_double_fault
	jmp trap_common
ENTRY(coprocessor_segment_overrun)
	pushl $0
	pushl $_do_coprocessor_segment_overrun
	jmp trap_common
ENTRY(invalid_TSS)
	pushl $_do_invalid_TSS
	jmp trap_common
ENTRY(segment_not_present)
	pushl $_do_segment_not_present
	jmp trap_common
ENTRY(stack_segment)
	pushl $_do_stack_segment
	jmp trap_common
ENTRY(general_protection)
	pushl $_do_general_protection
	jmp trap_common
ENTRY(page_fault)
	pushl $_do_page_fault
	jmp trap_common
ENTRY(spurious_interrupt_bug)
	pushl $0
	pushl $_do_spurious_interrupt_bug
	jmp trap_common
ENTRY(coprocessor_error)
	pushl $0
	pushl $_do_coprocessor_error
	jmp trap_common
ENTRY(system_call)
	pushl %eax
	pushl $_do_system_call
	jmp trap_common

trap_common:
	cld; \
	pushl %ds; \
	pushl %eax; \
	pushl %ebp; \
	pushl %edi; \
	pushl %esi; \
	pushl %edx; \
	pushl %ecx; \
	pushl %ebx; \
	movl OES(%esp), %edi; \
	movw %es, OES(%esp); \
	movw $KERNEL_DS, %ax; \
	movw %ax, %es; \
	movw %ax, %ds
	pushl $trap_ret
	jmp *%edi

trap_ret:
	RESTORE_ALL


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

