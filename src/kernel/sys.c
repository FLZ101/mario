
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

int sys_gettid(void)
{
	return current->pid;
}

int sys_prlimit64(pid_t pid, int resource, const struct rlimit *new, struct rlimit *old)
{
	int error;

	if (resource >= RLIM_NLIMITS)
		return -EINVAL;

	if (old) {
		error = verify_area(VERIFY_WRITE, old, sizeof(*old));
		if (error)
			return error;
		memcpy_tofs(old, current->rlim + resource, sizeof(*old));
	}
	if (new) {
		struct rlimit knew, *kold;

		error = verify_area(VERIFY_READ, new, sizeof(*new));
		if (error)
			return error;
		memcpy_fromfs(&knew, new, sizeof(*new));

		kold = current->rlim + resource;
		if (knew.rlim_cur > kold->rlim_max || knew.rlim_max > kold->rlim_max)
			return -EPERM;
		*kold = knew;
	}
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

struct sysinfo {
	unsigned long uptime;
	unsigned long loads[3];
	unsigned long totalram;
	unsigned long freeram;
	unsigned long sharedram;
	unsigned long bufferram;
	unsigned long totalswap;
	unsigned long freeswap;
	unsigned short procs, pad;
	unsigned long totalhigh;
	unsigned long freehigh;
	unsigned mem_unit;
	char __reserved[256];
};

extern volatile time_t boot_time_sec;

int sys_sysinfo(struct sysinfo *info)
{
	int err = verify_area(VERIFY_WRITE, info, sizeof(*info));
	if (err)
		return err;

	struct sysinfo k = {
		.uptime = boot_time_sec,
		.mem_unit = 1
	};
	memcpy_tofs(info, &k, sizeof(k));
	return 0;
}

int sys_not_exist(struct trap_frame tr)
{
	switch (tr.eax) {
	default:
		printk("Not implemented: %d\n", tr.eax);
		break;
	}
	return -ENOSYS;
}
