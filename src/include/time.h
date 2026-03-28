#ifndef _TIME_H
#define _TIME_H

#define HZ 100

void time_init(void);

struct timespec {
	long	tv_sec;		/* seconds */
	long	tv_nsec;	/* nanoseconds */
};

extern volatile long jiffies;

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

#endif	/* _TIME_H */
