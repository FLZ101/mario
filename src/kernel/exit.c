#include <sched.h>
#include <signal.h>
#include <misc.h>
#include <futex.h>

#include <syscall_trace.h>

#include <errno.h>

#include <mm/uaccess.h>
#include <mm/mm.h>

extern void free_page_tables(struct task_struct *);
extern void exit_mmap(struct task_struct *);
extern void exit_files(void);
extern void exit_fs(void);

int send_sig(unsigned long sig, struct task_struct *p, int priv)
{
	if (!p || sig > 32)
		return -EINVAL;
	if (!sig)
		return 0;

	if (p->state == TASK_ZOMBIE)
		return 0;
	if ((sig == SIGKILL) || (sig == SIGCONT)) {
		if (p->state == TASK_STOPPED)
			p->state = TASK_RUNNING;
		p->exit_code = 0;
		p->signal &= ~( (1<<(SIGSTOP-1)) | (1<<(SIGTSTP-1)) |
				(1<<(SIGTTIN-1)) | (1<<(SIGTTOU-1)) );
	}
	/* Depends on order SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU */
	if ((sig >= SIGSTOP) && (sig <= SIGTTOU))
		p->signal &= ~(1<<(SIGCONT-1));

	unsigned long mask = 1 << (sig-1);
	struct sigaction *sa = p->sigaction + (sig-1);

	/* don't bother with ignored signals (but SIGCHLD is special) */
	if (sa->sa_handler == SIG_IGN && sig != SIGCHLD)
		return 0;
	/* some signals are ignored by default.. (but SIGCONT already did its deed) */
	if ((sa->sa_handler == SIG_DFL) &&
	    (sig == SIGCONT || sig == SIGCHLD || sig == SIGWINCH))
		return 0;
	p->signal |= mask;
	return 0;
}

/*
 * kill_pg() sends a signal to a process group: this is what the tty
 * control characters do (^C, ^Z etc)
 */
int kill_pg(int pgrp, int sig, int priv)
{
	struct task_struct *p;
	int err, res = -ESRCH;
	int cnt = 0;

	if (sig < 0 || sig > 32 || pgrp <= 0)
		return -EINVAL;
	for_each_task(p) {
		if (p->pgrp == pgrp) {
			if ((err = send_sig(sig, p, priv)) != 0)
				res = err;
			else
				cnt++;
		}
	}
	return cnt ? 0 : res;
}

/*
 * kill_sl() sends a signal to the session leader: this is used
 * to send SIGHUP to the controlling process of a terminal when
 * the connection is lost.
 */
int kill_sl(int session, int sig, int priv)
{
	struct task_struct *p;
	int err, res = -ESRCH;
	int cnt = 0;

	if (sig < 0 || sig > 32 || session <= 0)
		return -EINVAL;
	for_each_task(p) {
		if (p->session == session && p->leader) {
			if ((err = send_sig(sig, p, priv)) != 0)
				res = err;
			else
				cnt++;
		}
	}
	return cnt ? 0 : res;
}

int kill_proc(int pid, int sig, int priv)
{
	struct task_struct *p;

	if (sig < 0 || sig > 32)
		return -EINVAL;
	for_each_task(p) {
		if (p->pid == pid)
			return send_sig(sig, p, priv);
	}
	return -ESRCH;
}

int sys_kill(int pid, int sig)
{
	int err, res = 0, cnt = 0;

	if (!pid)
		return kill_pg(current->pgrp, sig, 0);
	if (pid == -1) {
		struct task_struct *p;
		for_each_task(p) {
			if (p->pid > 1 && p != current) {
				cnt++;
				if ((err = send_sig(sig, p, 0)) != -EPERM)
					res = err;
			}
		}
		return cnt ? res : -ESRCH;
	}
	if (pid < 0)
		return kill_pg(-pid, sig, 0);
	return kill_proc(pid, sig, 0);
}

/*
 * Orphaned process groups are not to be affected by terminal-generated stop signals.
 * Newly orphaned process groups are to receive a SIGHUP and a SIGCONT.
 */
