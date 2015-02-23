#ifndef _SCHED_H
#define _SCHED_H

#include <task.h>
#include <time.h>
#include <trap.h>

#include <lib/list.h>

#define REMOVE_LINKS(p) \
do { \
	(p)->next_task->prev_task = (p)->prev_task; \
	(p)->prev_task->next_task = (p)->next_task; \
	if ((p)->p_osptr) \
		(p)->p_osptr->p_ysptr = (p)->p_ysptr; \
	if ((p)->p_ysptr) \
		(p)->p_ysptr->p_osptr = (p)->p_osptr; \
	else \
		(p)->p_pptr->p_cptr = (p)->p_osptr; \
} while (0)

#define SET_LINKS(p) \
do { \
	(p)->next_task = &init_task; \
	(p)->prev_task = init_task.prev_task; \
	init_task.prev_task->next_task = (p); \
	init_task.prev_task = (p); \
	(p)->p_ysptr = NULL; \
	if (((p)->p_osptr = (p)->p_pptr->p_cptr) != NULL) \
		(p)->p_osptr->p_ysptr = p; \
	(p)->p_pptr->p_cptr = p; \
} while (0)

#define DEF_COUNTER	(10*HZ/100)	/* 100 ms time slice */

#define for_each_task(p) \
	for (p = &init_task ; (p = p->next_task) != &init_task ; )

void wake_up_process(struct task_struct *p);

typedef struct {
	spinlock_t lock;
	struct list_head task_list;
} wait_queue_head_t;

int do_fork(struct trap_frame *tr);

extern unsigned long need_resched;

void sched_init(void);

void schedule(void);

#endif	/* _SCHED_H */