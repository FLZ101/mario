#include <multiboot.h>
#include <trap.h>
#include <misc.h>
#include <irq.h>

#include <mm/e820.h>

void mario(struct multiboot_info *m)
{
	early_print_init(m);

	setup_memory_region(m);

	trap_init();
	__asm__ __volatile__ ("int $0");

	irq_init();
	__asm__ __volatile__ ("sti");
}

