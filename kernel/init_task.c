#include <sched.h>
#include <misc.h>

union task_union init_task_union __attribute__((__section__(".init_task_union"))) = 
{{
	state: 0, 
	counter: DEF_COUNTER, 
	pid: 0,
	run_list: LIST_HEAD_INIT(init_task.run_list),
	next_task: &init_task,
	prev_task: &init_task,
	p_pptr: &init_task,
	thread: {(unsigned long)&init_task + KSTACK_SIZE, 0}
}};