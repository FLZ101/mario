#include <timer.h>
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

void del_timer(struct timer_list *timer)
{
	irq_save();
	timer->expires -= jiffies;
	list_del(&timer->list);
	irq_restore();
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
		sti();
		t->fun(t->data);
		cli();
	}
	sti();
}