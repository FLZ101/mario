#ifndef _TIME_H
#define _TIME_H

#include <types.h>

#define HZ 100

void time_init(void);

struct timespec {
	long	tv_sec;		/* seconds */
	long	tv_nsec;	/* nanoseconds */
};

struct timespec64 {
	time_t	tv_sec;		/* seconds */
	long	tv_nsec;	/* nanoseconds */
};

struct timeval {
	long tv_sec;
	long tv_usec;
};

struct timeval64 {
	time_t tv_sec;
	suseconds_t tv_usec;
};

struct itimerval {
	struct	timeval it_interval;
	struct	timeval it_value;
};

struct itimerval64 {
	struct	timeval64 it_interval;
	struct	timeval64 it_value;
};

#define CLOCK_REALTIME           0 // real (i.e., wall-clock) time
#define CLOCK_MONOTONIC          1 // A nonsettable system-wide clock that represents monotonic time
                                   // since — as described by POSIX — "some unspecified point in the past".
								   // On Linux, that point corresponds to the number of seconds that
								   // the system has been running since it was booted.
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define CLOCK_REALTIME_ALARM     8
#define CLOCK_BOOTTIME_ALARM     9
#define CLOCK_SGI_CYCLE         10
#define CLOCK_TAI               11

extern volatile long jiffies;

#define	ITIMER_REAL	0
#define	ITIMER_VIRTUAL	1
#define	ITIMER_PROF	2

#endif	/* _TIME_H */
