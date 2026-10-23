#include <wait.h>
#include <misc.h>

/*
 * Wait queues are touched from the timer interrupt, syscall paths, and future
 * reschedule IPIs.  A real lock is therefore needed; interrupt masking alone
 * cannot exclude work on another CPU.
 */
spinlock_t wait_queue_lock = SPINLOCK_UNLOCKED;

void in_wait_queue(wait_queue_t *q, wait_queue_node_t *n)
{
	unsigned long flags;

	spin_lock_irqsave(&wait_queue_lock, flags);
	list_add_tail(&n->task_list, &q->task_list);
	spin_unlock_irqrestore(&wait_queue_lock, flags);
}

void out_wait_queue(wait_queue_t *q, wait_queue_node_t *n)
{
	unsigned long flags;

	spin_lock_irqsave(&wait_queue_lock, flags);
	if (!list_empty(&n->task_list))
		list_del_init(&n->task_list);
	spin_unlock_irqrestore(&wait_queue_lock, flags);
}