int is_orphaned_pgrp(int pgrp)
{
	struct task_struct *p;

	for_each_task(p) {
		if ((p->pgrp != pgrp) ||
				(p->state == TASK_ZOMBIE) ||
					(p->p_pptr->pid == 1))
			continue;
		if ((p->p_pptr->pgrp != pgrp) &&
				(p->p_pptr->session == p->session))
			return 0;
	}
	return 1;
}

static int has_stopped_jobs(int pgrp)
{
	struct task_struct * p;

	for_each_task(p) {
		if (p->pgrp != pgrp)
			continue;
		if (p->state == TASK_STOPPED)
			return 1;
	}
	return 0;
}

void notify_parent(struct task_struct *p)
{
	if (p->p_pptr == init_task.next_task)
		p->exit_signal = SIGCHLD;
	send_sig(p->exit_signal, p->p_pptr, 1);
	wake_up_all(&p->p_pptr->wait_chldexit); // The parent may be blocking due to vfork
}

void do_exit(long code)
{
	struct task_struct *p;

	/*
	 * init_task doesn't exit
	 */
	if (current == &init_task)
		return;

	if (current == init_task.next_task) {
		hang("init exits with %d", code);
	}

	/*
	 * Publish thread death before releasing the address space.  For a
	 * process leader this address may live in the process mm that
	 * free_page_tables() is about to unmap, so writing it after page-table
	 * teardown would fault on a no-longer-mapped user page.  For a non-leader
	 * thread this is also the futex that wakes pthread_join(); the group
	 * leader waits for those threads to reach TASK_ZOMBIE below before
	 * tearing down the shared mm.
	 */
	if (current->clear_child_tid) {
		put_fs_long(0, current->clear_child_tid);
		futex_wake(current->clear_child_tid, 1);
		current->clear_child_tid = NULL;
	}

	/*
	 * CLONE_VM/FILES/FS tasks share these structures with their group
	 * leader.  A non-leader thread exit must only drop its reference; the
	 * final process exit is responsible for releasing the underlying pages,
	 * descriptors, and filesystem state.
	 */
	if (current->clone_flags & CLONE_VM) {
		atomic_dec(&current->mm->mm_users);
	} else {
		exit_mmap(current);
		free_page_tables(current);
	}
	if (!(current->clone_flags & CLONE_FILES))
		exit_files();
	if (!(current->clone_flags & CLONE_FS))
		exit_fs();

	/*
	 * Check to see if any process groups have become orphaned
	 * as a result of our exiting, and if they have any stopped
	 * jobs, send them a SIGUP and then a SIGCONT.
	 *
	 * Case i: Our father is in a different pgrp than we are
	 * and we were the only connection outside, so our pgrp
	 * is about to become orphaned.
 	 */
	if ((current->p_pptr->pgrp != current->pgrp) &&
			(current->p_pptr->session == current->session) &&
				is_orphaned_pgrp(current->pgrp) &&
					has_stopped_jobs(current->pgrp)) {
		kill_pg(current->pgrp, SIGHUP, 1);
		kill_pg(current->pgrp, SIGCONT, 1);
	}

	/*
	 * Make init inherit all the child processes
	 */
	while ((p = current->p_cptr) != NULL) {
		current->p_cptr = p->p_osptr;
		p->p_ysptr = NULL;
		p->p_pptr = init_task.next_task;	/* init process */
		p->p_osptr = p->p_pptr->p_cptr;
		p->p_osptr->p_ysptr = p;
		p->p_pptr->p_cptr = p;
		if (p->state == TASK_ZOMBIE)
			notify_parent(p);

		if ((p->pgrp != current->pgrp) &&
				(p->session == current->session) &&
					is_orphaned_pgrp(p->pgrp) &&
						has_stopped_jobs(p->pgrp)) {
			kill_pg(p->pgrp, SIGHUP, 1);
			kill_pg(p->pgrp, SIGCONT, 1);
		}
	}
	/* Preserve trace CPU counters before the task can be reaped/freed. */
	syscall_trace_task_exit(current);
	current->state = TASK_ZOMBIE;
	current->exit_code = code;
	schedule();
}

