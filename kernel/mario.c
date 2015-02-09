#include <multiboot.h>
#include <trap.h>
#include <time.h>
#include <misc.h>
#include <irq.h>

#include <mm/e820.h>

void mario(struct multiboot_info *m)
{
	early_print_init(m);

	setup_memory_region(m);

	trap_init();
	__asm__ __volatile__ ("int $0");
	__asm__ __volatile__ ("int $1");
	__asm__ __volatile__ ("int $2");
	__asm__ __volatile__ ("int $3");
	__asm__ __volatile__ ("int $4");
	__asm__ __volatile__ ("int $5");
	__asm__ __volatile__ ("int $6");
	__asm__ __volatile__ ("int $7");
	__asm__ __volatile__ ("int $9");
	__asm__ __volatile__ ("int $15");
	__asm__ __volatile__ ("int $16");
	__asm__ __volatile__ ("int $0x80");

	irq_init();
	time_init();
	__asm__ __volatile__ ("sti");
}

