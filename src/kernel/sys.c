
#include <errno.h>
#include <task.h>
#include <sched.h>
#include <resource.h>
#include <utsname.h>
#include <mm/uaccess.h>
#include <syscall.h>

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

void jiffiestotv64(long jiffies, struct timeval64 *value);

int sys_getrusage(int who, struct rusage *usage)
{
	long utime = 0;
	long stime = 0;
	struct rusage r = {0};

	int err = verify_area(VERIFY_WRITE, usage, sizeof(*usage));
	if (err)
		return err;

	switch (who) {
	case RUSAGE_THREAD:
	case RUSAGE_SELF:
		utime = current->utime;
		stime = current->stime;
		break;
	case RUSAGE_CHILDREN:
		utime = current->utime_children;
		stime = current->stime_children;
		break;
	default:
		return -EINVAL;
	}
	jiffiestotv64(utime, &r.ru_utime);
	jiffiestotv64(stime, &r.ru_stime);

	memcpy_tofs(usage, &r, sizeof(r));
	return 0;
}

struct tms {
	clock_t tms_utime;
	clock_t tms_stime;
	clock_t tms_cutime;
	clock_t tms_cstime;
};

int sys_times(struct tms *buf)
{
	struct tms t = {0};

	int err = verify_area(VERIFY_WRITE, buf, sizeof(*buf));
	if (err)
		return err;

	t.tms_utime = current->utime;
	t.tms_stime = current->stime;
	t.tms_cutime = current->utime_children;
	t.tms_cstime = current->stime_children;

	memcpy_tofs(buf, &t, sizeof(t));
	return 0;
}

int sys_uname(struct utsname *buf)
{
	static struct utsname u = {
		"Mario",
		"PC",
		"0.1.0",
		"#1",
		"i386",
		""
	};

	int err = verify_area(VERIFY_WRITE, buf, sizeof(u));
	if (err)
		return err;
	memcpy_tofs(buf, &u, sizeof(u));
	return 0;
}

uid_t sys_getuid(void)
{
	return 0;
}

uid_t sys_getuid32(void)
{
	return 0;
}

uid_t sys_geteuid32(void)
{
	return 0;
}

uid_t sys_setreuid32(void)
{
	return 0;
}

gid_t sys_getgid(void)
{
	return 0;
}

gid_t sys_getgid32(void)
{
	return 0;
}

gid_t sys_getegid32(void)
{
	return 0;
}

gid_t sys_setregid32(void)
{
	return 0;
}

mode_t sys_umask(mode_t mask)
{
	return 0;
}

int sys_not_exist(struct trap_frame tr)
{
	switch (tr.eax) {
	case SYS_ugetrlimit:
	case SYS_prlimit64:
		break;
	default:
		printk("Not implemented: %d\n", tr.eax);
		break;
	}
	return -ENOSYS;
}
