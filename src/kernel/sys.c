
#include <errno.h>
#include <task.h>
#include <sched.h>

int sys_getpgid(pid_t pid)
{
	struct task_struct * p;

	if (!pid)
		return current->pgrp;

	for_each_task(p) {
		if (p->pid == pid)
			return p->pgrp;
	}
	return -ESRCH;
}

int sys_getsid(void)
{
	return current->session;
}

int sys_setpgid(pid_t pid, pid_t pgid)
{
	struct task_struct *p;

	if (!pid)
		pid = current->pid;
	if (!pgid)
		pgid = pid;
	if (pgid < 0)
		return -EINVAL;
	for_each_task(p) {
		if (p->pid == pid)
			goto found_task;
	}
	return -ESRCH;

found_task:
	if (p->p_pptr == current) {
		if (p->session != current->session)
			return -EPERM;
		if (p->did_exec)
			return -EACCES;
	} else if (p != current)
		return -ESRCH;
	if (p->leader)
		return -EPERM;
	if (pgid != pid) {
		struct task_struct * tmp;
		for_each_task (tmp) {
			if (tmp->pgrp == pgid && tmp->session == current->session)
				goto ok_pgid;
		}
		return -EPERM;
	}

ok_pgid:
	p->pgrp = pgid;
	return 0;
}

int sys_setsid(void)
{
	if (current->leader)
		return -EPERM;
	current->leader = 1;
	current->session = current->pgrp = current->pid;
	return current->pgrp;
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->p_pptr->pid;
}
