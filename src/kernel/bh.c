#include <bh.h>
#include <sched.h>

#include <lib/stddef.h>

unsigned long bh_active = 0;
unsigned long bh_mask = 0;
struct bh_struct bh_base[32];

/* used to wake up the bh_thread */
struct task_struct *bh_task = NULL;

extern int sys_pause(void);

void do_bottom_half(void)
{
	unsigned long active = bh_active & bh_mask;
	unsigned long mask = 1, left = ~0;
	struct bh_struct *bh = bh_base;

	for (; left & active; bh++, mask += mask, left += left) {
		if (mask & active) {
			void (*fn)(void *);
			bh_active &= ~mask;
			fn = bh->routine;
			if (fn)
				fn(bh->data);
		}
	}
}

void bh_thread(void *arg)
{
	bh_task = current;

	for (; ; ) {
		if (bh_mask & bh_active)
			do_bottom_half();
		else
			sys_pause();
	}
}

