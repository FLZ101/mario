
#include <errno.h>
#include <task.h>
#include <sched.h>
#include <resource.h>
#include <mm/uaccess.h>

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

int sys_getrlimit(unsigned int resource, struct rlimit *rlim)
{
	int error;

	if (resource >= RLIM_NLIMITS)
		return -EINVAL;
	error = verify_area(VERIFY_WRITE,rlim,sizeof *rlim);
	if (error)
		return error;
	memcpy_tofs(rlim, current->rlim + resource, sizeof(*rlim));
	return 0;
}

int sys_setrlimit(unsigned int resource, struct rlimit *rlim)
{
	struct rlimit new_rlim, *old_rlim;
	int err;

	if (resource >= RLIM_NLIMITS)
		return -EINVAL;
	err = verify_area(VERIFY_READ, rlim, sizeof(*rlim));
	if (err)
		return err;
	memcpy_fromfs(&new_rlim, rlim, sizeof(*rlim));
	old_rlim = current->rlim + resource;
	if (new_rlim.rlim_cur > old_rlim->rlim_max || new_rlim.rlim_max > old_rlim->rlim_max)
		return -EPERM;
	*old_rlim = new_rlim;
	return 0;
}

uid_t sys_getuid(void)
{
	return 0;
}

uid_t sys_geteuid32(void)
{
	return 0;
}

gid_t sys_getgid(void)
{
	return 0;
}

int sys_not_exist(struct trap_frame tr)
{
	printk("Not implemented: %d\n", tr.eax);
	return -ENOSYS;
}
