#include <time.h>
#include <misc.h>
#include <io.h>

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
	i8253_init();
}

#include <mm/page_alloc.h>

void irq_PIT(void)
{
	if (!(jiffies % 100)) {
		early_print("%u, ", jiffies/100);
	}
	jiffies++;
}