#include <multiboot.h>
#include <trap.h>
#include <time.h>
#include <misc.h>
#include <irq.h>

#include <mm/page_alloc.h>
#include <mm/e820.h>

void mario(struct multiboot_info *m)
{
	early_print_init(m);

	setup_memory_region(m);

	page_alloc_init();

	int i;
	for (i = 0; i < 5; i++)
		free_list_print(i);
	for (i = 0; i < 6; i++)
		early_print("%x, ", pages_alloc(2));
	early_print("\n");
	for (i = 0; i < 5; i++)
		free_list_print(i);
	
	/*
	trap_init();

	irq_init();
	time_init();
	__asm__ __volatile__ ("sti");
	*/
}

