#ifndef _SCHED_H
#define _SCHED_H

#include <task.h>
#include <wait.h>
#include <time.h>
#include <trap.h>
#include <misc.h>

#include <lib/list.h>

#define REMOVE_LINKS(p) \
do { \
	irq_save(); \
	(p)->next_task->prev_task = (p)->prev_task; \
	(p)->prev_task->next_task = (p)->next_task; \
	if ((p)->p_osptr) \
		(p)->p_osptr->p_ysptr = (p)->p_ysptr; \
	if ((p)->p_ysptr) \
		(p)->p_ysptr->p_osptr = (p)->p_osptr; \
	else \
		(p)->p_pptr->p_cptr = (p)->p_osptr; \
	irq_restore(); \
} while (0)

#define SET_LINKS(p) \
do { \
	irq_save(); \
	(p)->next_task = &init_task; \
	(p)->prev_task = init_task.prev_task; \
	init_task.prev_task->next_task = (p); \
	init_task.prev_task = (p); \
	(p)->p_ysptr = NULL; \
	if (((p)->p_osptr = (p)->p_pptr->p_cptr) != NULL) \
		(p)->p_osptr->p_ysptr = p; \
	(p)->p_pptr->p_cptr = p; \
	irq_restore(); \
} while (0)

#define DEF_COUNTER	(10*HZ/100)	/* 100 ms time slice */
#define MAX_COUNTER	(20*HZ/100)

#define for_each_task(p) \
	for (p = &init_task; (p = p->next_task) != &init_task; )

void wake_up_process(struct task_struct *p);

int do_fork(struct trap_frame *tr);

extern unsigned long need_resched;

void schedule(void);

void sleep_on(wait_queue_t *q, long state, spinlock_t *lock);

#define sleep_on_interruptible(q) sleep_on((q), TASK_INTERRUPTIBLE, NULL)
#define sleep_on_uninterruptible(q) sleep_on((q), TASK_UNINTERRUPTIBLE, NULL)

void wake_up(wait_queue_t *q, long state);

#define wake_up_all(q) wakeup((q), TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE)
#define wake_up_interruptible(q) wakeup((q), TASK_INTERRUPTIBLE)
#define wake_up_uninterruptible(q) wakeup((q), TASK_UNINTERRUPTIBLE)

void wake_up_1st(wait_queue_t *q);

#endif	/* _SCHED_H */