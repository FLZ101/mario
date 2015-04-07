#include <signal.h>
#include <timer.h>
#include <task.h>
#include <time.h>
#include <misc.h>

static LIST_HEAD(timer_head);

void add_timer(struct timer_list *timer)
{
	struct list_head *tmp;

	irq_save();
	timer->expires += jiffies;
	list_for_each(tmp, &timer_head) {
		struct timer_list *t =
			list_entry(tmp, struct timer_list, list);
		if (timer->expires < t->expires)
			break;
	}
	list_add_tail(&timer->list, tmp);
	irq_restore();
}

static inline int timer_in_list(struct timer_list *timer)
{
	return (timer->list.next != NULL);
}

int del_timer(struct timer_list *timer)
{
	if (!timer_in_list(timer))
		return 0;

	irq_save();
	list_del(&timer->list);
	timer->list.next = NULL;
	timer->expires -= jiffies;
	if (timer->expires < 0)
		timer->expires = 0;
	irq_restore();
	return 1;
}

void run_timer_list(void)
{
	struct list_head *tmp;

	cli();
	list_for_each(tmp, &timer_head) {
		struct timer_list *t =
			list_entry(tmp, struct timer_list, list);
		if (t->expires > jiffies)
			break;
		list_del(tmp);
		t->list.next = NULL;
		tmp = t->list.prev;
		sti();
		t->fun(t->data);
		cli();
	}
	sti();
}

void it_real_fun(unsigned long data)
{
	struct task_struct *p = (struct task_struct *)data;

	send_sig(SIGALRM, p, 1);

	if (p->it_real_incr) {
		p->real_timer.expires = p->it_real_incr;
		add_timer(&p->real_timer);
	}
}

static long tvtojiffies(struct timeval *value)
{
	long sec = value->tv_sec;
	long usec = value->tv_usec;

	usec += 1000000 / HZ - 1;
	usec /= 1000000 / HZ;
	return HZ*sec+usec;
}

static void jiffiestotv(long jiffies, struct timeval *value)
{
	value->tv_sec = jiffies / HZ;
	value->tv_usec = (jiffies % HZ) * (1000000 / HZ);
}

static int _getitimer(int which, struct itimerval *value)
{
	long val, interval;

	switch (which) {
	case ITIMER_REAL:
		interval = current->it_real_incr;
		val = 0;
		if (del_timer(&current->real_timer)) {
			val = current->real_timer.expires;
			add_timer(&current->real_timer);
		}
		break;
	case ITIMER_VIRTUAL:
		val = current->it_virt_value;
		interval = current->it_virt_incr;
		break;
	case ITIMER_PROF:
		val = current->it_prof_value;
		interval = current->it_prof_incr;
		break;
	default:
		return -EINVAL;
	}

	jiffiestotv(val, &value->it_value);
	jiffiestotv(interval, &value->it_interval);
	return 0;
}

int _setitimer(int which, struct itimerval *value, struct itimerval *ovalue)
{
	long i, j;

	if (ovalue && _getitimer(which, ovalue) < 0)
		return -EINVAL;

	i = tvtojiffies(&value->it_interval);
	j = tvtojiffies(&value->it_value);

	switch (which) {
	case ITIMER_REAL:
		del_timer(&current->real_timer);
		current->it_real_value = j;
		current->it_real_incr = i;
		if (!j)
			break;
		current->real_timer.expires = j;
		add_timer(&current->real_timer);
		break;
	case ITIMER_VIRTUAL:
		if (j)
			j++;
		current->it_virt_value = j;
		current->it_virt_incr = i;
		break;
	case ITIMER_PROF:
		if (j)
			j++;
		current->it_prof_value = j;
		current->it_prof_incr = i;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int sys_alarm(long seconds)
{
	struct itimerval it_new, it_old;

	it_new.it_interval.tv_sec = it_new.it_interval.tv_usec = 0;
	it_new.it_value.tv_sec = seconds;
	it_new.it_value.tv_usec = 0;
	_setitimer(ITIMER_REAL, &it_new, &it_old);
	return (it_old.it_value.tv_sec + (it_old.it_value.tv_usec / 1000000));
}
