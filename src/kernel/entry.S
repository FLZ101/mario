#include <misc.h>

/*
 * Stack layout when 'ret_from_sys_call':
 *
 *   0(%esp) - ebx
 *   4(%esp) - ecx
 *   8(%esp) - edx
 *   c(%esp) - esi
 *  10(%esp) - edi
 *  14(%esp) - ebp
 *  18(%esp) - eax
 *  1c(%esp) - ds
 *  20(%esp) - es
 *  24(%esp) - error_code
 *  28(%esp) - eip
 *  2c(%esp) - cs
 *  30(%esp) - eflags
 *  34(%esp) - esp
 *  38(%esp) - ss
 */

EBX			= 0x00
ECX			= 0x04
EDX			= 0x08
ESI			= 0x0c
EDI			= 0x10
EBP			= 0x14
EAX			= 0x18
DS			= 0x1c
ES			= 0x20
ERROR_CODE	= 0x24
EIP			= 0x28
CS			= 0x2c
EFLAGS		= 0x30
ESP			= 0x34
SS			= 0x38

/*
 * these are offsets into the task-struct.
 */
state	= 0
signal	= 12
blocked	= 16
need_resched = 20

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
	movw $KERNEL_DS, %dx; \
	movw %dx, %es; \
	movw %dx, %ds

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

#define GET_CURRENT(reg) \
	movl $-8192, reg; \
	andl %esp, reg

	.text

ENTRY(fork_ret)
	jmp ret_from_sys_call

ENTRY(system_call)
	pushl %eax	/* the system call number */
	SAVE_ALL
	call *_sys_call_table(, %eax, 4)
	movl %eax, EAX(%esp)	/* save the return value */

ret_from_sys_call:
	/* handle bottom_half */
	movl _bh_mask, %eax
	andl _bh_active, %eax
	je 1f
	/* wake up the bh_thread */
	pushl _bh_task
	call _wake_up_process
	addl $4, %esp
1:
	GET_CURRENT(%ebx)
	movl need_resched(%ebx), %ecx
	testl %ecx, %ecx
	je 2f
	pushl %ebx
	call _schedule
	popl %ebx
2:
	movl blocked(%ebx), %ecx
	movl %ecx, %edx
	notl %ecx
	andl signal(%ebx), %ecx
	je 3f
	movl %esp, %ecx
	pushl %edx
	pushl %ecx
	call _do_signal
	addl $8, %esp
3:
	RESTORE_ALL

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
	pushl $-1
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

trap_common:
	cld
	pushl %ds
	pushl %eax
	pushl %ebp
	pushl %edi
	pushl %esi
	pushl %edx
	pushl %ecx
	pushl %ebx
	movl ES(%esp), %edi		# get the function address
	movl %es, %eax
	movl %eax, ES(%esp)
	movl $-1, %eax
	xchgl %eax, ERROR_CODE(%esp)	# get the error code
	movl %esp, %edx
	pushl %eax				# push the error code
	pushl %edx				# push the trap_frame pointer
	movw $KERNEL_DS, %ax
	movw %ax, %es
	movw %ax, %ds
	call *%edi
	addl $8, %esp
	jmp ret_from_sys_call

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

ret_from_intr:
	jmp ret_from_sys_call

_irq0:
	pushl $-1
	SAVE_ALL
	call _irq_PIT
	EOI_MASTER
	jmp ret_from_intr

_irq1:
	pushl $-1
	SAVE_ALL
	call _irq_PS2
	EOI_MASTER
	jmp ret_from_intr

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

	.data
	.balign 4
ENTRY(sys_call_table)
	.long _sys_brk
	.long _sys_mmap
	.long _sys_munmap
	.long _sys_alarm
	.long _sys_execve
	.long _sys_exit
	.long _sys_fork
	.long _sys_getpid
	.long _sys_getppid
	.long _sys_nanosleep
	.long _sys_pause
	.long _sys_waitpid
	.long _sys_chdir
	.long _sys_fchdir
	.long _sys_chroot
	.long _sys_close
	.long _sys_creat
	.long _sys_dup
	.long _sys_dup2
	.long _sys_getdents
	.long 0	/* _sys_ioctl */
	.long _sys_link
	.long _sys_lseek
	.long _sys_mkdir
	.long _sys_mknod
	.long _sys_mount
	.long _sys_open
	.long 0	/* _sys_pipe */
	.long _sys_read
	.long _sys_rename
	.long _sys_rmdir
	.long _sys_stat
	.long _sys_fstat
	.long _sys_truncate
	.long _sys_ftruncate
	.long _sys_umount
	.long _sys_unlink
	.long _sys_write
	.long _sys_time
	.long _sys_getitimer
	.long _sys_setitimer
	.long _sys_getpgid
	.long _sys_getpgrp
	.long _sys_setpgid
	.long _sys_setsid
	.long _sys_kill
	.long _sys_signal
	.long _sys_sigaction
	.long _sys_sigsuspend
	.long _sys_sigpending
	.long _sys_sigprocmask
	.long _sys_sigreturn
	.long _sys_putchar
