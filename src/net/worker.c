#include <misc.h>
#include <net/worker.h>
#include <sched.h>
#include <time.h>

#include <lib/spinlock.h>
#include <lib/string.h>

#include <lwip/init.h>
#include <lwip/sys.h>
#include <lwip/timeouts.h>

#define NET_TICK_MS (1000U / HZ)

static spinlock_t net_wait_lock = SPINLOCK_UNLOCKED;
static wait_queue_t net_wait;
static int net_armed;
static unsigned long net_deadline;

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

static void net_worker(void *arg)
{
	(void)arg;
	memcpy(current->comm, "[net]", sizeof("[net]"));
	lwip_init();
	pr_info("net: lwIP worker ready\n");
	for (;;) {
		sys_check_timeouts();
		net_wait_for_timer();
	}
}

void net_start(void)
{
	init_wait_queue(&net_wait);
	if (kernel_thread(net_worker, NULL) < 0)
		pr_err("net: cannot start lwIP worker\n");
}
