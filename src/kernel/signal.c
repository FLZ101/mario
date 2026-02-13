#include <signal.h>
#include <task.h>
#include <trap.h>
#include <sched.h>
#include <unistd.h>
#include <misc.h>

#include <mm/uaccess.h>

extern int sys_waitpid(int pid, int *status, int option);
extern void notify_parent(struct task_struct *p);

#define _S(nr) (1<<((nr)-1))

#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

int sys_sigprocmask(int how, sigset_t *set, sigset_t *oset)
{
	sigset_t new_set, old_set = current->blocked;
	int error;

	if (set) {
		error = verify_area(VERIFY_READ, set, sizeof(sigset_t));
		if (error)
			return error;
		new_set = get_fs_long((unsigned long *) set) & _BLOCKABLE;
		switch (how) {
		case SIG_BLOCK:
			current->blocked |= new_set;
			break;
		case SIG_UNBLOCK:
			current->blocked &= ~new_set;
			break;
		case SIG_SETMASK:
			current->blocked = new_set;
			break;
		default:
			return -EINVAL;
		}
	}
	if (oset) {
		error = verify_area(VERIFY_WRITE, oset, sizeof(sigset_t));
		if (error)
			return error;
		put_fs_long(old_set, (unsigned long *)oset);
	}
	return 0;
}

int sys_sigpending(sigset_t *set)
{
	int error;
	error = verify_area(VERIFY_WRITE, set, sizeof(sigset_t));
	if (!error)
		put_fs_long(current->blocked & current->signal, (unsigned long *)set);
	return error;
}

/*
 *  "Setting a signal action to SIG_IGN for a signal that is pending
 *   shall cause the pending signal to be discarded, whether or not
 *   it is blocked" (but SIGCHLD is unspecified)
 *
 *  "Setting a signal action to SIG_DFL for a signal that is pending
 *   and whose default action is to ignore the signal (for example,
 *   SIGCHLD), shall cause the pending signal to be discarded, whether
 *   or not it is blocked"
 *
 * Note the silly behaviour of SIGCHLD: SIG_IGN means that the signal
 * isn't actually ignored, but does automatic child reaping, while
 * SIG_DFL is explicitly said by POSIX to force the signal to be ignored.
 */
static void check_pending(int signum)
{
	struct sigaction *p;

	p = signum - 1 + current->sigaction;
	if (p->sa_handler == SIG_IGN) {
		if (signum == SIGCHLD)
			return;
		current->signal &= ~_S(signum);
		return;
	}
	if (p->sa_handler == SIG_DFL) {
		if (signum != SIGCONT && signum != SIGCHLD && signum != SIGWINCH)
			return;
		current->signal &= ~_S(signum);
		return;
	}
}

unsigned long sys_signal(int signum, void (*handler)(int))
{
	int err;
	struct sigaction tmp;

	if (signum < 1 || signum > 32)
		return -EINVAL;
	if (signum == SIGKILL || signum == SIGSTOP)
		return -EINVAL;
	if (handler != SIG_DFL && handler != SIG_IGN) {
		err = verify_area(VERIFY_READ, handler, 1);
		if (err)
			return err;
	}
	tmp.sa_handler = handler;
	tmp.sa_mask = 0;
	tmp.sa_flags = SA_ONESHOT | SA_NOMASK;
	tmp.sa_restorer = NULL;
	handler = current->sigaction[signum-1].sa_handler;
	current->sigaction[signum-1] = tmp;
	check_pending(signum);
	return (unsigned long)handler;
}

