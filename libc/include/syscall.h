#ifndef _SYSCALL_H
#define _SYSCALL_H

#define __SYS_brk			 0
#define __SYS_mmap			 1
#define __SYS_munmap		 2
#define __SYS_alarm			 3
#define __SYS_execve		 4
#define __SYS_exit			 5
#define __SYS_fork			 6
#define __SYS_getpid		 7
#define __SYS_getppid		 8
#define __SYS_nanosleep		 9
#define __SYS_pause			10
#define __SYS_waitpid		11
#define __SYS_chdir			12
#define __SYS_fchdir		13
#define __SYS_chroot		14
#define __SYS_close			15
#define __SYS_creat			16
#define __SYS_dup			17
#define __SYS_dup2			18
#define __SYS_getdents		19
#define __SYS_ioctl			20
#define __SYS_link			21
#define __SYS_lseek			22
#define __SYS_mkdir			23
#define __SYS_mknod			24
#define __SYS_mount			25
#define __SYS_open			26
#define __SYS_pipe			27
#define __SYS_read			28
#define __SYS_rename		29
#define __SYS_rmdir			30
#define __SYS_stat			31
#define __SYS_fstat			32
#define __SYS_truncate		33
#define __SYS_ftruncate		34
#define __SYS_umount		35
#define __SYS_unlink		36
#define __SYS_write			37
#define __SYS_time			38
#define __SYS_getitimer		39
#define __SYS_setitimer		40
#define __SYS_getpgid		41
#define __SYS_getpgrp		42
#define __SYS_setpgid		43
#define __SYS_setsid		44
#define __SYS_kill			45
#define __SYS_signal		46
#define __SYS_sigaction		47
#define __SYS_sigsuspend	48
#define __SYS_sigpending	49
#define __SYS_sigprocmask	50
#define __SYS_sigreturn		51
#define __SYS_putchar		52

#define __syscall_return(type, __res) \
do { \
	if (__res >= 0) \
		return (type) __res; \
	errno = -__res; \
	return -1; \
} while (0)

#define _syscall0(type,name) \
type name(void) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__SYS_##name)); \
__syscall_return(type, __res); \
}

#define _syscall1(type,name,type1,arg1) \
type name(type1 arg1) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__SYS_##name),"b" ((long)(arg1))); \
__syscall_return(type, __res); \
}

#define _syscall2(type,name,type1,arg1,type2,arg2) \
type name(type1 arg1,type2 arg2) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__SYS_##name),"b" ((long)(arg1)),"c" ((long)(arg2))); \
__syscall_return(type, __res); \
}

#define _syscall3(type,name,type1,arg1,type2,arg2,type3,arg3) \
type name(type1 arg1,type2 arg2,type3 arg3) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__SYS_##name),"b" ((long)(arg1)),"c" ((long)(arg2)), \
		  "d" ((long)(arg3))); \
__syscall_return(type, __res); \
}

#define _syscall4(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4) \
type name (type1 arg1, type2 arg2, type3 arg3, type4 arg4) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__SYS_##name),"b" ((long)(arg1)),"c" ((long)(arg2)), \
	  "d" ((long)(arg3)),"S" ((long)(arg4))); \
__syscall_return(type, __res); \
}

#define _syscall5(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4, \
	  type5,arg5) \
type name (type1 arg1,type2 arg2,type3 arg3,type4 arg4,type5 arg5) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__SYS_##name),"b" ((long)(arg1)),"c" ((long)(arg2)), \
	  "d" ((long)(arg3)),"S" ((long)(arg4)),"D" ((long)(arg5))); \
__syscall_return(type, __res); \
}

#define __SYS__exit __SYS_exit

#endif /* _SYSCALL_H */
