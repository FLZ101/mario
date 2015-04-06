#ifndef _TIMER_H
#define _TIMER_H

#include <lib/list.h>

struct timer_list {
	struct list_head list;
	unsigned long expires;
	unsigned long data;
	void (*fun)(unsigned long data);
};

static inline void init_timer(struct timer_list *timer)
{
	INIT_LIST_HEAD(&timer->list);
}

void add_timer(struct timer_list *timer);

void del_timer(struct timer_list *timer);

void run_timer_list(void);

#endif	/* _TIMER_H */