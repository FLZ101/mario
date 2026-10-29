
#include <errno.h>
#include <task.h>
#include <sched.h>
#include <resource.h>
#include <utsname.h>
#include <version.h>
#include <mm/uaccess.h>
#include <mm/page_alloc.h>
#include <syscall.h>

int sys_getpgid(pid_t pid)
{
	struct task_struct * p;
	unsigned long flags;
	int pgrp = -ESRCH;

	if (!pid)
		return current->pgrp;

	spin_lock_irqsave(&tasklist_lock, flags);
	for_each_task(p) {
		if (p->pid == pid) {
			pgrp = p->pgrp;
			break;
		}
	}
	spin_unlock_irqrestore(&tasklist_lock, flags);
	return pgrp;
}

int sys_getsid(void)
{
	return current->session;
}

int sys_setpgid(pid_t pid, pid_t pgid)
{
	struct task_struct *p;
	unsigned long flags;
	int ret = -ESRCH;
	int found = 0;

	if (!pid)
		pid = current->pid;
	if (!pgid)
		pgid = pid;
	if (pgid < 0)
		return -EINVAL;

	spin_lock_irqsave(&tasklist_lock, flags);
	for_each_task(p) {
		if (p->pid == pid) {
			found = 1;
			break;
		}
	}
	if (!found)
		goto out;

	if (p->p_pptr == current) {
		if (p->session != current->session)
			goto eperm;
		if (p->did_exec)
			goto eacces;
	} else if (p != current)
		goto out;
	if (p->leader)
		goto eperm;
	if (pgid != pid) {
		struct task_struct * tmp;
		int found = 0;

		for_each_task (tmp) {
			if (tmp->pgrp == pgid && tmp->session == current->session) {
				found = 1;
				break;
			}
		}
		if (!found)
			goto eperm;
	}

	p->pgrp = pgid;
	ret = 0;
	goto out;

eperm:
	ret = -EPERM;
	goto out;
eacces:
	ret = -EACCES;
out:
	spin_unlock_irqrestore(&tasklist_lock, flags);
	return ret;
}

int sys_setsid(void)
{
	if (current->leader)
		return -EPERM;
	current->tty = NULL;
	current->leader = 1;
	current->session = current->pgrp = current->pid;
	return current->pgrp;
}

int sys_getpid(void)
{
	return current->tgid;
}

int sys_getppid(void)
{
	return current->p_pptr->pid;
}

int sys_getpriority(int which, int who)
{
	struct task_struct *p;
	unsigned long flags;
	unsigned long pflags;
	int found = 0;
	int best = 0;

	if (which != PRIO_PROCESS && which != PRIO_PGRP &&
	    which != PRIO_USER)
		return -EINVAL;

	spin_lock_irqsave(&tasklist_lock, flags);
	if (which == PRIO_PROCESS) {
		if (!who)
			who = current->pid;
		for_each_task(p) {
			if (p->pid == who) {
				int prio;

				spin_lock_irqsave(&p->lock, pflags);
				prio = NZERO - p->priority;
				spin_unlock_irqrestore(&p->lock, pflags);
				if (prio > best)
					best = prio;
				found = 1;
			}
		}
	} else if (which == PRIO_PGRP) {
		if (!who)
			who = current->pgrp;
		for_each_task(p) {
			if (p->pgrp == who) {
				int prio;

				spin_lock_irqsave(&p->lock, pflags);
				prio = NZERO - p->priority;
				spin_unlock_irqrestore(&p->lock, pflags);
				if (prio > best)
					best = prio;
				found = 1;
			}
		}
	} else {
		if (who)
			goto esrch;
		for_each_task(p) {
			int prio;

			spin_lock_irqsave(&p->lock, pflags);
			prio = NZERO - p->priority;
			spin_unlock_irqrestore(&p->lock, pflags);
			if (prio > best)
				best = prio;
			found = 1;
		}
	}

	if (!found)
		goto esrch;
	spin_unlock_irqrestore(&tasklist_lock, flags);
	return best;

esrch:
	spin_unlock_irqrestore(&tasklist_lock, flags);
	return -ESRCH;
}

