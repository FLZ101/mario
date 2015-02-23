#include <time.h>
#include <misc.h>
#include <io.h>

struct timeval xtime = {0};

long get_cmos_time(void)
{
	return 0;
}

#define LATCH (1193180/HZ)

volatile unsigned long jiffies = 0;

/*
 * Initialize the 8253 Programmable Interval Timer
 */
void __tinit i8253_init(void)
{
	outb(0x43, 0x36);	/* Binary, Mode 3, LSB/MSB, Counter 0 */
	outb(0x40, LATCH & 0xff);
	outb(0x40, LATCH >> 8);
}

void __tinit time_init(void)
{
	xtime.tv_sec = get_cmos_time();

	i8253_init();
}

#include <sched.h>

void irq_PIT(void)
{
	if (!(jiffies % 100))
		need_resched = 1;
	jiffies++;
}
