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
 *  24(%esp) - fs
 *  28(%esp) - gs
 *  2c(%esp) - error_code
 *  30(%esp) - eip
 *  34(%esp) - cs
 *  38(%esp) - eflags
 *  3c(%esp) - esp
 *  40(%esp) - ss
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
FS			= 0x24
GS			= 0x28
ERROR_CODE	= 0x2c
EIP			= 0x30
CS			= 0x34
EFLAGS		= 0x38
ESP			= 0x3c
SS			= 0x40

/*
 * these are offsets into the task-struct.
 */
state	= 0
signal	= 12
blocked	= 16
need_resched = 20

#define SAVE_ALL \
	cld; \
	pushl %gs; \
	pushl %fs; \
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
	popl %fs; \
	popl %gs; \
	addl $4, %esp; \
	iret

#define GET_CURRENT(reg) \
	movl $-8192, reg; \
	andl %esp, reg

	.text

ENTRY(fork_ret)
	jmp ret_common

ENTRY(system_call)
	pushl %eax	/* the system call number */
	SAVE_ALL
	call *sys_call_table(, %eax, 4)
	movl %eax, EAX(%esp)	/* save the return value */

ret_from_sys_call:
	jmp ret_common

ret_common:
	/* handle bottom_half */
	movl bh_mask, %eax
	andl bh_active, %eax
	je 1f
	/* wake up the bh_thread */
	pushl bh_task
	call wake_up_process
	addl $4, %esp
1:
	GET_CURRENT(%ebx)
	movl need_resched(%ebx), %ecx
	testl %ecx, %ecx
	je 2f
	pushl %ebx
	call schedule
	popl %ebx
2:
	/* only handle signal if return to userland */
	testl $3, CS(%esp)
	jz 3f

	movl blocked(%ebx), %ecx
	movl %ecx, %edx
	notl %ecx
	andl signal(%ebx), %ecx
	je 3f
	movl %esp, %ecx
	pushl %edx
	pushl %ecx
	call do_signal
	addl $8, %esp
3:
	RESTORE_ALL

ENTRY(divide_error)
	pushl $0
	pushl $do_divide_error
	jmp trap_common
ENTRY(debug)
	pushl $0
	pushl $do_debug
	jmp trap_common
ENTRY(nmi)
	pushl $0
	pushl $do_nmi
	jmp trap_common
ENTRY(int3)
	pushl $0
	pushl $do_int3
	jmp trap_common
ENTRY(overflow)
	pushl $0
	pushl $do_overflow
	jmp trap_common
ENTRY(bounds)
	pushl $0
	pushl $do_bounds
	jmp trap_common
ENTRY(invalid_op)
	pushl $0
	pushl $do_invalid_op
	jmp trap_common
ENTRY(device_not_available)
	pushl $-1
	pushl $do_device_not_available
	jmp trap_common
ENTRY(double_fault)
	pushl $do_double_fault
	jmp trap_common
ENTRY(coprocessor_segment_overrun)
	pushl $0
	pushl $do_coprocessor_segment_overrun
	jmp trap_common
ENTRY(invalid_TSS)
	pushl $do_invalid_TSS
	jmp trap_common
ENTRY(segment_not_present)
	pushl $do_segment_not_present
	jmp trap_common
ENTRY(stack_segment)
	pushl $do_stack_segment
	jmp trap_common
ENTRY(general_protection)
	pushl $do_general_protection
	jmp trap_common
ENTRY(page_fault)
	pushl $do_page_fault
	jmp trap_common
ENTRY(spurious_interrupt_bug)
	pushl $0
	pushl $do_spurious_interrupt_bug
	jmp trap_common
ENTRY(coprocessor_error)
	pushl $0
	pushl $do_coprocessor_error

trap_common:
	cld
	pushl %fs
	pushl %es
	pushl %ds
	pushl %eax
	pushl %ebp
	pushl %edi
	pushl %esi
	pushl %edx
	pushl %ecx
	pushl %ebx
	movl GS(%esp), %edi		# get the function address
	movl %gs, %eax
	movl %eax, GS(%esp)
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
	RESTORE_ALL

/****************
 * IRQ handlers *
 ****************/
.globl irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
.globl irq8, irq9, irqa, irqb, irqc, irqd, irqe, irqf

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
	jmp ret_common

irq0:
	pushl $-1
	SAVE_ALL
	call irq_PIT
	EOI_MASTER
	jmp ret_from_intr

