#include <timer.h>
#include <errno.h>
#include <sched.h>
#include <trap.h>
#include <misc.h>

#include <lib/stddef.h>
#include <lib/string.h>
#include <lib/spinlock.h>

#include <mm/page_alloc.h>
#include <mm/kmalloc.h>

#include <fs/fs.h>

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

extern int copy_page_tables(struct task_struct *p);
extern void free_page_tables(struct task_struct *p);
extern int dup_mmap(struct task_struct *p);

static int copy_mm(struct task_struct *p)
{
	*(p->mm) = *(current->mm);

	if (copy_page_tables(p))
		return -ENOMEM;
	if (dup_mmap(p)) {
		free_page_tables(p);
		return -ENOMEM;
	}
	return 0;
}

extern void copy_files(struct task_struct *p);
extern void copy_fs(struct task_struct *p);

int do_fork(struct trap_frame *tr)
{
	struct task_struct *p;
	
	if (!(p = alloc_task_struct()))
		return -ENOMEM;
	*p = *current;

	p->mm = (struct mm_struct *)kmalloc(sizeof(struct mm_struct));
	if (!p->mm)
		goto fail_1;
	p->fs = (struct fs_struct *)kmalloc(sizeof(struct fs_struct));
	if (!p->fs)
		goto fail_2;
	p->files = (struct files_struct *)kmalloc(sizeof(struct files_struct));
	if (!p->files)
		goto fail_3;

	init_wait_queue(&p->wait_chldexit);
	p->state = TASK_UNINTERRUPTIBLE;
	p->pid = get_pid();
	p->did_exec = 0;
	p->leader = 0;

	p->run_list.next = NULL;
	p->run_list.prev = NULL;
	p->p_pptr = current;
	p->p_cptr = NULL;

	p->counter = (current->counter + 1) >> 1;
	current->counter >>= 1;
	if (!current->counter)
		current->need_resched = 1;

	p->signal = 0;
	p->it_real_value = p->it_virt_value = p->it_prof_value = 0;
	p->it_real_incr = p->it_virt_incr = p->it_prof_incr = 0;
	p->real_timer.data = (unsigned long)p;

	copy_thread(p, tr);
	if (copy_mm(p))
		goto fail_4;
	copy_files(p);
	copy_fs(p);
	SET_LINKS(p);
	wake_up_process(p);
	return p->pid;
fail_4:
	kfree(p->files);
fail_3:
	kfree(p->fs);
fail_2:
	kfree(p->mm);
fail_1:
	free_task_struct(p);
	return -ENOMEM;
}

int sys_fork(struct trap_frame tr)
{
	return do_fork(&tr);
}