int sys_exit(int error_code)
{
	do_exit((error_code&0xff)<<8);
	return 0;
}

/*
 * exit_group(2) terminates every task in the current thread group.  Live
 * workers are signalled and woken, then we wait until each has committed to
 * exit so the leader does not tear down the shared mm while another thread
 * is still running.  Zombie workers are left for do_exit()'s existing
 * reparent-to-init logic so they are reaped only after their final context
 * switch has safely parked them.
 */
int sys_exit_group(int error_code)
{
	long code = (error_code & 0xff) << 8;
	struct task_struct *p, *next;

	for (p = init_task.next_task; p != &init_task; p = next) {
		next = p->next_task;
		if (p == current || p->tgid != current->tgid)
			continue;
		if (p->state != TASK_ZOMBIE) {
			send_sig(SIGKILL, p, 1);
			if (p->state != TASK_RUNNING)
				wake_up_process(p);
		}
	}

	/*
	 * Threads that have published their userspace death (pthread_join
	 * returns) may still be running the tail of do_exit() on another CPU.
	 * Tearing down the shared mm while one of those tasks is still between
	 * "clear child tid" and the final schedule() would free user pages that
	 * the thread may still fault on.  Wait until every other thread in this
	 * group has committed to exit and has been parked by its final context
	 * switch.  We do not reap them here: the leader's do_exit() reparents
	 * those zombies to init, which can then safely reap the parked tasks.
	 */
	for (;;) {
		struct task_struct *t;
		int done = 1;

		for_each_task(t) {
			if (t == current || t->tgid != current->tgid)
				continue;
			if (t->state != TASK_ZOMBIE || !t->parked) {
				done = 0;
				break;
			}
		}
		if (done)
			break;
		schedule_timeout(1);
	}

	do_exit(code);
	return 0;
}

int sys_waitpid(int pid, int *stat_addr, int option)
{
	int res, flag;
	struct task_struct *p;
	wait_queue_node_t node;
	init_wait_queue_node(&node, current);

	if (stat_addr) {
		res = verify_area(VERIFY_WRITE, stat_addr, sizeof(int));
		if (res)
			return res;
	}
	in_wait_queue(&current->wait_chldexit, &node);
repeat:
	flag = 0;
 	for (p = current->p_cptr; p; p = p->p_osptr) {
 		if (pid > 0) {
 			if (p->pid != pid)
 				continue;
 		} else if (!pid) {
 			if (p->pgrp != current->pgrp)
 				continue;
 		} else if (pid != -1) {
 			if (p->pgrp != -pid)
 				continue;
 		}

 		flag = 1;
 		if (p->state == TASK_STOPPED) {
 			if (!p->exit_code)
 				continue;
 			if (stat_addr)
 				put_fs_long(p->exit_code, stat_addr);
 			p->exit_code = 0;
 			res = p->pid;
 			goto tail;
 		} else if (p->state == TASK_ZOMBIE) {
			current->stime_children += p->stime + p->stime_children;
			current->utime_children += p->utime + p->utime_children;

 			if (stat_addr)
 				put_fs_long(p->exit_code, stat_addr);
 			res = p->pid;
 			REMOVE_LINKS(p);
 			free_task_struct(p);
 			goto tail;
 		}
	}
	if (flag) {
		res = 0;
		if (option & WNOHANG)
			goto tail;
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		current->signal &= ~(1<<(SIGCHLD-1));
		res = -ERESTARTSYS;
		if (current->signal & ~current->blocked)
			goto tail;
		goto repeat;
	}
	res = -ECHILD;
tail:
	out_wait_queue(&current->wait_chldexit, &node);
	return res;
}

int sys_wait4(int pid, int *stat_addr, int option/*, struct rusage *rusage */)
{
	return sys_waitpid(pid, stat_addr, option);
}
