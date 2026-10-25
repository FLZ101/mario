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

static struct list_head runqueue_head[NR_CPUS];
static spinlock_t runqueue_lock[NR_CPUS];
/*
 * One scheduler lock deliberately serialises all runqueue and task-state
 * transitions in this SMP bring-up phase.  It is held across switch_to() and
 * released by _switch_to() only after the old task's kernel stack has been
 * switched away.  That closes the migration window where a task is no longer
 * on its old runqueue but is still executing on that CPU, which could
 * otherwise let another CPU start the same task on a second stack.
 */
static spinlock_t scheduler_lock = SPINLOCK_UNLOCKED;

extern void notify_parent(struct task_struct *p);

/*
 * Global task-list lock used by SET_LINKS/REMOVE_LINKS in <sched.h>.  It is
 * deliberately separate from runqueue and wait-queue locks so process creation
 * and reaping do not create an unnecessary lock-order dependency.
 */
spinlock_t tasklist_lock = SPINLOCK_UNLOCKED;

/* Initialize per-CPU runqueues before APs may enter schedule(). */
void sched_init_smp(void)
{
	for (int cpu = 0; cpu < NR_CPUS; cpu++) {
		INIT_LIST_HEAD(&runqueue_head[cpu]);
		INIT_LOCK(&runqueue_lock[cpu]);
	}
	INIT_LOCK(&scheduler_lock);
}

void print_runqueue(void)
{
	struct list_head *tmp;
	struct task_struct *p;
	unsigned long flags;

	spin_lock_irqsave(&scheduler_lock, flags);
	for (int cpu = 0; cpu < nr_cpu_ids; cpu++) {
		list_for_each(tmp, &runqueue_head[cpu]) {
			p = list_entry(tmp, struct task_struct, run_list);
			pr_debug("cpu%d:%d-", cpu, p->pid);
		}
	}
	spin_unlock_irqrestore(&scheduler_lock, flags);
}

static int task_on_runqueue(struct task_struct *p)
{
	return (p->run_list.next != NULL);
}

static void __enqueue_task(int cpu, struct task_struct *p)
{
	list_add_tail(&p->run_list, &runqueue_head[cpu]);
}

static void __dequeue_task(struct task_struct *p)
{
	list_del(&p->run_list);
	p->run_list.prev = NULL;
	p->run_list.next = NULL;
}

/*
 * Pick the least-loaded online CPU allowed by the task's affinity mask.
 * Migrating a task to a remote CPU is what makes the runqueues useful without
 * an APIC timer: new work spreads across CPUs at fork/wakeup time.
 */
static int select_task_cpu(struct task_struct *p)
{
	unsigned long allowed = p->cpus_allowed & online_cpu_mask();
	int best = -1;
	int best_len = NR_CPUS + 1;

	if (!allowed)
		allowed = 1;

	for (int cpu = 0; cpu < nr_cpu_ids; cpu++) {
		int len = 0;
		struct list_head *tmp;

		if (!(allowed & (1UL << cpu)))
			continue;

		/* scheduler_lock is held by every caller of this helper. */
		list_for_each(tmp, &runqueue_head[cpu])
			len++;

		if (len < best_len) {
			best_len = len;
			best = cpu;
		}
	}

	if (best < 0)
		best = p->cpu;
	return best;
}

void wake_up_process(struct task_struct *p)
{
	unsigned long flags;
	int target = -1;
	int self;

	if (!p)
		return;

	self = current->cpu;

	spin_lock_irqsave(&scheduler_lock, flags);
	if (p->state != TASK_RUNNING) {
		/* Mark it runnable first.  If it is already linked on a runqueue
		 * (for example a task that set TASK_INTERRUPTIBLE but has not yet
		 * reached schedule), leave it there; its CPU will continue it. */
		p->state = TASK_RUNNING;
		if (!task_on_runqueue(p)) {
			target = select_task_cpu(p);
			p->cpu = target;
			__enqueue_task(target, p);
		}
	} else if (!task_on_runqueue(p)) {
		/* A runnable task must always be either running on a CPU (and
		 * still linked) or queued.  Repair the invariant if a transition
		 * ever leaves it unlinked. */
		target = p->cpu;
		__enqueue_task(target, p);
	}
	spin_unlock_irqrestore(&scheduler_lock, flags);

	/*
	 * A remote CPU may be halted in its idle loop.  Kick it with a
	 * reschedule IPI; a local wakeup only needs need_resched set.
	 */
	if (target >= 0 && target != self)
		smp_send_reschedule(target);
	else if (target == self)
		current->need_resched = 1;
}

