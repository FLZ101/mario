#include <timer.h>
#include <errno.h>
#include <sched.h>
#include <trap.h>
#include <misc.h>

#include <lib/stddef.h>
#include <lib/string.h>
#include <lib/spinlock.h>

#include <mm/page_alloc.h>

static int last_pid = 0;

spinlock_t last_pid_lock = SPINLOCK_UNLOCKED;

static int get_pid(void)
{
	int res;
	ACQUIRE_LOCK(&last_pid_lock);
	res = ++last_pid;
	RELEASE_LOCK(&last_pid_lock);
	return res;
}

extern void fork_ret(void);
void copy_thread(struct task_struct *p, struct trap_frame *tr)
{
	struct trap_frame *tr0;
	tr0 = (struct trap_frame *)(KSTACK_SIZE + (unsigned long)p) - 1;
	*tr0 = *tr;
	tr0->eax = 0;

	p->thread.esp = (unsigned long)tr0;
	p->thread.esp0 = (unsigned long)(tr0 + 1);

	p->thread.eip = (unsigned long)fork_ret;

	save_segment(fs, p->thread.fs);
	save_segment(gs, p->thread.gs);
}

int do_fork(struct trap_frame *tr)
{
	struct task_struct *p;
	
	if (!(p = alloc_task_struct()))
		return -ENOMEM;

	*p = *current;

	p->state = TASK_UNINTERRUPTIBLE;
	p->pid = get_pid();
	p->run_list.next = NULL;
	p->run_list.prev = NULL;
	p->p_pptr = current;
	p->p_cptr = NULL;
	p->counter = DEF_COUNTER;
	p->signal = 0;
	p->it_real_value = p->it_virt_value = p->it_prof_value = 0;
	p->it_real_incr = p->it_virt_incr = p->it_prof_incr = 0;
	p->real_timer.data = (unsigned long)p;

	copy_thread(p, tr);
	SET_LINKS(p);
	wake_up_process(p);
	return p->pid;
}
