#include <multiboot.h>
#include <trap.h>
#include <time.h>
#include <misc.h>
#include <irq.h>

#include <mm/e820.h>
#include <mm/mm.h>

#include <fs/fs.h>

void kernel_thread(void (*fun)(void *), void *arg)
{
	__asm__ __volatile__(
		"movl %%esp, %%esi\n\t"
		"movl $6, %%eax\n\t"
		"int $0x80\n\t"
		"cmpl %%esp, %%esi\n\t"
		"je 1f\n\t"
		"pushl %0\n\t"
		"call *%1\n\t"
		"jmp .\n\t"
		"1:"
		:
		:"b"(arg), "c"(fun)
		:"eax", "esi", "memory");
}

extern int sys_pause(void);

extern void bh_thread(void *arg);

extern void test_signal(void *arg);
extern void test_init(void *arg);

void init(void *arg)
{
	kernel_thread(bh_thread, NULL);
	kernel_thread(test_init, NULL);

	while(1)
		sys_pause();
}

void idle(void)
{
	cli();
	if (!current->need_resched)
		safe_halt();
	else
		sti();
}

void cpu_idle(void)
{
	while (1) {
		while (!current->need_resched)
			idle();
	}
}

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
	chrdev_init();
	fs_init();

	sti();
	kernel_thread(init, NULL);
	cpu_idle();
}
