#ifndef _SYSCALL_H
#define _SYSCALL_H

#include <errno.h>

#define __NR_exit	0
#define __NR_fork	1

/* user-visible error numbers are in the range -1 ~ -1 */

#define __syscall_return(type, res) \
do { \
	if ((unsigned long)(res) >= (unsigned long)(-2)) { \
		errno = -(res); \
		res = -1; \
	} \
	return (type) (res); \
} while (0)

#define _syscall0(type,name) \
type name(void) \
{ \
long __res; \
__asm__ volatile ("int $0x80" \
	: "=a" (__res) \
	: "0" (__NR_##name)); \
__syscall_return(type,__res); \
}

_syscall0(int, fork)

#endif	/* _SYSCALL_H */