irq1:
	pushl $-1
	SAVE_ALL
	call irq_PS2
	EOI_MASTER
	jmp ret_from_intr

irq2:
irq3:
irq4:
	pushl $-1
	SAVE_ALL
	call irq_uart
	EOI_MASTER
	jmp ret_from_intr
irq5:
irq6:
	pushl %eax
	EOI_MASTER
	popl %eax
	iret

irq7:
	pushl %eax
	movb $0x0b, %al		/* read ISR of master PIC */
	outb %al, $0x20
	inb $0x20, %al
	andb $0x80, %al		/* check whether IRQ7 is a spurious IRQ */
	jz 1f
	EOI_MASTER
1:	popl %eax
	iret

irq8:
irq9:
irqa:
irqb:
irqc:
irqd:
irqe:
	pushl %eax
	EOI_MASTER
	EOI_SLAVE
	popl %eax
	iret

irqf:
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
	.long sys_not_exist /* SYS_restart_syscall */
	.long sys_exit
	.long sys_fork
	.long sys_read
	.long sys_write
	.long sys_open
	.long sys_close
	.long sys_waitpid
	.long sys_creat
	.long sys_link
	.long sys_unlink
	.long sys_execve
	.long sys_chdir
	.long sys_time
	.long sys_mknod
	.long sys_not_exist /* SYS_chmod */
	.long sys_not_exist /* SYS_lchown */
	.long sys_not_exist /* SYS_break */
	.long sys_not_exist /* SYS_oldstat */
	.long sys_lseek
	.long sys_getpid
	.long sys_mount
	.long sys_umount
	.long sys_not_exist /* SYS_setuid */
	.long sys_not_exist /* SYS_getuid */
	.long sys_not_exist /* SYS_stime */
	.long sys_not_exist /* SYS_ptrace */
	.long sys_alarm
	.long sys_not_exist /* SYS_oldfstat */
	.long sys_pause
	.long sys_not_exist /* SYS_utime */
	.long sys_not_exist /* SYS_stty */
	.long sys_not_exist /* SYS_gtty */
	.long sys_not_exist /* SYS_access */
	.long sys_not_exist /* SYS_nice */
	.long sys_not_exist /* SYS_ftime */
	.long sys_not_exist /* SYS_sync */
	.long sys_kill
	.long sys_rename
	.long sys_mkdir
	.long sys_rmdir
	.long sys_dup
	.long sys_pipe
	.long sys_not_exist /* SYS_times */
	.long sys_not_exist /* SYS_prof */
	.long sys_brk
	.long sys_not_exist /* SYS_setgid */
	.long sys_not_exist /* SYS_getgid */
	.long sys_signal
	.long sys_not_exist /* SYS_geteuid */
	.long sys_not_exist /* SYS_getegid */
	.long sys_not_exist /* SYS_acct */
	.long sys_not_exist /* SYS_umount2 */
	.long sys_not_exist /* SYS_lock */
	.long sys_ioctl
	.long sys_fcntl
	.long sys_not_exist /* SYS_mpx */
	.long sys_setpgid
	.long sys_not_exist /* SYS_ulimit */
	.long sys_not_exist /* SYS_oldolduname */
	.long sys_not_exist /* SYS_umask */
	.long sys_chroot
	.long sys_not_exist /* SYS_ustat */
	.long sys_dup2
	.long sys_getppid
	.long sys_not_exist /* SYS_getpgrp */
	.long sys_setsid
	.long sys_sigaction
	.long sys_not_exist /* SYS_sgetmask */
	.long sys_not_exist /* SYS_ssetmask */
	.long sys_not_exist /* SYS_setreuid */
	.long sys_not_exist /* SYS_setregid */
	.long sys_sigsuspend
	.long sys_sigpending
	.long sys_not_exist /* SYS_sethostname */
	.long sys_setrlimit
	.long sys_getrlimit
	.long sys_not_exist /* SYS_getrusage */
	.long sys_not_exist /* SYS_gettimeofday_time32 */
	.long sys_not_exist /* SYS_settimeofday_time32 */
	.long sys_not_exist /* SYS_getgroups */
	.long sys_not_exist /* SYS_setgroups */
	.long sys_not_exist /* SYS_select */
	.long sys_not_exist /* SYS_symlink */
	.long sys_not_exist /* SYS_oldlstat */
	.long sys_not_exist /* SYS_readlink */
	.long sys_not_exist /* SYS_uselib */
	.long sys_not_exist /* SYS_swapon */
	.long sys_not_exist /* SYS_reboot */
	.long sys_not_exist /* SYS_readdir */
	.long sys_not_exist /* SYS_mmap */
	.long sys_munmap
	.long sys_truncate
	.long sys_ftruncate
	.long sys_not_exist /* SYS_fchmod */
	.long sys_not_exist /* SYS_fchown */
	.long sys_not_exist /* SYS_getpriority */
	.long sys_not_exist /* SYS_setpriority */
	.long sys_not_exist /* SYS_profil */
	.long sys_not_exist /* SYS_statfs */
	.long sys_not_exist /* SYS_fstatfs */
	.long sys_not_exist /* SYS_ioperm */
	.long sys_not_exist /* SYS_socketcall */
	.long sys_not_exist /* SYS_syslog */
	.long sys_setitimer
	.long sys_getitimer
	.long sys_stat
	.long sys_not_exist /* SYS_lstat */
	.long sys_fstat
	.long sys_not_exist /* SYS_olduname */
	.long sys_not_exist /* SYS_iopl */
	.long sys_not_exist /* SYS_vhangup */
	.long sys_not_exist /* SYS_idle */
	.long sys_not_exist /* SYS_vm86old */
	.long sys_not_exist /* SYS_wait4 */
	.long sys_not_exist /* SYS_swapoff */
	.long sys_not_exist /* SYS_sysinfo */
	.long sys_not_exist /* SYS_ipc */
	.long sys_not_exist /* SYS_fsync */
	.long sys_sigreturn
	.long sys_not_exist /* SYS_clone */
	.long sys_not_exist /* SYS_setdomainname */
	.long sys_not_exist /* SYS_uname */
	.long sys_not_exist /* SYS_modify_ldt */
	.long sys_not_exist /* SYS_adjtimex */
	.long sys_not_exist /* SYS_mprotect */
	.long sys_sigprocmask
	.long sys_not_exist /* SYS_create_module */
	.long sys_not_exist /* SYS_init_module */
	.long sys_not_exist /* SYS_delete_module */
	.long sys_not_exist /* SYS_get_kernel_syms */
	.long sys_not_exist /* SYS_quotactl */
	.long sys_getpgid
	.long sys_fchdir
	.long sys_not_exist /* SYS_bdflush */
	.long sys_not_exist /* SYS_sysfs */
	.long sys_not_exist /* SYS_personality */
	.long sys_not_exist /* SYS_afs_syscall */
	.long sys_not_exist /* SYS_setfsuid */
	.long sys_not_exist /* SYS_setfsgid */
	.long sys_not_exist /* SYS__llseek */
	.long sys_getdents
	.long sys_not_exist /* SYS__newselect */
	.long sys_not_exist /* SYS_flock */
	.long sys_not_exist /* SYS_msync */
	.long sys_not_exist /* SYS_readv */
	.long sys_not_exist /* SYS_writev */
	.long sys_getsid
	.long sys_not_exist /* SYS_fdatasync */
	.long sys_not_exist /* SYS__sysctl */
	.long sys_not_exist /* SYS_mlock */
	.long sys_not_exist /* SYS_munlock */
	.long sys_not_exist /* SYS_mlockall */
	.long sys_not_exist /* SYS_munlockall */
	.long sys_not_exist /* SYS_sched_setparam */
	.long sys_not_exist /* SYS_sched_getparam */
	.long sys_not_exist /* SYS_sched_setscheduler */
	.long sys_not_exist /* SYS_sched_getscheduler */
	.long sys_not_exist /* SYS_sched_yield */
	.long sys_not_exist /* SYS_sched_get_priority_max */
	.long sys_not_exist /* SYS_sched_get_priority_min */
	.long sys_not_exist /* SYS_sched_rr_get_interval */
	.long sys_nanosleep
	.long sys_not_exist /* SYS_mremap */
	.long sys_not_exist /* SYS_setresuid */
	.long sys_not_exist /* SYS_getresuid */
	.long sys_not_exist /* SYS_vm86 */
	.long sys_not_exist /* SYS_query_module */
	.long sys_not_exist /* SYS_poll */
	.long sys_not_exist /* SYS_nfsservctl */
	.long sys_not_exist /* SYS_setresgid */
	.long sys_not_exist /* SYS_getresgid */
	.long sys_not_exist /* SYS_prctl */
	.long sys_not_exist /* SYS_rt_sigreturn */
	.long sys_not_exist /* SYS_rt_sigaction */
	.long sys_not_exist /* SYS_rt_sigprocmask */
	.long sys_not_exist /* SYS_rt_sigpending */
	.long sys_not_exist /* SYS_rt_sigtimedwait */
	.long sys_not_exist /* SYS_rt_sigqueueinfo */
	.long sys_not_exist /* SYS_rt_sigsuspend */
	.long sys_not_exist /* SYS_pread64 */
	.long sys_not_exist /* SYS_pwrite64 */
	.long sys_not_exist /* SYS_chown */
	.long sys_not_exist /* SYS_getcwd */
	.long sys_not_exist /* SYS_capget */
	.long sys_not_exist /* SYS_capset */
	.long sys_not_exist /* SYS_sigaltstack */
	.long sys_not_exist /* SYS_sendfile */
	.long sys_not_exist /* SYS_getpmsg */
	.long sys_not_exist /* SYS_putpmsg */
	.long sys_not_exist /* SYS_vfork */
	.long sys_not_exist /* SYS_ugetrlimit */
	.long sys_not_exist /* SYS_mmap2 */
	.long sys_not_exist /* SYS_truncate64 */
	.long sys_not_exist /* SYS_ftruncate64 */
	.long sys_not_exist /* SYS_stat64 */
	.long sys_not_exist /* SYS_lstat64 */
	.long sys_not_exist /* SYS_fstat64 */
	.long sys_not_exist /* SYS_lchown32 */
	.long sys_not_exist /* SYS_getuid32 */
	.long sys_not_exist /* SYS_getgid32 */
	.long sys_not_exist /* SYS_geteuid32 */
	.long sys_not_exist /* SYS_getegid32 */
	.long sys_not_exist /* SYS_setreuid32 */
	.long sys_not_exist /* SYS_setregid32 */
	.long sys_not_exist /* SYS_getgroups32 */
	.long sys_not_exist /* SYS_setgroups32 */
	.long sys_not_exist /* SYS_fchown32 */
	.long sys_not_exist /* SYS_setresuid32 */
	.long sys_not_exist /* SYS_getresuid32 */
	.long sys_not_exist /* SYS_setresgid32 */
	.long sys_not_exist /* SYS_getresgid32 */
	.long sys_not_exist /* SYS_chown32 */
	.long sys_not_exist /* SYS_setuid32 */
	.long sys_not_exist /* SYS_setgid32 */
	.long sys_not_exist /* SYS_setfsuid32 */
	.long sys_not_exist /* SYS_setfsgid32 */
	.long sys_not_exist /* SYS_pivot_root */
	.long sys_not_exist /* SYS_mincore */
	.long sys_not_exist /* SYS_madvise */
	.long sys_not_exist /* SYS_getdents64 */
	.long sys_not_exist /* SYS_fcntl64 */
	.long sys_not_exist
	.long sys_not_exist
	.long sys_not_exist /* SYS_gettid */
	.long sys_not_exist /* SYS_readahead */
	.long sys_not_exist /* SYS_setxattr */
	.long sys_not_exist /* SYS_lsetxattr */
	.long sys_not_exist /* SYS_fsetxattr */
	.long sys_not_exist /* SYS_getxattr */
	.long sys_not_exist /* SYS_lgetxattr */
	.long sys_not_exist /* SYS_fgetxattr */
	.long sys_not_exist /* SYS_listxattr */
	.long sys_not_exist /* SYS_llistxattr */
	.long sys_not_exist /* SYS_flistxattr */
	.long sys_not_exist /* SYS_removexattr */
	.long sys_not_exist /* SYS_lremovexattr */
	.long sys_not_exist /* SYS_fremovexattr */
	.long sys_not_exist /* SYS_tkill */
	.long sys_not_exist /* SYS_sendfile64 */
	.long sys_not_exist /* SYS_futex */
	.long sys_not_exist /* SYS_sched_setaffinity */
	.long sys_not_exist /* SYS_sched_getaffinity */
	.long sys_not_exist /* SYS_set_thread_area */
	.long sys_not_exist /* SYS_get_thread_area */
	.long sys_not_exist /* SYS_io_setup */
	.long sys_not_exist /* SYS_io_destroy */
	.long sys_not_exist /* SYS_io_getevents */
	.long sys_not_exist /* SYS_io_submit */
	.long sys_not_exist /* SYS_io_cancel */
	.long sys_not_exist /* SYS_fadvise64 */
	.long sys_not_exist
	.long sys_not_exist /* SYS_exit_group */
	.long sys_not_exist /* SYS_lookup_dcookie */
	.long sys_not_exist /* SYS_epoll_create */
	.long sys_not_exist /* SYS_epoll_ctl */
	.long sys_not_exist /* SYS_epoll_wait */
	.long sys_not_exist /* SYS_remap_file_pages */
	.long sys_not_exist /* SYS_set_tid_address */
	.long sys_not_exist /* SYS_timer_create */
	.long sys_not_exist /* SYS_timer_settime32 */
	.long sys_not_exist /* SYS_timer_gettime32 */
	.long sys_not_exist /* SYS_timer_getoverrun */
	.long sys_not_exist /* SYS_timer_delete */
	.long sys_not_exist /* SYS_clock_settime32 */
	.long sys_not_exist /* SYS_clock_gettime32 */
	.long sys_not_exist /* SYS_clock_getres_time32 */
	.long sys_not_exist /* SYS_clock_nanosleep_time32 */
	.long sys_not_exist /* SYS_statfs64 */
	.long sys_not_exist /* SYS_fstatfs64 */
	.long sys_not_exist /* SYS_tgkill */
	.long sys_not_exist /* SYS_utimes */
	.long sys_not_exist /* SYS_fadvise64_64 */
	.long sys_not_exist /* SYS_vserver */
	.long sys_not_exist /* SYS_mbind */
	.long sys_not_exist /* SYS_get_mempolicy */
	.long sys_not_exist /* SYS_set_mempolicy */
	.long sys_not_exist /* SYS_mq_open */
	.long sys_not_exist /* SYS_mq_unlink */
	.long sys_not_exist /* SYS_mq_timedsend */
	.long sys_not_exist /* SYS_mq_timedreceive */
	.long sys_not_exist /* SYS_mq_notify */
	.long sys_not_exist /* SYS_mq_getsetattr */
	.long sys_not_exist /* SYS_kexec_load */
	.long sys_not_exist /* SYS_waitid */
	.long sys_not_exist /* SYS_sys_setaltroot */
	.long sys_not_exist /* SYS_add_key */
	.long sys_not_exist /* SYS_request_key */
	.long sys_not_exist /* SYS_keyctl */
	.long sys_not_exist /* SYS_ioprio_set */
	.long sys_not_exist /* SYS_ioprio_get */
	.long sys_not_exist /* SYS_inotify_init */
	.long sys_not_exist /* SYS_inotify_add_watch */
	.long sys_not_exist /* SYS_inotify_rm_watch */
	.long sys_not_exist /* SYS_migrate_pages */
	.long sys_not_exist /* SYS_openat */
	.long sys_not_exist /* SYS_mkdirat */
	.long sys_not_exist /* SYS_mknodat */
	.long sys_not_exist /* SYS_fchownat */
	.long sys_not_exist /* SYS_futimesat */
	.long sys_not_exist /* SYS_fstatat64 */
	.long sys_not_exist /* SYS_unlinkat */
	.long sys_not_exist /* SYS_renameat */
	.long sys_not_exist /* SYS_linkat */
	.long sys_not_exist /* SYS_symlinkat */
	.long sys_not_exist /* SYS_readlinkat */
	.long sys_not_exist /* SYS_fchmodat */
	.long sys_not_exist /* SYS_faccessat */
	.long sys_not_exist /* SYS_pselect6 */
	.long sys_not_exist /* SYS_ppoll */
	.long sys_not_exist /* SYS_unshare */
	.long sys_not_exist /* SYS_set_robust_list */
	.long sys_not_exist /* SYS_get_robust_list */
	.long sys_not_exist /* SYS_splice */
	.long sys_not_exist /* SYS_sync_file_range */
	.long sys_not_exist /* SYS_tee */
	.long sys_not_exist /* SYS_vmsplice */
	.long sys_not_exist /* SYS_move_pages */
	.long sys_not_exist /* SYS_getcpu */
	.long sys_not_exist /* SYS_epoll_pwait */
	.long sys_not_exist /* SYS_utimensat */
	.long sys_not_exist /* SYS_signalfd */
	.long sys_not_exist /* SYS_timerfd_create */
	.long sys_not_exist /* SYS_eventfd */
	.long sys_not_exist /* SYS_fallocate */
	.long sys_not_exist /* SYS_timerfd_settime32 */
	.long sys_not_exist /* SYS_timerfd_gettime32 */
	.long sys_not_exist /* SYS_signalfd4 */
	.long sys_not_exist /* SYS_eventfd2 */
	.long sys_not_exist /* SYS_epoll_create1 */
	.long sys_not_exist /* SYS_dup3 */
	.long sys_not_exist /* SYS_pipe2 */
	.long sys_not_exist /* SYS_inotify_init1 */
	.long sys_not_exist /* SYS_preadv */
	.long sys_not_exist /* SYS_pwritev */
	.long sys_not_exist /* SYS_rt_tgsigqueueinfo */
	.long sys_not_exist /* SYS_perf_event_open */
	.long sys_not_exist /* SYS_recvmmsg */
	.long sys_not_exist /* SYS_fanotify_init */
	.long sys_not_exist /* SYS_fanotify_mark */
	.long sys_not_exist /* SYS_prlimit64 */
	.long sys_not_exist /* SYS_name_to_handle_at */
	.long sys_not_exist /* SYS_open_by_handle_at */
	.long sys_not_exist /* SYS_clock_adjtime */
	.long sys_not_exist /* SYS_syncfs */
	.long sys_not_exist /* SYS_sendmmsg */
	.long sys_not_exist /* SYS_setns */
	.long sys_not_exist /* SYS_process_vm_readv */
	.long sys_not_exist /* SYS_process_vm_writev */
	.long sys_not_exist /* SYS_kcmp */
	.long sys_not_exist /* SYS_finit_module */
	.long sys_not_exist /* SYS_sched_setattr */
	.long sys_not_exist /* SYS_sched_getattr */
	.long sys_not_exist /* SYS_renameat2 */
	.long sys_not_exist /* SYS_seccomp */
	.long sys_not_exist /* SYS_getrandom */
	.long sys_not_exist /* SYS_memfd_create */
	.long sys_not_exist /* SYS_bpf */
	.long sys_not_exist /* SYS_execveat */
	.long sys_not_exist /* SYS_socket */
	.long sys_not_exist /* SYS_socketpair */
	.long sys_not_exist /* SYS_bind */
	.long sys_not_exist /* SYS_connect */
	.long sys_not_exist /* SYS_listen */
	.long sys_not_exist /* SYS_accept4 */
	.long sys_not_exist /* SYS_getsockopt */
	.long sys_not_exist /* SYS_setsockopt */
	.long sys_not_exist /* SYS_getsockname */
	.long sys_not_exist /* SYS_getpeername */
	.long sys_not_exist /* SYS_sendto */
	.long sys_not_exist /* SYS_sendmsg */
	.long sys_not_exist /* SYS_recvfrom */
	.long sys_not_exist /* SYS_recvmsg */
	.long sys_not_exist /* SYS_shutdown */
	.long sys_not_exist /* SYS_userfaultfd */
	.long sys_not_exist /* SYS_membarrier */
	.long sys_not_exist /* SYS_mlock2 */
	.long sys_not_exist /* SYS_copy_file_range */
	.long sys_not_exist /* SYS_preadv2 */
	.long sys_not_exist /* SYS_pwritev2 */
	.long sys_not_exist /* SYS_pkey_mprotect */
	.long sys_not_exist /* SYS_pkey_alloc */
	.long sys_not_exist /* SYS_pkey_free */
	.long sys_not_exist /* SYS_statx */
	.long sys_not_exist /* SYS_arch_prctl */
	.long sys_not_exist /* SYS_io_pgetevents */
	.long sys_not_exist /* SYS_rseq */
	.long sys_not_exist
	.long sys_not_exist
	.long sys_not_exist
	.long sys_not_exist
	.long sys_not_exist
	.long sys_not_exist
	.long sys_not_exist /* SYS_semget */
	.long sys_not_exist /* SYS_semctl */
	.long sys_not_exist /* SYS_shmget */
	.long sys_not_exist /* SYS_shmctl */
	.long sys_not_exist /* SYS_shmat */
	.long sys_not_exist /* SYS_shmdt */
	.long sys_not_exist /* SYS_msgget */
	.long sys_not_exist /* SYS_msgsnd */
	.long sys_not_exist /* SYS_msgrcv */
	.long sys_not_exist /* SYS_msgctl */
	.long sys_not_exist /* SYS_clock_gettime64 */
	.long sys_not_exist /* SYS_clock_settime64 */
	.long sys_not_exist /* SYS_clock_adjtime64 */
	.long sys_not_exist /* SYS_clock_getres_time64 */
	.long sys_not_exist /* SYS_clock_nanosleep_time64 */
	.long sys_not_exist /* SYS_timer_gettime64 */
	.long sys_not_exist /* SYS_timer_settime64 */
	.long sys_not_exist /* SYS_timerfd_gettime64 */
	.long sys_not_exist /* SYS_timerfd_settime64 */
	.long sys_not_exist /* SYS_utimensat_time64 */
	.long sys_not_exist /* SYS_pselect6_time64 */
	.long sys_not_exist /* SYS_ppoll_time64 */
	.long sys_not_exist
	.long sys_not_exist /* SYS_io_pgetevents_time64 */
	.long sys_not_exist /* SYS_recvmmsg_time64 */
	.long sys_not_exist /* SYS_mq_timedsend_time64 */
	.long sys_not_exist /* SYS_mq_timedreceive_time64 */
	.long sys_not_exist /* SYS_semtimedop_time64 */
	.long sys_not_exist /* SYS_rt_sigtimedwait_time64 */
	.long sys_not_exist /* SYS_futex_time64 */
	.long sys_not_exist /* SYS_sched_rr_get_interval_time64 */
	.long sys_not_exist /* SYS_pidfd_send_signal */
	.long sys_not_exist /* SYS_io_uring_setup */
	.long sys_not_exist /* SYS_io_uring_enter */
	.long sys_not_exist /* SYS_io_uring_register */
	.long sys_not_exist /* SYS_open_tree */
	.long sys_not_exist /* SYS_move_mount */
	.long sys_not_exist /* SYS_fsopen */
	.long sys_not_exist /* SYS_fsconfig */
	.long sys_not_exist /* SYS_fsmount */
	.long sys_not_exist /* SYS_fspick */
	.long sys_not_exist /* SYS_pidfd_open */
	.long sys_not_exist /* SYS_clone3 */
	.long sys_not_exist /* SYS_close_range */
	.long sys_not_exist /* SYS_openat2 */
	.long sys_not_exist /* SYS_pidfd_getfd */
	.long sys_not_exist /* SYS_faccessat2 */
	.long sys_not_exist /* SYS_process_madvise */
	.long sys_not_exist /* SYS_epoll_pwait2 */
	.long sys_not_exist /* SYS_mount_setattr */
	.long sys_not_exist /* SYS_quotactl_fd */
	.long sys_not_exist /* SYS_landlock_create_ruleset */
	.long sys_not_exist /* SYS_landlock_add_rule */
	.long sys_not_exist /* SYS_landlock_restrict_self */
	.long sys_not_exist /* SYS_memfd_secret */
	.long sys_not_exist /* SYS_process_mrelease */
	.long sys_not_exist /* SYS_futex_waitv */
	.long sys_not_exist /* SYS_set_mempolicy_home_node */
	.long sys_not_exist /* SYS_cachestat */
	.long sys_not_exist /* SYS_fchmodat2 */
	.long sys_not_exist /* SYS_map_shadow_stack */
	.long sys_not_exist /* SYS_futex_wake */
	.long sys_not_exist /* SYS_futex_wait */
	.long sys_not_exist /* SYS_futex_requeue */
	.long sys_not_exist /* SYS_statmount */
	.long sys_not_exist /* SYS_listmount */
	.long sys_not_exist /* SYS_lsm_get_self_attr */
	.long sys_not_exist /* SYS_lsm_set_self_attr */
	.long sys_not_exist /* SYS_lsm_list_modules */
	.long sys_not_exist /* SYS_mseal */
	.long sys_not_exist /* SYS_setxattrat */
	.long sys_not_exist /* SYS_getxattrat */
	.long sys_not_exist /* SYS_listxattrat */
	.long sys_not_exist /* SYS_removexattrat */
	.long sys_not_exist /* SYS_open_tree_attr */
	.long sys_not_exist /* SYS_file_getattr */
	.long sys_not_exist /* SYS_file_setattr */
	.long sys_not_exist /* SYS_listns */
