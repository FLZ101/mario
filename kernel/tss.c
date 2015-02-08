#include <task.h>
#include <misc.h>
#include <tss.h>

struct tss_struct init_tss = {
	0, 0,
	(unsigned long)&init_task_union + KSTACK_SIZE,
	KERNEL_DS, 0
};