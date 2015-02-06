#ifndef _SCHEDULE_H
#define _SCHEDULE_H

struct task_struct {
	int pid;
};

#define INIT_TASK_SIZE	2048*sizeof(long)

union task_union {
	struct task_struct task;
	unsigned long stack[INIT_TASK_SIZE/sizeof(long)];
};

extern union task_union init_task_union;

#define init_task init_task_union.task
#define init_stack init_task_union.stack

#endif	/* _SCHEDULE_H */