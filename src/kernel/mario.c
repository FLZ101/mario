#include <multiboot.h>
#include <trap.h>
#include <time.h>
#include <misc.h>
#include <irq.h>
#include <smp.h>
#include <futex.h>

#define __KERNEL_SYSCALLS__
#include <unistd.h>

#include <mm/e820.h>
#include <mm/mm.h>

#include <fs/fs.h>

#include <syscall_trace.h>

void kernel_thread(void (*fun)(void *), void *arg)
{
	__asm__ __volatile__(
		"movl %%esp, %%esi\n\t"
		"movl %2, %%eax\n\t"
		"int $0x80\n\t"
		"cmpl %%esp, %%esi\n\t"
		"je 1f\n\t"
		"pushl %0\n\t"
		"call *%1\n\t"
		"jmp .\n\t"
		"1:"
		:
		:"b"(arg), "c"(fun), "i"(SYS_fork)
		:"eax", "esi", "memory");
}

#define MAX_INIT_ARGS 8
#define MAX_INIT_ENVS 8

static char *argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
static char *envp_init[MAX_INIT_ENVS+2] = { "HOME=/root", NULL, };

void init(void *arg)
{
	// pid of init.exe is 1
	execve("/sbin/init.exe", argv_init, envp_init);
	hang("Fail to start init.exe: %d", errno);
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

void printk_init(struct multiboot_info *m);

void mario(struct multiboot_info *m)
{
	percpu_init();
	printk_init(m);
	setup_memory_region(m);
	paging_init(m);
	page_alloc_init();
	trap_init();
	irq_init();
	time_init();

	blkdev_init();
	buffer_init();
	chrdev_init();
	fs_init();

	/*
	 * Initialise the per-CPU scheduler state before interrupts are enabled.
	 * The PIT starts ticking at time_init(); once sti() runs a timer IRQ can
	 * call schedule() through ret_common, and that path must never observe an
	 * uninitialised runqueue.  It is also required before APs may schedule.
	 */
	sched_init_smp();
	futex_init();

	sti();
	/* The PIT is already running, which lets the AP start-up path wait on
	 * jiffies for INIT/SIPI pacing. */
	smp_init();
	/* rdtsc needs a running PIT for a known time base. */
	syscall_trace_calibrate();
	kernel_thread(init, NULL);
	cpu_idle();
}
