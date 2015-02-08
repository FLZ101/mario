#include <task.h>
#include <misc.h>

union task_union init_task_union
	__attribute__((__section__(".init_task_union"))) = 
	{{0}};