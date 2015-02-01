#include <multiboot.h>
#include <string.h>
#include <misc.h>
#include <mm.h>

void mario(struct multiboot_info *m)
{
	early_print_init(m);

	mem_init(m);
}

