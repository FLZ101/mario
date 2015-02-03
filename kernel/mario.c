#include <multiboot.h>
#include <string.h>
#include <misc.h>

#include <mm/mm.h>

void mario(struct multiboot_info *m)
{
	early_print_init(m);

	setup_memory_region(m);
}

