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

	/* Save the user selectors from the trap frame, not the kernel %gs. */
	p->thread.fs = tr->fs;
	p->thread.gs = tr->gs;
}

extern int copy_page_tables(struct task_struct *p);
extern void free_page_tables(struct task_struct *p);
extern int dup_mmap(struct task_struct *p);

static int copy_mm(struct task_struct *p)
{
	*(p->mm) = *(current->mm);
	/* The copied lock state is meaningless; start the new mm unlocked. */
	INIT_LOCK(&p->mm->mm_lock);

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

int do_fork(struct trap_frame *tr, int vfork)
{
	struct task_struct *p;
	if (!(p = alloc_task_struct())) {
		pr_task_err("fork failed: no memory\n");
		return -ENOMEM;
	}
	*p = *current;
	/* The child must not inherit a transient scheduler-lock state. */
	INIT_LOCK(&p->lock);
	p->syscall_trace_start = 0;
	p->syscall_trace_running = 0;
	p->syscall_trace_last_tsc = 0;
	p->syscall_trace_active = 0;

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
	p->did_vfork = 0;
	p->leader = 0;

	p->cpu = current->cpu;
	p->cpus_allowed = online_cpu_mask();
	p->run_list.next = NULL;
	p->run_list.prev = NULL;
	p->p_pptr = current;
	p->p_cptr = NULL;

	p->counter = (current->counter + 1) >> 1;
	current->counter >>= 1;
	if (!current->counter)
		current->need_resched = 1;

	p->signal = 0;
	p->exit_signal = SIGCHLD; // !!!
	p->it_real_value = p->it_virt_value = p->it_prof_value = 0;
	p->it_real_incr = p->it_virt_incr = p->it_prof_incr = 0;
	p->real_timer.data = (unsigned long)p;

	p->stime = p->utime = p->stime_children = p->utime_children = 0;

	copy_thread(p, tr);
	if (copy_mm(p))
		goto fail_4;
	copy_files(p);
	copy_fs(p);
	SET_LINKS(p);

	if (vfork) {
		wait_queue_node_t node;

		current->did_vfork = 1;
		/*
		 * Queue the parent before making the child runnable.  On SMP the
		 * child can run on another CPU immediately, call exec/_exit, and
		 * wake wait_chldexit before the parent is queued; that lost
		 * wakeup would leave the parent sleeping forever.
		 */
		init_wait_queue_node(&node, current);
		irq_save();
		current->state = TASK_UNINTERRUPTIBLE;
		in_wait_queue(&current->wait_chldexit, &node);
		irq_restore();
		wake_up_process(p);
		schedule();
		out_wait_queue(&current->wait_chldexit, &node);
	} else {
		wake_up_process(p);
	}
	current->did_vfork = 0;
	pr_task_debug("fork -> %d%s\n", p->pid,
		      vfork ? " (vfork)" : "");
	return p->pid;
fail_4:
	kfree(p->files);
fail_3:
	kfree(p->fs);
fail_2:
	kfree(p->mm);
fail_1:
	free_task_struct(p);
	pr_task_err("fork failed: no memory\n");
	return -ENOMEM;
}

int sys_fork(struct trap_frame tr)
{
	return do_fork(&tr, 0);
}

int sys_vfork(struct trap_frame tr)
{
	return do_fork(&tr, 1);
}
