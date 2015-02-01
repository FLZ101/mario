#include <multiboot.h>
#include <string.h>
#include <e820.h>
#include <misc.h>
#include <mm.h>

extern unsigned long rd_start, rd_end;

void __tinit mem_init(struct multiboot_info *m)
{
	make_e820_map(m);

	if (MB_FLAG_MODULE & m->flags && m->mods_count) {
		struct multiboot_module *mod = 
			(struct multiboot_module *)m->mods_addr;

		/*
		 * The first module should be initrd
		 */
		if (strstr((char *)mod->string, "MARIO")) {
			rd_start = mod->mod_start;
			rd_end = mod->mod_end;
		}
	}

	/*
	 * We assume that initrd is next to our kernel
	 */
	if (rd_start) {
		early_print("initrd:\n");
		early_print("rd_start=%x, rd_end=%x\n", rd_start, rd_end);
	} else {
		early_print("initrd not loaded!\n");
	}
}

