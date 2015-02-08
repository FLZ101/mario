#ifndef _TASK_H
#define _TASK_H

#include <misc.h>

#define KSTACK_SIZE	2*PAGE_SIZE

struct task_struct {
	int pid;
};

union task_union {
	struct task_struct task;
	unsigned long stack[KSTACK_SIZE/sizeof(long)];
};

extern union task_union init_task_union;

#endif	/* _TASK_H */