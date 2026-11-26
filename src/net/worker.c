#include <misc.h>
#include <net/worker.h>
#include <sched.h>
#include <signal.h>
#include <time.h>

#include <lib/spinlock.h>
#include <lib/string.h>

#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/sys.h>
#include <lwip/timeouts.h>

#define NET_TICK_MS (1000U / HZ)
#define NET_WORK_BATCH 32
#define NET_CALL_QUEUE_LIMIT 64

struct net_call_work {
	struct net_work work;
	int (*run)(void *);
	void *data;
	wait_queue_t wait;
	int done;
	int result;
};

static spinlock_t net_wait_lock = SPINLOCK_UNLOCKED;
static wait_queue_t net_wait;
static int net_armed;
static unsigned long net_deadline;
static int net_started;
static struct net_work *net_head;
static struct net_work **net_tail = &net_head;
static unsigned int net_calls_queued;

void mario_lwip_assert(const char *expr, const char *file, int line)
{
	hang("lwIP: %s (%s:%d)", expr, file, line);
}

u32_t sys_now(void)
{
	return (u32_t)jiffies * NET_TICK_MS;
}

void net_tick(void)
{
	unsigned long flags;
	int wake = 0;

	if (!__atomic_load_n(&net_armed, __ATOMIC_RELAXED))
		return;
	spin_lock_irqsave(&net_wait_lock, flags);
	if (__atomic_load_n(&net_armed, __ATOMIC_RELAXED) &&
	    (long)((unsigned long)jiffies - net_deadline) >= 0) {
		__atomic_store_n(&net_armed, 0, __ATOMIC_RELAXED);
		wake = 1;
	}
	spin_unlock_irqrestore(&net_wait_lock, flags);
	if (wake)
		wake_up_uninterruptible(&net_wait);
}

static void net_wait_for_timer(void)
{
	unsigned long start = jiffies;
	u32_t delay = sys_timeouts_sleeptime();

	if (!delay || (unsigned long)jiffies != start)
		return;
	irq_save();
	ACQUIRE_LOCK(&net_wait_lock);
	if (net_head) {
		RELEASE_LOCK(&net_wait_lock);
		irq_restore();
		return;
	}
	if (delay != SYS_TIMEOUTS_SLEEPTIME_INFINITE) {
		net_deadline = start + (delay + NET_TICK_MS - 1) / NET_TICK_MS;
		if ((long)((unsigned long)jiffies - net_deadline) >= 0) {
			RELEASE_LOCK(&net_wait_lock);
			irq_restore();
			return;
		}
		__atomic_store_n(&net_armed, 1, __ATOMIC_RELAXED);
	}
	sleep_on(&net_wait, TASK_UNINTERRUPTIBLE, &net_wait_lock);
	irq_restore();
}

static void net_enqueue(struct net_work *work)
{
	work->next = NULL;
	*net_tail = work;
	net_tail = &work->next;
	wake_up_uninterruptible(&net_wait);
}

void net_submit(struct net_work *work)
{
	unsigned long flags;

	spin_lock_irqsave(&net_wait_lock, flags);
	net_enqueue(work);
	spin_unlock_irqrestore(&net_wait_lock, flags);
}

static int net_cancel(struct net_work *work)
{
	struct net_work **entry = &net_head;

	while (*entry && *entry != work)
		entry = &(*entry)->next;
	if (!*entry)
		return 0;
	*entry = work->next;
	if (!*entry)
		net_tail = entry;
	net_calls_queued--;
	return 1;
}

static void net_run_call(struct net_work *work)
{
	struct net_call_work *call = list_entry(work, struct net_call_work, work);
	unsigned long flags;

	call->result = call->run(call->data);
	spin_lock_irqsave(&net_wait_lock, flags);
	call->done = 1;
	wake_up_all(&call->wait);
	spin_unlock_irqrestore(&net_wait_lock, flags);
}

static int net_do_call(int (*run)(void *), void *data, int interruptible)
{
	struct net_call_work call = {
		.work.run = net_run_call, .run = run, .data = data,
	};

	if (!__atomic_load_n(&net_started, __ATOMIC_ACQUIRE))
		return -ENETDOWN;
	init_wait_queue(&call.wait);
	irq_save();
	ACQUIRE_LOCK(&net_wait_lock);
	if ((interruptible && signal_pending(current)) || net_calls_queued == NET_CALL_QUEUE_LIMIT) {
		int error = net_calls_queued == NET_CALL_QUEUE_LIMIT ? -ENOBUFS : -EINTR;

		RELEASE_LOCK(&net_wait_lock);
		irq_restore();
		return error;
	}
	net_calls_queued++;
	net_enqueue(&call.work);
	while (!call.done) {
		if (interruptible && signal_pending(current)) {
			if (net_cancel(&call.work)) {
				call.result = -EINTR;
				break;
			}
			interruptible = 0;
		}
		sleep_on(&call.wait, interruptible ? TASK_INTERRUPTIBLE : TASK_UNINTERRUPTIBLE,
			 &net_wait_lock);
		ACQUIRE_LOCK(&net_wait_lock);
	}
	RELEASE_LOCK(&net_wait_lock);
	irq_restore();
	return call.result;
}

int net_call(int (*run)(void *), void *data)
{
	return net_do_call(run, data, 0);
}

int net_call_interruptible(int (*run)(void *), void *data)
{
	return net_do_call(run, data, 1);
}

static void net_run_work(void)
{
	for (int count = 0; count < NET_WORK_BATCH; count++) {
		struct net_work *work;
		unsigned long flags;

		spin_lock_irqsave(&net_wait_lock, flags);
		work = net_head;
		if (work) {
			if (!(net_head = work->next))
				net_tail = &net_head;
			if (work->run == net_run_call)
				net_calls_queued--;
		}
		spin_unlock_irqrestore(&net_wait_lock, flags);
		if (!work)
			return;
		work->run(work);
	}
}

static void net_worker(void *arg)
{
	(void)arg;
	memcpy(current->comm, "[net]", sizeof("[net]"));
	lwip_init();
	pr_info("net: lwIP worker ready\n");
	for (;;) {
		net_run_work();
		netif_poll_all();
		sys_check_timeouts();
		net_wait_for_timer();
	}
}

void net_start(void)
{
	init_wait_queue(&net_wait);
	if (kernel_thread(net_worker, NULL) < 0)
		pr_err("net: cannot start lwIP worker\n");
	else
		__atomic_store_n(&net_started, 1, __ATOMIC_RELEASE);
}
