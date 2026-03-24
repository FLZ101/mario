#ifndef _RESOURCE_H
#define _RESOURCE_H

#include <syscall.h>

typedef unsigned long long rlim_t;

struct rlimit {
	rlim_t	rlim_cur;
	rlim_t	rlim_max;
};

#define RLIMIT_DATA		0	/* maximum data size */
#define RLIMIT_STACK	1	/* maximum stack size */
#define RLIMIT_NLIMITS	2

#define RLIM_NLIMITS RLIMIT_NLIMITS

_syscall2(int,getrlimit,int,resource,struct rlimit *,rlim)
_syscall2(int,setrlimit,int,resource,struct rlimit *,rlim)

#endif	/* _RESOURCE_H */
