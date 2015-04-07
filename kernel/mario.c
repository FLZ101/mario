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

void printf(const char *fmt, ...);

void kernel_thread(void (*fun)(unsigned int), unsigned int arg)
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

unsigned int x = 0;
unsigned int y = 0;

long get_cmos_time(void);
void init(unsigned int n)
{
	if (!n)
		return;
	if (!y) {
		y = n;
		x = n - 1;
	}
	if (--n)
		kernel_thread(init, n);

	printf("[%u]\t",n);
	get_cmos_time();

	while (1) {
		if (x == n) {
			printf("%c",'A'+n);
			x = (x+1)%y;
		}
	}
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

void mario(struct multiboot_info *m)
{
	early_print_init(m);
	setup_memory_region(m);
	paging_init();
	page_alloc_init();
	test_mm();
	trap_init();
	irq_init();
	time_init();
	sti();

	kernel_thread(timer_thread, 0x45184518);
	kernel_thread(init, 2);
}
