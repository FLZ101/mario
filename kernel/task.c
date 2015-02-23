#include <sched.h>
#include <trap.h>

int sys_fork(struct trap_frame tr)
{
	return do_fork(&tr);
}