void FASTCALL _switch_to(struct task_struct *p, struct task_struct *n)
{
	struct thread_struct *prev = &p->thread;
	struct thread_struct *next = &n->thread;
	struct tss_struct *tss;

	/*
	 * The inline switch already changed the kernel stack.  Publish the new
	 * task in the destination CPU's slot first so the assertion below (and
	 * any C code on this stack) observes current == n.
	 */
	cpu_info[n->cpu].current_task = n;
	assert(current == n);

	/* Every CPU must update its own ring-0 stack pointer. */
	tss = (n->cpu == 0) ? &init_tss : &ap_tss[n->cpu];
	tss->esp0 = next->esp0;

	/*
	 * %fs is still the user segment used by get_user/put_user, so it must
	 * follow the task across kernel context switches.  %gs is the per-CPU
	 * segment and is deliberately not touched here.
	 */
	save_segment(fs, prev->fs);
	load_segment(fs, next->fs);

	/*
	 * AP GDTs are per-CPU copies.  Update TLS descriptors in the GDT that is
	 * actually installed on this CPU, not always the BSP's global table.
	 */
	struct desc_struct *g = cpu_gdt(n->cpu);
	struct user_desc *ud = n->thread.user_descs;
	for (int idx = 0; idx < NR_USER_DESC; ++idx) {
		if (user_desc_zero(ud + idx))
			zero_desc(g + GDT_ENTRY_TLS_MIN_IDX + idx);
		else
			fill_desc(g + GDT_ENTRY_TLS_MIN_IDX + idx, ud + idx);
	}

	/*
	 * schedule() holds the scheduler lock across switch_to().  Only after
	 * this point is the previous task's kernel stack safely parked in
	 * prev->thread, so another CPU may now be allowed to wake and run it.
	 */
	spin_unlock(&scheduler_lock);

	/*
	 * If the previous task has become a zombie, its parent must not be
	 * woken while the task is still running.  Waiters scan the child state
	 * and may reap/free the task immediately, so wake the parent only now
	 * that the old task is safely parked and no longer executing.
	 */
	if (p->state == TASK_ZOMBIE) {
		p->parked = 1;
		notify_parent(p);
	}
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
	int cpu = current->cpu;
	struct list_head *rq = &runqueue_head[cpu];
	struct task_struct *idle = smp_idle_task(cpu);
	struct task_struct *p, *next;
	struct list_head *tmp;
	int c;
	int current_woke = 0;

	irq_save();
	/*
	 * tasklist_lock is ordered before scheduler_lock so syscalls that find a
	 * task under tasklist_lock can safely update its scheduling state (for
	 * example sched_setaffinity()).  The scheduler still drops tasklist_lock
	 * before switch_to(), which releases scheduler_lock.
	 */
	spin_lock(&tasklist_lock);
	spin_lock(&scheduler_lock);

	/*
	 * Resolve the outgoing task first.  If it slept but already has a
	 * pending signal, keep it runnable (and therefore linked on this CPU's
	 * runqueue); otherwise unlink it before choosing the next task.
	 */
	spin_lock(&current->lock);
	if (current->state != TASK_RUNNING && current != idle) {
		if (current->state == TASK_INTERRUPTIBLE &&
		    signal_pending(current))
			current->state = TASK_RUNNING;
		else if (task_on_runqueue(current))
			__dequeue_task(current);
	}
	spin_unlock(&current->lock);

	/*
	 * Wake any fully sleeping interruptible task that has a signal pending.
	 * scheduler_lock makes this safe against concurrent wake_up_process()
	 * calls and against the same task being switched out on another CPU.
	 * current is deliberately handled above rather than here.
	 */
	for_each_task(p) {
		/*
		 * Never wake and enqueue the task that called schedule() from this
		 * loop.  It is still executing on this CPU and will either be parked
		 * below or relinked on this CPU by the re-check that follows.  Enqueueing
		 * it here could place it on a remote runqueue while it is still running
		 * locally, which would let two CPUs execute the same task concurrently.
		 */
		if (p == current)
			continue;
		if (p->state != TASK_INTERRUPTIBLE)
			continue;

		spin_lock(&p->lock);
		if (p->state != TASK_INTERRUPTIBLE || !signal_pending(p)) {
			spin_unlock(&p->lock);
			continue;
		}
		p->state = TASK_RUNNING;
		spin_unlock(&p->lock);
		if (!task_on_runqueue(p)) {
			int target = select_task_cpu(p);

			p->cpu = target;
			__enqueue_task(target, p);
			if (target != cpu)
				smp_send_reschedule(target);
		}
	}

	/*
	 * A signal can arrive for current after the outgoing-state check above but
	 * before the runqueue scan.  Re-check current under scheduler_lock so an
	 * interruptible sleeper with a newly pending signal stays runnable and is
	 * relinked on this CPU, never on another CPU.
	 */
	spin_lock(&current->lock);
	if (current->state == TASK_INTERRUPTIBLE && signal_pending(current)) {
		current->state = TASK_RUNNING;
		current_woke = 1;
	}
	spin_unlock(&current->lock);
	if (current_woke && !task_on_runqueue(current))
		__enqueue_task(cpu, current);

	c = -100;
	next = idle;
	list_for_each(tmp, rq) {
		p = list_entry(tmp, struct task_struct, run_list);
		if (p->counter > c) {
			c = p->counter;
			next = p;
		}
	}

	if (!c)
		for_each_task(p)
			p->counter = DEF_COUNTER;

	spin_unlock(&tasklist_lock);

	current->need_resched = 0;
	if (next != current) {
		syscall_trace_context_switch_out();
		/* _switch_to() releases scheduler_lock once current is parked. */
		switch_to(next);
		syscall_trace_context_switch_in();
	} else {
		spin_unlock(&scheduler_lock);
		irq_restore();
	}
}