int sys_setpriority(int which, int who, int prio)
{
	struct task_struct *p;
	unsigned long flags;
	unsigned long pflags;
	int found = 0;

	if (which != PRIO_PROCESS && which != PRIO_PGRP &&
	    which != PRIO_USER)
		return -EINVAL;
	if (prio < PRIO_MIN || prio > PRIO_MAX)
		prio = prio < PRIO_MIN ? PRIO_MIN : PRIO_MAX;

	spin_lock_irqsave(&tasklist_lock, flags);
	if (which == PRIO_PROCESS) {
		if (!who)
			who = current->pid;
		for_each_task(p) {
			if (p->pid == who) {
				spin_lock_irqsave(&p->lock, pflags);
				p->priority = prio;
				spin_unlock_irqrestore(&p->lock, pflags);
				found = 1;
			}
		}
	} else if (which == PRIO_PGRP) {
		if (!who)
			who = current->pgrp;
		for_each_task(p) {
			if (p->pgrp == who) {
				spin_lock_irqsave(&p->lock, pflags);
				p->priority = prio;
				spin_unlock_irqrestore(&p->lock, pflags);
				found = 1;
			}
		}
	} else {
		if (who)
			goto esrch;
		for_each_task(p) {
			spin_lock_irqsave(&p->lock, pflags);
			p->priority = prio;
			spin_unlock_irqrestore(&p->lock, pflags);
			found = 1;
		}
	}

	if (!found)
		goto esrch;
	spin_unlock_irqrestore(&tasklist_lock, flags);
	return 0;

esrch:
	spin_unlock_irqrestore(&tasklist_lock, flags);
	return -ESRCH;
}

int sys_gettid(void)
{
	return current->pid;
}

int sys_sched_setaffinity(pid_t pid, size_t len, unsigned long *user_mask)
{
	struct task_struct *p;
	unsigned long flags;
	unsigned long mask;
	int error;
	int found = 0;

	if (len < sizeof(mask))
		return -EINVAL;

	if (!pid)
		pid = current->pid;

	spin_lock_irqsave(&tasklist_lock, flags);
	for_each_task(p) {
		if (p->pid == pid) {
			found = 1;
			break;
		}
	}
	if (!found)
		goto esrch;

	error = verify_area(VERIFY_READ, user_mask, sizeof(mask));
	if (error)
		goto out;
	mask = get_fs_long(user_mask);

	/* Reject offline CPUs and completely empty masks. */
	if (!mask || (mask & ~online_cpu_mask())) {
		error = -EINVAL;
		goto out;
	}
	error = sched_set_cpus_allowed(p, mask);
	goto out;

esrch:
	error = -ESRCH;
out:
	spin_unlock_irqrestore(&tasklist_lock, flags);
	return error;
}

int sys_sched_getaffinity(pid_t pid, size_t len, unsigned long *user_mask)
{
	struct task_struct *p;
	unsigned long flags;
	unsigned long pflags;
	unsigned long mask = 1;
	size_t i;
	int error;
	int found = 0;

	if (len < sizeof(mask))
		return -EINVAL;

	if (!pid)
		pid = current->pid;

	spin_lock_irqsave(&tasklist_lock, flags);
	for_each_task(p) {
		if (p->pid == pid) {
			found = 1;
			break;
		}
	}
	if (!found) {
		spin_unlock_irqrestore(&tasklist_lock, flags);
		return -ESRCH;
	}

	/* Snapshot cpus_allowed before dropping the task-list lock. */
	spin_lock_irqsave(&p->lock, pflags);
	mask = p->cpus_allowed;
	spin_unlock_irqrestore(&p->lock, pflags);
	spin_unlock_irqrestore(&tasklist_lock, flags);

	error = verify_area(VERIFY_WRITE, user_mask, len);
	if (error)
		return error;

	/* Report the task's own affinity mask rather than a hard-coded CPU 0. */
	put_fs_long(mask, user_mask);
	for (i = sizeof(mask); i < len; i++)
		put_fs_byte(0, (char *)user_mask + i);
	return sizeof(mask);
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
		MARIO_OS_NAME,
		MARIO_NODENAME,
		MARIO_KERNEL_RELEASE,
		MARIO_KERNEL_VERSION,
		MARIO_MACHINE,
		MARIO_DOMAINNAME,
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

int sys_getgroups32(int size, gid_t *list)
{
	/* No supplementary group support yet; the primary group is already
	 * available through getgid()/getegid(). */
	(void)list;
	if (size < 0)
		return -EINVAL;
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
		.totalram = max_pfn * PAGE_SIZE,
		.freeram = (unsigned long)get_free_area_size() * PAGE_SIZE,
		.mem_unit = 1
	};
	memcpy_tofs(info, &k, sizeof(k));
	return 0;
}

int sys_not_exist(struct trap_frame tr)
{
	switch (tr.eax) {
	case SYS_setfsuid32:
	case SYS_setfsgid32:
		return 0;
	default:
		pr_warn("Not implemented: %d\n", tr.eax);
		break;
	}
	return -ENOSYS;
}