int sys_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
	struct sigaction new_sa, *p;

	if (signum < 1 || signum > 32)
		return -EINVAL;
	if (signum == SIGKILL || signum == SIGSTOP)
		return -EINVAL;
	p = signum - 1 + current->sigaction;
	if (act) {
		int err = verify_area(VERIFY_READ, act, sizeof(*act));
		if (err)
			return err;
		memcpy_fromfs(&new_sa, act, sizeof(struct sigaction));
		if (new_sa.sa_flags & SA_NOMASK)
			new_sa.sa_mask = 0;
		else {
			new_sa.sa_mask |= _S(signum);
			new_sa.sa_mask &= _BLOCKABLE;
		}
		if (new_sa.sa_handler != SIG_DFL && new_sa.sa_handler != SIG_IGN) {
			err = verify_area(VERIFY_READ, new_sa.sa_handler, 1);
			if (err)
				return err;
		}
	}
	if (oldact) {
		int err = verify_area(VERIFY_WRITE, oldact, sizeof(*oldact));
		if (err)
			return err;
		memcpy_tofs(oldact, p, sizeof(struct sigaction));
	}
	if (act) {
		*p = new_sa;
		check_pending(signum);
	}
	return 0;
}

/*
 * This sets tr->esp even though we don't actually use sigstacks yet..
 */
int sys_sigreturn(unsigned long __unused)
{
#define COPY(x) tr->x = context.x

	struct sigcontext_struct context;
	struct trap_frame *tr;

	tr = (struct trap_frame *)&__unused;
	if (verify_area(VERIFY_READ, (void *)tr->esp, sizeof(context)))
		goto badframe;
	memcpy_fromfs(&context,(void *)tr->esp, sizeof(context));
	current->blocked = context.old_mask & _BLOCKABLE;
	COPY(ds);
	COPY(es);
	COPY(ss);
	COPY(cs);
	COPY(eip);
	COPY(ecx);
	COPY(edx);
	COPY(ebx);
	COPY(esp);
	COPY(ebp);
	COPY(edi);
	COPY(esi);
	tr->eflags &= ~0x40DD5;
	tr->eflags |= context.eflags & 0x40DD5;
	tr->error_code = -1;		/* disable syscall checks */
	return context.eax;
badframe:
	do_exit(SIGSEGV);
	return 0;

#undef COPY
}

void setup_frame(struct sigaction *sa, unsigned long **fp, unsigned long eip,
		struct trap_frame *tr, int signr, unsigned long old_mask)
{
#define __CODE ((unsigned long)(frame+22))
#define CODE(x) ((unsigned long *) ((x)+__CODE))

	unsigned long *frame = *fp;

	frame -= 32;
	if (verify_area(VERIFY_WRITE, frame, 30*4))
		do_exit(SIGSEGV);
	/* set up the "normal" stack seen by the signal handler (iBCS2) */
	put_fs_long(__CODE, frame);
	put_fs_long(signr, frame+1);
	put_fs_long(tr->es, frame+2);
	put_fs_long(tr->ds, frame+3);
	put_fs_long(tr->edi, frame+4);
	put_fs_long(tr->esi, frame+5);
	put_fs_long(tr->ebp, frame+6);
	put_fs_long((long)*fp, frame+7);
	put_fs_long(tr->ebx, frame+8);
	put_fs_long(tr->edx, frame+9);
	put_fs_long(tr->ecx, frame+10);
	put_fs_long(tr->eax, frame+11);
	put_fs_long(current->thread.trap_no, frame+12);
	put_fs_long(current->thread.error_code, frame+13);
	put_fs_long(eip, frame+14);
	put_fs_long(tr->cs, frame+15);
	put_fs_long(tr->eflags, frame+16);
	put_fs_long(tr->esp, frame+17);
	put_fs_long(tr->ss, frame+18);
	put_fs_long(0, frame+19);		/* 387 state pointer - not implemented*/
	/* non-iBCS2 extensions.. */
	put_fs_long(old_mask, frame+20);
	put_fs_long(current->thread.cr2, frame+21);
	/* set up the return code... */
	put_fs_long(0x0000b858, CODE(0));	/* popl %eax ; movl $__SYS_sigreturn, %eax */
	put_fs_long(0x80cd0000, CODE(4));	/* int $0x80 */
	put_fs_long(__SYS_sigreturn, CODE(2));
	*fp = frame;

#undef __CODE
#undef CODE
}

