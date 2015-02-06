#include <schedule.h>
#include <misc.h>
#include <tss.h>

union task_union init_task_union
	__attribute__((__section__(".init_task_union"))) = 
	{{0}};

struct tss_struct init_tss = {
	0, 0,
	sizeof(init_stack) + (unsigned long) &init_stack,
	KERNEL_DS, 0
};