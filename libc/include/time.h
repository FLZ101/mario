#ifndef _TIME_H
#define _TIME_H

#include <syscall.h>

struct timespec {
	long	tv_sec;		/* seconds */
	long	tv_nsec;	/* nanoseconds */
};

static inline _syscall2(int,nanosleep,const struct timespec *,req,struct timespec *,rem)

#endif /* _TIME_H */
