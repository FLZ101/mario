#ifndef _TASK_H
#define _TASK_H

#include <errno.h>
#include <timer.h>
#include <misc.h>
#include <types.h>
#include <wait.h>
#include <signal.h>
#include <resource.h>

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

#define NR_RLIMIT	2

struct fs_struct;
struct files_struct;
struct wait_queue;
struct task_struct {
	volatile long state;
	int counter;
	int priority;
	unsigned long signal;
	unsigned long blocked;
	long need_resched;
	int exit_code, exit_signal;
	pid_t pid, pgrp, session;
	int leader, did_exec;

	long it_real_value, it_prof_value, it_virt_value;
	long it_real_incr, it_prof_incr, it_virt_incr;
	struct timer_list real_timer;

	struct list_head run_list;
	struct task_struct *next_task, *prev_task;
	/*
	 * pointers to parent, youngest child, younger sibling, 
	 * older sibling, respectively.
	 */
	struct task_struct *p_pptr, *p_cptr, *p_ysptr, *p_osptr;

	spinlock_t lock;
	wait_queue_t wait_chldexit;
	struct rlimit rlim[NR_RLIMIT];
	struct mm_struct *mm;
	struct fs_struct *fs;
	struct files_struct *files;
	struct thread_struct thread;
	struct sigaction sigaction[32];
	char comm[16];
};

#define TASK_RUNNING		0
#define TASK_INTERRUPTIBLE	1
#define TASK_UNINTERRUPTIBLE	2
#define TASK_ZOMBIE		4
#define TASK_STOPPED	8

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
