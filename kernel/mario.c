#include <multiboot.h>
#include <misc.h>
#include <idt.h>

#include <mm/e820.h>

void mario(struct multiboot_info *m)
{
	early_print_init(m);

	setup_memory_region(m);

	isr_init();

	__asm__ __volatile__ ("int $0");
}

