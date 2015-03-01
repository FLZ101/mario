#include <errno.h>
#include <sched.h>
#include <trap.h>
#include <misc.h>

#include <lib/stddef.h>
#include <lib/string.h>

#include <mm/page_alloc.h>

unsigned long last_pid = 0;

static int get_pid(void)
{
	return ++last_pid;
}

#define new_stack() (pages_alloc(1) + 8192)

extern void fork_ret(void);
void copy_thread(struct task_struct *p, struct trap_frame *tr)
{
	struct trap_frame *tr0;
	tr0 = (struct trap_frame *)(KSTACK_SIZE + (unsigned long)p) - 1;
	*tr0 = *tr;
	tr0->eax = 0;
	/*
	 * we need to allocate a user stack for the new task, because 
	 * COW is not implemented now
	 */
	tr0->esp = new_stack();

	p->thread.esp = (unsigned long)tr0;
	p->thread.esp0 = (unsigned long)(tr0 + 1);

	p->thread.eip = (unsigned long)fork_ret;

	save_segment(fs, p->thread.fs);
	save_segment(gs, p->thread.gs);
}

int do_fork(struct trap_frame *tr)
{
	struct task_struct *p = alloc_task_struct();
	if (!p)
		return -ENOMEM;

	*p = *current;

	p->state = TASK_UNINTERRUPTIBLE;
	p->pid = get_pid();
	p->run_list.next = NULL;
	p->run_list.prev = NULL;
	p->p_pptr = current;
	p->p_cptr = NULL;
	p->counter = DEF_COUNTER;
	copy_thread(p, tr);
	SET_LINKS(p);
	wake_up_process(p);
	return p->pid;
}
