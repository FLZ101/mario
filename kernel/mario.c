#include <multiboot.h>
#include <syscall.h>
#include <sched.h>
#include <trap.h>
#include <time.h>
#include <timer.h>
#include <misc.h>
#include <irq.h>

#include <mm/page_alloc.h>
#include <mm/kmalloc.h>
#include <mm/paging.h>
#include <mm/e820.h>

#include <lib/stdarg.h>

#include <fs/fs.h>

void printf(const char *fmt, ...);

void kernel_thread(void (*fun)(void *), void *arg)
{
	__asm__ __volatile__(
		"movl %%esp, %%esi\n\t"
		"movl $1, %%eax\n\t"
		"int $0x80\n\t"
		"cmpl %%esp, %%esi\n\t"
		"je 1f\n\t"
		"pushl %0\n\t"
		"call *%1\n\t"
		"jmp .\n\t"
		"1:"
		:
		:"b"(arg), "c"(fun)
		:"eax", "memory");
}

void page_alloc_print(void)
{
	int i;
	for (i = 3; i >= 0; i--)
		free_list_print(i);
}

void test_mm(void)
{
	/* test page_malloc */
	unsigned long x;
	page_alloc_print();
	x = page_alloc();
	early_print("%x\n", x);
	page_free(x);
	page_alloc_print();
	x = page_alloc();
	early_print("%x\n", x);
	page_free(x);

	/* test kmalloc */
	void *p;
	p = kmalloc(123);
	early_print("%x\n", (unsigned int)p);
	kfree(p);
	p = kmalloc(123);
	early_print("%x\n", (unsigned int)p);
	kfree(p);
}

void test_timer(unsigned long data)
{
	early_print("\t%x\n", data);

	struct timer_list *timer = kmalloc(sizeof(*timer));
	init_timer(timer);
	timer->expires = 3*HZ;
	timer->fun = test_timer;
	timer->data = (unsigned long)timer;
	add_timer(timer);
}

int sys_alarm(long seconds);

void timer_thread(unsigned int n)
{
	struct itimerval it_new;
	it_new.it_interval.tv_usec = 0;
	it_new.it_value.tv_usec = 0;

	it_new.it_interval.tv_sec = 1;
	it_new.it_value.tv_sec = 1;
	_setitimer(ITIMER_REAL, &it_new, NULL);

	it_new.it_interval.tv_sec = 1;
	it_new.it_value.tv_sec = 1;
	_setitimer(ITIMER_VIRTUAL, &it_new, NULL);

	it_new.it_interval.tv_sec = 1;
	it_new.it_value.tv_sec = 1;
	_setitimer(ITIMER_PROF, &it_new, NULL);

	/*
	sys_alarm(7);
	*/

	test_timer(n);
}

void test_blkdev(void *arg)
{
	int n = (int)arg;

	struct buffer_head *bh = get_buffer(MKDEV(RD_MAJOR, 0), 0);
	bread(bh);
	char *buf = bh->b_data;
	buf[10] = '\0';
	printf("%s%u\n", buf, n);
	set_dirty(bh);
	brelse(bh);
}

void init(void *arg)
{
	int n = (int)arg;

	struct buffer_head *bh = get_buffer(MKDEV(RD_MAJOR, 0), 0);
	printf("blkdev test starts\n");
	while (n--)
		kernel_thread(test_blkdev, (void *)n);

	schedule_timeout(3*HZ);
	brelse(bh);
}

void cpu_idle(void)
{
	for (; ; )
		schedule();
}

void bh_thread(void *arg);

void mario(struct multiboot_info *m)
{
	early_print_init(m);
	setup_memory_region(m);
	paging_init();
	page_alloc_init();
	trap_init();
	irq_init();
	time_init();

	blkdev_init();
	buffer_init();
	sti();

	kernel_thread(init, (void *)10000);
	kernel_thread(bh_thread, NULL);

	cpu_idle();
}
