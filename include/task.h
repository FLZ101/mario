#ifndef _TASK_H
#define _TASK_H

#include <errno.h>
#include <timer.h>
#include <misc.h>
#include <types.h>

#include <mm/page_alloc.h>
#include <mm/mm.h>

#include <lib/spinlock.h>

struct thread_struct {
	unsigned long esp0;
	unsigned long eip;
	unsigned long esp;
	unsigned long fs;
	unsigned long gs;

	unsigned long cr2, trap_no, error_code;
};

struct rlimit {
	long	rlim_cur;
	long	rlim_max;
};

#define RLIMIT_DATA	0	/* maximum data size */
#define RLIMIT_STACK	1	/* maximum stack size */

#define NR_RLIMIT	2

struct fs_struct;
struct files_struct;
struct task_struct {
	volatile long state;
	int counter;
	int priority;

	unsigned long signal;
	unsigned long blocked;

	int exit_code;
	struct rlimit rlim[NR_RLIMIT];
	pid_t pid;

	struct list_head run_list;
	struct task_struct *next_task, *prev_task;
	/*
	 * pointers to parent, youngest child, younger sibling, 
	 * older sibling, respectively.
	 */
	struct task_struct *p_pptr, *p_cptr, *p_ysptr, *p_osptr;

	long it_real_value, it_prof_value, it_virt_value;
	long it_real_incr, it_prof_incr, it_virt_incr;
	struct timer_list real_timer;

	struct mm_struct *mm;
	struct fs_struct *fs;
	struct files_struct *files;
	struct thread_struct thread;
	spinlock_t lock;
};

#define TASK_RUNNING		0
#define TASK_INTERRUPTIBLE	1
#define TASK_UNINTERRUPTIBLE	2
#define TASK_ZOMBIE		4
#define TASK_STOPPED		8

#define alloc_task_struct() ((struct task_struct *)pages_alloc(1))
#define free_task_struct(p) pages_free((unsigned long)(p), 1)

#define KSTACK_SIZE (2*PAGE_SIZE)

union task_union {
	struct task_struct task;
	unsigned long stack[KSTACK_SIZE/sizeof(long)];
};

extern union task_union init_task_union;

#define init_task (init_task_union.task)

static inline struct task_struct *get_current(void)
{
	struct task_struct *current;
	__asm__("andl %%esp, %0" : "=r"(current) : "0"(~8191UL));
	return current;
}

#define current get_current()

#endif	/* _TASK_H */