#ifndef _TIME_H
#define _TIME_H

#define HZ 100

void time_init(void);

extern volatile long jiffies;

struct timeval {
	long tv_sec;
	long tv_usec;
};

#endif	/* _TIME_H */