int sched_set_cpus_allowed(struct task_struct *p, unsigned long mask)
{
	unsigned long allowed = mask & online_cpu_mask();
	unsigned long flags;

	if (!allowed)
		return -EINVAL;

	/*
	 * Keep migration explicit and safe for this step: a running task cannot
	 * be atomically moved off the CPU it is executing on without a
	 * stop-machine style handoff.  Accept masks that include the current CPU,
	 * and defer migration of sleeping tasks to their next wakeup.
	 */
	if (p == current && !(allowed & (1UL << p->cpu)))
		return -EINVAL;

	spin_lock_irqsave(&scheduler_lock, flags);
	spin_lock(&p->lock);
	p->cpus_allowed = allowed;
	if (!(allowed & (1UL << p->cpu)) && p->state != TASK_RUNNING) {
		for (int cpu = 0; cpu < nr_cpu_ids; cpu++)
			if (allowed & (1UL << cpu)) {
				p->cpu = cpu;
				break;
			}
	}
	spin_unlock(&p->lock);
	spin_unlock_irqrestore(&scheduler_lock, flags);
	return 0;
}

void ap_cpu_idle(void)
{
	for (;;) {
		if (current->need_resched) {
			schedule();
			continue;
		}

		/*
		 * APs do not have the BSP PIT tick.  Halt with interrupts enabled
		 * so a wakeup IPI can resume this CPU and set need_resched.
		 */
		__asm__ __volatile__("sti; hlt");
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
