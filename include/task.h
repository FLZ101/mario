#ifndef _TASK_H
#define _TASK_H

#include <errno.h>
#include <misc.h>

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

struct task_struct {
	volatile long state;
	unsigned long counter;
	
	int exit_code;

	unsigned long pid;

	struct list_head run_list;
	struct task_struct *next_task, *prev_task;
	/*
	 * pointers to parent, youngest child, younger sibling, 
	 * older sibling, respectively.
	 */
	struct task_struct *p_pptr, *p_cptr, *p_ysptr, *p_osptr;

	struct mm_struct *mm;
	struct thread_struct thread;
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