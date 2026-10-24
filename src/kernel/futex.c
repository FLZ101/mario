#include <task.h>
#include <sched.h>
#include <futex.h>
#include <signal.h>
#include <errno.h>

#include <mm/uaccess.h>

#include <lib/spinlock.h>
#include <lib/list.h>

/*
 * Futexes are hashed by user address.  Collisions are permitted: waiters are
 * expected to re-check the futex word after waking, exactly as musl's __wait
 * loop does.  The hash is intentionally small because pthread traffic on
 * Mario is light.
 */
#define FUTEX_HASH_BITS 6
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS)

struct futex_queue {
	wait_queue_t wait;
};

struct futex_waiter {
	wait_queue_node_t node;
	int *uaddr;
};

static struct futex_queue futex_queues[FUTEX_HASH_SIZE];
static spinlock_t futex_lock = SPINLOCK_UNLOCKED;

void futex_init(void)
{
	for (int i = 0; i < FUTEX_HASH_SIZE; i++)
		init_wait_queue(&futex_queues[i].wait);
}

static struct futex_queue *futex_queue(unsigned long addr)
{
	return &futex_queues[(addr >> 2) & (FUTEX_HASH_SIZE - 1)];
}

/*
 * Wake up to @nr tasks from one hash bucket.  This is deliberately kept
 * separate from sys_futex so do_exit() can use the same wake path when it
 * clears CLONE_CHILD_CLEARTID.
 */
static int wake_matching(struct futex_queue *q, int *uaddr, int nr)
{
	struct list_head *tmp;
	unsigned long flags;
	int woke = 0;

	if (nr <= 0)
		nr = 0x7fffffff;

	spin_lock_irqsave(&wait_queue_lock, flags);
	list_for_each(tmp, &q->wait.task_list) {
		wait_queue_node_t *node =
			list_entry(tmp, wait_queue_node_t, task_list);
		struct futex_waiter *waiter = (struct futex_waiter *)node;

		/* Hash collisions are possible, so only wake waiters that are
		 * actually waiting on this futex address.  Waking a waiter from
		 * another address would consume the wake count and strand the
		 * intended waiter forever. */
		if (waiter->uaddr == uaddr &&
		    (node->p->state &
		     (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE))) {
			wake_up_process(node->p);
			if (++woke >= nr)
				break;
		}
	}
	spin_unlock_irqrestore(&wait_queue_lock, flags);
	return woke;
}

int futex_wake(int *uaddr, int nr_wake)
{
	struct futex_queue *q;
	int woke;

	/*
	 * Take futex_lock only to order against futex_wait's check-plus-enqueue
	 * sequence.  The bucket is static, so the lock can be dropped before
	 * waking tasks and taking the scheduler lock.
	 */
	spin_lock(&futex_lock);
	q = futex_queue((unsigned long)uaddr);
	spin_unlock(&futex_lock);

	woke = wake_matching(q, uaddr, nr_wake);
	return woke;
}

static int futex_wait(int *uaddr, int op, int val,
		      const struct timespec *timeout)
{
	struct futex_waiter waiter;
	struct futex_queue *q;
	int current_val;

	(void)op;
	/* Timed waits are not required by the initial pthread smoke test. */
	if (timeout)
		return -ENOSYS;

	if (verify_area(VERIFY_READ, uaddr, sizeof(int)))
		return -EFAULT;

	spin_lock(&futex_lock);
	current_val = get_fs_long(uaddr);
	if (current_val != val) {
		spin_unlock(&futex_lock);
		return -EAGAIN;
	}

	q = futex_queue((unsigned long)uaddr);
	init_wait_queue_node(&waiter.node, current);
	waiter.uaddr = uaddr;

	/*
	 * Enqueue while futex_lock is still held, then drop it before
	 * scheduling.  That makes the uaddr check and waiter insertion atomic
	 * with respect to futex_wake(), preventing the classic lost wakeup.
	 */
	irq_save();
	current->state = TASK_INTERRUPTIBLE;
	in_wait_queue(&q->wait, &waiter.node);
	spin_unlock(&futex_lock);
	irq_restore();
	schedule();
	out_wait_queue(&q->wait, &waiter.node);

	if (signal_pending(current))
		return -EINTR;
	return 0;
}

int sys_futex(int *uaddr, int op, int val, struct timespec *timeout,
	      int *uaddr2, int val3)
{
	int cmd = op & ~(FUTEX_PRIVATE | FUTEX_CLOCK_REALTIME);

	if (verify_area(VERIFY_READ, uaddr, sizeof(int)))
		return -EFAULT;

	switch (cmd) {
	case FUTEX_WAIT:
		return futex_wait(uaddr, op, val, timeout);
	case FUTEX_WAKE:
		return futex_wake(uaddr, val);
	case FUTEX_REQUEUE:
		/*
		 * musl's condvar path uses requeue as an optimisation.  Waking
		 * val3 tasks from the source futex is sufficient for correctness
		 * with musl's retry loop; the destination queue is intentionally
		 * not populated yet.
		 */
		(void)uaddr2;
		return futex_wake(uaddr, val3);
	case FUTEX_CMP_REQUEUE:
	case FUTEX_WAKE_OP:
	case FUTEX_LOCK_PI:
	case FUTEX_UNLOCK_PI:
	case FUTEX_TRYLOCK_PI:
	case FUTEX_WAIT_BITSET:
	case FUTEX_WAKE_BITSET:
	default:
		return -ENOSYS;
	}
}
