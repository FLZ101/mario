#include <multiboot.h>
#include <syscall.h>
#include <sched.h>
#include <trap.h>
#include <time.h>
#include <misc.h>
#include <irq.h>

#include <mm/page_alloc.h>
#include <mm/kmalloc.h>
#include <mm/e820.h>

#include <lib/stdarg.h>

void printf(const char *fmt, ...);

void *sys_malloc(struct trap_frame tr)
{
	return kmalloc(tr.ebx);
}

void *malloc(size_t size)
{
	void *p = NULL;
	asm volatile("int $0x80":"=a"(p):"0"(2), "b"(size));
	return p;
}

void sys_free(struct trap_frame tr)
{
	kfree((void *)tr.ebx);
}

void free(void *p)
{
	asm volatile("movl $3, %%eax; int $0x80"::"b"(p):"eax");
}

volatile unsigned long ABC = 3;

void A(void)
{
	while (1) {
		if (1 == ABC) {
			void *p = malloc(123);
			printf("A: %x\n", (unsigned int)p);
			free(p);
			ABC = 3;
		}
	}
}

void B(void)
{
	while (1) {
		if (2 == ABC) {
			void *p = malloc(123);
			printf("B: %x\n", (unsigned int)p);
			free(p);
			ABC = 1;
		}
	}
}

void init(void)
{
	if (!fork())
		A();
	if (!fork())
		B();

	while (1) {
		if (3 == ABC) {
			void *p = malloc(123);
			printf("C: %x\n", (unsigned int)p);
			free(p);
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
	/* test page_malloc */

	/* test kmalloc */
	void *p;
	p = kmalloc(123);
	early_print("%x\n", (unsigned int)p);
	kfree(p);
	p = kmalloc(123);
	early_print("%x\n", (unsigned int)p);
	kfree(p);
	/* test kmalloc */
}

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
	test_mm();
	move_to_user_mode();
	if (!fork())
		init();

	for (;;);
}
