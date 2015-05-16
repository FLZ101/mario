#include <sched.h>
#include <signal.h>

#include <errno.h>

#include <mm/uaccess.h>
#include <mm/mm.h>

extern void free_page_tables(struct task_struct *);
extern void exit_mmap(struct task_struct *);
extern void exit_files(void);
extern void exit_fs(void);

/*
 * Is it right?
 */
static void forget_original_parent(struct task_struct *father)
{
	struct task_struct *p;

	for_each_task(p) {
		if (p->p_pptr == father)
			p->p_pptr = init_task.next_task;
	}
}

void notify_parent(struct task_struct *p)
{
	if (p->p_pptr == init_task.next_task)
		p->exit_signal = SIGCHLD;
	send_sig(p->exit_signal, p->p_pptr, 1);
	wake_up_interruptible(&p->p_pptr->wait_chldexit);
}

void do_exit(long code)
{
	struct task_struct *p;

	exit_mmap(current);
	free_page_tables(current);
	exit_files();
	exit_fs();
	forget_original_parent(current);
	notify_parent(current);
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
	}
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
 * Because group ID has not been implemented, 
 * waitpid supports only pid > 0 and pid = -1
 */
int sys_waitpid(int pid, int option)
{
	int res;
	struct task_struct *p;
	wait_queue_node_t node;
	init_wait_queue_node(&node, current);

	if (pid != -1 && pid <= 0)
		return -EINVAL;
	in_wait_queue(&current->wait_chldexit, &node);
repeat:
	res = 0;
 	for (p = current->p_cptr; p; p = p->p_osptr) {
 		if (pid > 0 && p->pid != pid)
 			continue;
 		res = 1;
 		if (p->state == TASK_ZOMBIE) {
 			res = p->pid;
 			REMOVE_LINKS(p);
 			free_task_struct(p);
 			goto tail;
 		}
	}
	if (res) {
		res = 0;
		if (option & WNOHANG)
			goto tail;
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		current->signal &= ~(1<<(SIGCHLD-1));
		if (current->signal & ~current->blocked)
			goto tail;
		goto repeat;
	}
	res = -ECHILD;
tail:
	out_wait_queue(&current->wait_chldexit, &node);
	return res;
}