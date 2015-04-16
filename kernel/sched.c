#include <signal.h>
#include <sched.h>
#include <timer.h>
#include <misc.h>
#include <tss.h>

#include <lib/spinlock.h>
#include <lib/stddef.h>
#include <lib/list.h>

static LIST_HEAD(runqueue_head);

void print_runqueue(void)
{
	struct list_head *tmp;
	struct task_struct *p;

	list_for_each(tmp, &runqueue_head) {
		p = list_entry(tmp, struct task_struct, run_list);
		early_print("%x ", p);
	}
	early_print("\n");
}

static void in_runqueue(struct task_struct *p)
{
	irq_save();
	list_add(&p->run_list, &runqueue_head);
	irq_restore();
}

static void out_runqueue(struct task_struct *p)
{
	irq_save();
	list_del(&p->run_list);
	p->run_list.next = NULL;
	irq_restore();
}

static int task_on_runqueue(struct task_struct *p)
{
	return (p->run_list.next != NULL);
}

void wake_up_process(struct task_struct *p)
{
	if (!p)
		return;
	
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
	int c;
	struct list_head *tmp;
	struct task_struct *p, *next;

	if (current->state != TASK_RUNNING)
		out_runqueue(current);

	irq_save();
	/*
	 * init_task wouldn't handle signal
	 */
	for_each_task(p) {
		if ((p->state == TASK_INTERRUPTIBLE) && signal_pending(p))
			wake_up_process(p);
	}

	c = -100;
	next = &init_task;
	list_for_each(tmp, &runqueue_head) {
		p = list_entry(tmp, struct task_struct, run_list);
		if (p->counter > c)
			c = p->counter, next = p;
	}

	if (c)
		goto tail;

	for_each_task(p) {
		p->counter = p->counter/2 + DEF_COUNTER;
		if (p->counter > MAX_COUNTER)
			p->counter = MAX_COUNTER;
	}

tail:
	irq_restore();

	need_resched = 0;

	if (next != current)
		switch_to(next);
}

static void process_timeout(unsigned long data)
{
	print_runqueue();
	wake_up_process((struct task_struct *)data);
	print_runqueue();
}

/*
 * @timeout:	a proper value
 */
long schedule_timeout(long timeout)
{
	struct timer_list timer;
	init_timer(&timer);
	timer.expires = timeout;
	timer.data = (unsigned long)current;
	timer.fun = process_timeout;

	current->state = TASK_INTERRUPTIBLE;
	add_timer(&timer);
	schedule();
	del_timer(&timer);

	return timer.expires;
}

int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

/*
 * @lock:	used by <void down(struct semaphore *sem)>
 * @state:	TASK_INTERRUPTIBLE or TASK_UNINTERRUPTIBLE
 */
void sleep_on(wait_queue_t *q, long state, spinlock_t *lock)
{
	wait_queue_node_t node;
	init_wait_queue_node(&node, current);

	current->state = state;
	in_wait_queue(q, &node);

	if (lock)
		RELEASE_LOCK(lock);

	schedule();
	out_wait_queue(q, &node);
}

void wake_up(wait_queue_t *q, long state)
{
	struct list_head *tmp, *head = &q->task_list;

	list_for_each(tmp, head) {
		wait_queue_node_t *node = 
			list_entry(tmp, wait_queue_node_t, task_list);

		if (node->p->state & state) {
			out_wait_queue(q, node);
			wake_up_process(node->p);
		}
	}
}

/*
 * This ugly function is used by <void up(struct semaphore *sem)>
 */
void wake_up_1st(wait_queue_t *q)
{
	struct list_head *head = &q->task_list;
	if (list_empty(head))
		return;

	wait_queue_node_t *node = 
		list_entry(head->next, wait_queue_node_t, task_list);

	out_wait_queue(q, node);
	wake_up_process(node->p);
}