/*
 * Note that 'init' is a special process: it doesn't get signals it doesn't
 * want to handle. Thus you cannot kill init even with a SIGKILL even by
 * mistake.
 *
 * Note that we go through the signals twice: once to check the signals that
 * the kernel can handle, and then we build all the user-level signal handling
 * stack-frames in one go after that.
 */
int do_signal(struct trap_frame *tr, unsigned long old_mask)
{
	unsigned long m = ~current->blocked;
	unsigned long to_handle = 0;
	unsigned long *frame = NULL;
	unsigned long eip;
	unsigned long signr;
	struct sigaction *sa;

	while ((signr = current->signal & m)) {
		// get & clear lowest signal no
		__asm__ __volatile__ (
				"bsf %1, %1\n\t"
				"btrl %1, %0"
				:"+m"(current->signal), "+r"(signr)
				:);
		sa = current->sigaction + signr;
		signr++;

		if (sa->sa_handler == SIG_IGN) {
			if (signr != SIGCHLD)
				continue;
			/* check for SIGCHLD: it's special */
			while (sys_waitpid(-1, NULL, WNOHANG) > 0)
				/* nothing */;
			continue;
		}
		if (sa->sa_handler == SIG_DFL) {
			if (current->pid == 1)
				continue;
			switch (signr) {
			case SIGCONT: case SIGCHLD: case SIGWINCH:
				continue;

			case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
				current->state = TASK_STOPPED;
				current->exit_code = signr;
				if (!(current->p_pptr->sigaction[SIGCHLD-1].sa_flags &
						SA_NOCLDSTOP))
					notify_parent(current);
				schedule();
				continue;

			case SIGQUIT: case SIGILL: case SIGTRAP:
			case SIGABRT: case SIGFPE: case SIGSEGV:
				/* no break */
			default:
				current->signal |= _S(signr & 0x7f);
				do_exit(signr);
			}
		}
		/*
		 * OK, we're invoking a handler
		 */
		if (tr->error_code >= 0) {	/* Is from system call? */
			if (tr->eax == -ERESTARTNOHAND ||
					(tr->eax == -ERESTARTSYS && !(sa->sa_flags & SA_RESTART)))
				tr->eax = -EINTR;
		}
		to_handle |= 1 << (signr-1);
		m &= ~sa->sa_mask;
	}

	if (tr->error_code >= 0) {
		if (tr->eax == -ERESTARTNOHAND ||
				tr->eax == -ERESTARTSYS ||
					tr->eax == -ERESTARTNOINTR) {
			tr->eax = tr->error_code;
			tr->eip -= 2; // restart the system call
		}
	}

	if (!to_handle)
		return 0;
	eip = tr->eip;
	frame = (unsigned long *)tr->esp;
	signr = 1;
	sa = current->sigaction;
	// set up the stack frames for signal handlers.
	// return from or restart the system call after all signal handlers are run
	for (m = 1; m; sa++, signr++, m += m) {
		if (m > to_handle)
			break;
		if (!(m & to_handle))
			continue;
		setup_frame(sa, &frame, eip, tr, signr, old_mask);
		eip = (unsigned long)sa->sa_handler;
		if (sa->sa_flags & SA_ONESHOT)
			sa->sa_handler = SIG_DFL;

		/* force a supervisor-mode page-in of the signal handler to reduce races */
		__asm__("testb $0,%%fs:%0": :"m" (*(char *) eip));
		tr->cs = USER_CS; tr->ss = USER_DS;
		tr->ds = USER_DS; tr->es = USER_DS;

		current->blocked |= sa->sa_mask;
		old_mask |= sa->sa_mask;
	}
	tr->esp = (unsigned long) frame;
	tr->eip = eip;		/* "return" to the first handler */
	current->thread.trap_no = current->thread.error_code = 0;
	return 1;
}

int sys_sigsuspend(int dummy0, int dummy1, sigset_t mask)
{
	sigset_t old_mask;
	struct trap_frame *tr = (struct trap_frame *) &dummy0;

	old_mask = current->blocked;
	current->blocked = mask & _BLOCKABLE;
	tr->eax = -EINTR;
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		if (do_signal(tr, old_mask))
			return -EINTR;
	}
}
