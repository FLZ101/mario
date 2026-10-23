#include <signal.h>
#include <sched.h>
#include <timer.h>
#include <misc.h>
#include <tss.h>
#include <ldt.h>

#include <lib/spinlock.h>
#include <lib/stddef.h>
#include <lib/list.h>

#include <mm/mm.h>
#include <syscall_trace.h>

static LIST_HEAD(runqueue_head);

/*
 * Global task-list lock used by SET_LINKS/REMOVE_LINKS in <sched.h>.  It is
 * deliberately separate from runqueue and wait-queue locks so process creation
 * and reaping do not create an unnecessary lock-order dependency.
 */
spinlock_t tasklist_lock = SPINLOCK_UNLOCKED;

void print_runqueue(void)
{
	struct list_head *tmp;
	struct task_struct *p;

	irq_save();
	list_for_each(tmp, &runqueue_head) {
		p = list_entry(tmp, struct task_struct, run_list);
		pr_debug("%d-", p->pid);
	}
	irq_restore();
}

static void in_runqueue(struct task_struct *p)
{
	list_add(&p->run_list, &runqueue_head);
}

static void out_runqueue(struct task_struct *p)
{
	list_del(&p->run_list);
	p->run_list.prev = NULL;
	p->run_list.next = NULL;
}

static int task_on_runqueue(struct task_struct *p)
{
	return (p->run_list.next != NULL);
}

void wake_up_process(struct task_struct *p)
{
	if (!p)
		return;

	irq_save();
	p->state = TASK_RUNNING;
	if (!task_on_runqueue(p))
		in_runqueue(p);
	irq_restore();
}

void FASTCALL _switch_to(struct task_struct *p, struct task_struct *n)
{
	/*
	 * `current` inside this function refers to the next task, since kernel
	 * stack has been changed
	 */
	assert(current == n);

	struct thread_struct *prev = &p->thread;
	struct thread_struct *next = &n->thread;

	init_tss.esp0 = next->esp0;
	save_segment(fs, prev->fs);
	load_segment(fs, next->fs);

	// TLS
	struct user_desc *ud = n->thread.user_descs;
	for (int idx = 0; idx < NR_USER_DESC; ++idx) {
		if (user_desc_zero(ud + idx)) {
			zero_desc(gdt + GDT_ENTRY_TLS_MIN_IDX + idx);
		} else {
			fill_desc(gdt + GDT_ENTRY_TLS_MIN_IDX + idx, ud + idx);
		}
	}
	save_segment(gs, prev->gs);
	load_segment(gs, next->gs);
}

void switch_to(struct task_struct *next)
{
	switch_pd(next->mm->pd);
	__asm__ __volatile__(
		/* make sure eflags unchanged when we back */
		"pushfl\n\t"
		"pushl %%esi\n\t"
		"pushl %%edi\n\t"
		"pushl %%ebp\n\t"
		"pushl %%eax\n\t"
		"pushl %%ebx\n\t"
		"movl %%esp, %0\n\t"	/* save esp */
		"movl %2, %%esp\n\t"	/* restore esp */
		"movl $1f, %1\n\t"	/* save eip */
		"pushl %3\n\t"		/* restore eip */
		"jmp _switch_to\n"
		"1:\n\t"
		"popl %%ebx\n\t"
		"popl %%eax\n\t"
		"popl %%ebp\n\t"
		"popl %%edi\n\t"
		"popl %%esi\n\t"
		"popfl"
		:"=m"(current->thread.esp), "=m"(current->thread.eip)
		:"m"(next->thread.esp), "m"(next->thread.eip),
		"c"(current), "d"(next));
}

void schedule(void)
{
	int c;
	struct list_head *tmp;
	struct task_struct *p, *next;

	irq_save();
	if (current->state != TASK_RUNNING && current != &init_task)
		out_runqueue(current);
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
		p->counter = DEF_COUNTER;
	}

tail:
	current->need_resched = 0;
	if (next != current) {
		syscall_trace_context_switch_out();
		switch_to(next);
		syscall_trace_context_switch_in();
	} else {
		irq_restore();
	}
}

static void process_timeout(unsigned long data)
{
	wake_up_process((struct task_struct *)data);
}

/*
 * @timeout:	a proper value in jiffies
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
	return -ERESTARTNOHAND;
}

/*
 * @lock:	used by <void down(struct semaphore *sem)>
 * @state:	TASK_INTERRUPTIBLE or TASK_UNINTERRUPTIBLE
 */
void sleep_on(wait_queue_t *q, long state, spinlock_t *lock)
{
	if (current->pid == 0)
		hang("task 0 is trying to sleep");

	wait_queue_node_t node;
	init_wait_queue_node(&node, current);

	irq_save();
	current->state = state;
	in_wait_queue(q, &node);

	if (lock)
		RELEASE_LOCK(lock);
	irq_restore();

	schedule();

	out_wait_queue(q, &node);
}

void wake_up(wait_queue_t *q, long state)
{
	struct list_head *tmp, *head;
	unsigned long flags;

	spin_lock_irqsave(&wait_queue_lock, flags);

	head = &q->task_list;
	list_for_each(tmp, head) {
		wait_queue_node_t *node =
			list_entry(tmp, wait_queue_node_t, task_list);

		if (node->p->state & state)
			wake_up_process(node->p);
	}

	spin_unlock_irqrestore(&wait_queue_lock, flags);
}

/*
 * This ugly function is used by <void up(struct semaphore *sem)>
 */
void wake_up_1st(wait_queue_t *q)
{
	struct list_head *head;
	unsigned long flags;

	spin_lock_irqsave(&wait_queue_lock, flags);
	if (list_empty(&q->task_list))
		goto tail;
	head = &q->task_list;
	wait_queue_node_t *node =
		list_entry(head->next, wait_queue_node_t, task_list);

	wake_up_process(node->p);
tail:
	spin_unlock_irqrestore(&wait_queue_lock, flags);
}
