#ifndef _TIME_H
#define _TIME_H

#include <syscall.h>
#include <sys/types.h>

struct timespec {
	long	tv_sec;		/* seconds */
	long	tv_nsec;	/* nanoseconds */
};

struct timeval {
	long tv_sec;
	long tv_usec;
};

struct itimerval {
	struct	timeval it_interval;
	struct	timeval it_value;
};

#define	ITIMER_REAL	0
#define	ITIMER_VIRTUAL	1
#define	ITIMER_PROF	2

static inline _syscall2(int,nanosleep,const struct timespec *,req,struct timespec *,rem)

time_t time(time_t *tloc);

static inline _syscall2(int,getitimer,int,which, struct itimerval *,curr_value)
static inline _syscall3(int,setitimer,int, which, const struct itimerval *, new_value,struct itimerval *, old_value)

#endif /* _TIME_H */
