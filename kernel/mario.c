#include <multiboot.h>
#include <syscall.h>
#include <sched.h>
#include <trap.h>
#include <time.h>
#include <misc.h>
#include <irq.h>

#include <mm/page_alloc.h>
#include <mm/e820.h>

void put_c(unsigned char c);

void sys_putc(struct trap_frame tr)
{
	put_c(tr.ebx);
}

#define putchar(c) \
	__asm__ __volatile__("int $0x80"::"a"(0), "b"(c));

/*
Think about why the putchar below is wrong :)
void putchar(unsigned char c)
{
	__asm__ __volatile__("int $0x80"::"a"(0), "b"(c));
}
*/

volatile unsigned long ABC = 3;

void A(void)
{
	while (1) {
		if (1 == ABC) {
			putchar('A');
			ABC = 3;
		}
	}
}

void B(void)
{
	while (1) {
		if (2 == ABC) {
			putchar('B');
			ABC = 1;
		}
	}
}

void init(void)
{
	int pid;

	asm volatile("int $0x80; movl %%eax, %0":"=m"(pid):"a"(1));
	if (!pid)
		A();
	asm volatile("int $0x80; movl %%eax, %0":"=m"(pid):"a"(1));
	if (!pid)
		B();

	while (1) {
		if (3 == ABC) {
			putchar('C');
			ABC = 2;
		}
	}
}

extern unsigned long dump_stack(unsigned long old_esp);

#define move_to_user_mode() \
do { \
	unsigned long esp; \
	__asm__ __volatile__ ("movl %%esp, %0":"=m"(esp)); \
	esp = dump_stack(esp); \
	__asm__ __volatile__ ( \
	"pushl $0x2b\n\t" \
	"pushl %%eax\n\t" \
	"pushfl\n\t" \
	"pushl $0x23\n\t" \
	"pushl $1f\n\t" \
	"iret\n\t" \
	"1:\n" \
	"movl $0x2b, %%eax\n\t" \
	"movw %%ax, %%ds\n\t" \
	"movw %%ax, %%es\n\t" \
	"movw %%ax, %%fs\n\t" \
	"movw %%ax, %%gs" \
	::"a"(esp)); \
} while (0)

void mario(struct multiboot_info *m)
{
	early_print_init(m);
	setup_memory_region(m);
	page_alloc_init();
	trap_init();
	irq_init();
	time_init();
	sched_init();
	sti();

	move_to_user_mode();
	int pid;
	asm volatile("int $0x80; movl %%eax, %0":"=m"(pid):"a"(1));
	if (!pid)
		init();

	for (;;);
}
