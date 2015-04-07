#ifndef _TIMER_H
#define _TIMER_H

#include <time.h>

#include <lib/stddef.h>
#include <lib/list.h>

struct timer_list {
	struct list_head list;
	long expires;
	unsigned long data;
	void (*fun)(unsigned long data);
};

static inline void init_timer(struct timer_list *timer)
{
	timer->list.next = NULL;
}

void add_timer(struct timer_list *timer);

int del_timer(struct timer_list *timer);

void run_timer_list(void);

struct itimerval {
	struct	timeval it_interval;
	struct	timeval it_value;
};

#define	ITIMER_REAL	0
#define	ITIMER_VIRTUAL	1
#define	ITIMER_PROF	2

void it_real_fun(unsigned long data);
int _setitimer(int which, struct itimerval *value, struct itimerval *ovalue);

#endif	/* _TIMER_H */