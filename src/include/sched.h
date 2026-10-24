#ifndef _SCHED_H
#define _SCHED_H

#include <task.h>
#include <wait.h>
#include <time.h>
#include <trap.h>
#include <misc.h>

#include <lib/list.h>

/*
 * Protects the global init_task-linked task list.  SET_LINKS/REMOVE_LINKS used
 * to rely on local interrupt disable; that is insufficient on SMP because two
 * CPUs can insert or reap tasks concurrently.
 */
extern spinlock_t tasklist_lock;

#define REMOVE_LINKS(p) \
do { \
	unsigned long __tasklist_flags; \
	spin_lock_irqsave(&tasklist_lock, __tasklist_flags); \
	(p)->next_task->prev_task = (p)->prev_task; \
	(p)->prev_task->next_task = (p)->next_task; \
	if ((p)->p_osptr) \
		(p)->p_osptr->p_ysptr = (p)->p_ysptr; \
	if ((p)->p_ysptr) \
		(p)->p_ysptr->p_osptr = (p)->p_osptr; \
	else \
		(p)->p_pptr->p_cptr = (p)->p_osptr; \
	spin_unlock_irqrestore(&tasklist_lock, __tasklist_flags); \
} while (0)

#define SET_LINKS(p) \
do { \
	unsigned long __tasklist_flags; \
	spin_lock_irqsave(&tasklist_lock, __tasklist_flags); \
	(p)->next_task = &init_task; \
	(p)->prev_task = init_task.prev_task; \
	init_task.prev_task->next_task = (p); \
	init_task.prev_task = (p); \
	(p)->p_ysptr = NULL; \
	if (((p)->p_osptr = (p)->p_pptr->p_cptr) != NULL) \
		(p)->p_osptr->p_ysptr = p; \
	(p)->p_pptr->p_cptr = p; \
	spin_unlock_irqrestore(&tasklist_lock, __tasklist_flags); \
} while (0)

#define DEF_COUNTER	(10*HZ/100)	/* 100 ms time slice */
#define MAX_COUNTER	(20*HZ/100)

#define for_each_task(p) \
	for (p = &init_task; (p = p->next_task) != &init_task; )

void sched_init_smp(void);
void ap_cpu_idle(void);
int sched_set_cpus_allowed(struct task_struct *p, unsigned long mask);
void wake_up_process(struct task_struct *p);

void schedule(void);

long schedule_timeout(long timeout);

void sleep_on(wait_queue_t *q, long state, spinlock_t *lock);

#define sleep_on_interruptible(q) sleep_on((q), TASK_INTERRUPTIBLE, NULL)
#define sleep_on_uninterruptible(q) sleep_on((q), TASK_UNINTERRUPTIBLE, NULL)

void wake_up(wait_queue_t *q, long state);

#define wake_up_all(q) wake_up((q), TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE)
#define wake_up_interruptible(q) wake_up((q), TASK_INTERRUPTIBLE)
#define wake_up_uninterruptible(q) wake_up((q), TASK_UNINTERRUPTIBLE)

void wake_up_1st(wait_queue_t *q);

void do_exit(long code);
#endif	/* _SCHED_H */
