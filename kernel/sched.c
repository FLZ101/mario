#include <sched.h>
#include <misc.h>
#include <tss.h>

#include <lib/spinlock.h>
#include <lib/stddef.h>
#include <lib/list.h>

static LIST_HEAD(runqueue_head);
static spinlock_t runqueue_lock;

#define in_runqueue(p) list_add(&(p)->run_list, &runqueue_head)
#define out_runqueue(p) list_del(&(p)->run_list, &runqueue_head)

void runqueue_print(void)
{
	struct list_head *pos;
	early_print("TASK: ");
	list_for_each(pos, &runqueue_head) {
		early_print("%x\n", (unsigned long)list_entry(pos, struct task_struct, run_list));
	}
}

void sched_init(void)
{
	INIT_LOCK(&runqueue_lock);
}

void wake_up_process(struct task_struct *p)
{
	p->state = TASK_RUNNING;
	ACQUIRE_LOCK(&runqueue_lock);
	in_runqueue(p);
	RELEASE_LOCK(&runqueue_lock);
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
