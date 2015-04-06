#include <sched.h>
#include <misc.h>
#include <tss.h>

#include <lib/spinlock.h>
#include <lib/stddef.h>
#include <lib/list.h>

static LIST_HEAD(runqueue_head);

void in_runqueue(struct task_struct *p)
{
	irq_save();
	list_add(&p->run_list, &runqueue_head);
	irq_restore();
}

void out_runqueue(struct task_struct *p)
{
	irq_save();
	list_del(&p->run_list);
	p->run_list.next = NULL;
	irq_restore();
}

int task_on_runqueue(struct task_struct *p)
{
	return (p->run_list.next != NULL);
}

void wake_up_process(struct task_struct *p)
{
	if (!task_on_runqueue(p)) {
		p->state = TASK_RUNNING;
		in_runqueue(p);
	}
}

void FASTCALL _switch_to(struct task_struct *p, struct task_struct *n)
{
	struct thread_struct *prev = &p->thread;
	struct thread_struct *next = &n->thread;

	init_tss.esp0 = next->esp0;
	save_segment(fs, prev->fs);
	save_segment(gs, prev->gs);
	load_segment(fs, next->fs);
	load_segment(gs, next->gs);
}

#define switch_to(next) \
do { \
	__asm__ __volatile__( \
		"pushl %%ebp\n\t" \
		"movl %%esp, %0\n\t" \
		"movl %2, %%esp\n\t" \
		"movl $1f, %1\n\t" \
		"pushl %3\n\t" \
		"jmp @_switch_to@8\n\t" \
		"1:\n" \
		"popl %%ebp" \
		:"=m"(current->thread.esp), "=m"(current->thread.eip) \
		:"m"(next->thread.esp), "m"(next->thread.eip), \
		"c"(current), "d"(next)); \
} while (0)

unsigned long need_resched = 0;

void schedule(void)
{
	struct task_struct *prev = current;
	struct task_struct *next = &init_task;
	struct list_head *tmp;

	if (prev == next)
		tmp = &runqueue_head;
	else
		tmp = prev->run_list.next;

	if (tmp == &runqueue_head)
		tmp = tmp->next;
	if (tmp != &runqueue_head)
		next = list_entry(tmp, struct task_struct, run_list);

	next->counter = DEF_COUNTER;

	need_resched = 0;
	
	if (next != prev)
		switch_to(next);
}

int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

/*
 * The ugly @lock is used by <void down(struct semaphore *sem)>
 */
void sleep_on(wait_queue_t *q, long state, spinlock_t *lock)
{
	wait_queue_node_t node;
	init_wait_queue_node(&node, current);

	out_runqueue(current);
	current->state = state;
	in_wait_queue(q, &node);

	if (lock)
		RELEASE_LOCK(lock);

	schedule();
	out_wait_queue(&node);
}

void wake_up(wait_queue_t *q, long state)
{
	struct list_head *tmp, *head = &q->task_list;

	list_for_each(tmp, head) {
		wait_queue_node_t *node = 
			list_entry(tmp, wait_queue_node_t, task_list);

		if (node->p->state & state)
			wake_up_process(node->p);
	}
}

/*
 * This ugly function is used by <void up(struct semaphore *sem)>
 */
void wake_up_1st(wait_queue_t *q, long state)
{
	struct list_head *tmp, *head = &q->task_list;

	list_for_each(tmp, head) {
		wait_queue_node_t *node = 
			list_entry(tmp, wait_queue_node_t, task_list);

		if (node->p->state & state) {
			wake_up_process(node->p);
			break;
		}
	}
}