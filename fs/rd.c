#include <lib/string.h>

#include <multiboot.h>
#include <misc.h>

#define MAX_RD 3

struct {
	unsigned long start;
	unsigned long end;
} rd_info[MAX_RD];

extern unsigned long end;

void __tinit rd_init(struct multiboot_info *m)
{
	int i, j;
	for (i = 0; i < MAX_RD; i++)
		rd_info[i].end = 0;

	end = 0;

	if (!(MB_FLAG_MODULE & m->flags))
		goto tail;

	struct multiboot_module *mod = 
		(struct multiboot_module *)m->mods_addr;
	for (i = 0, j = 0; i < MAX_RD && j < m->mods_count; j++) {
		if (!strstr((char *)mod[j].string, "MARIO_RAMDISK"))
			continue;

		rd_info[i].start = mod[j].mod_start;
		rd_info[i].end = mod[j].mod_end;

		if (rd_info[i].end > end)
			end = rd_info[i].end;
		i++;
	}
tail:
	if (!end)
		early_hang("no ramdisk loaded!\n");

	early_print("ramdisk(s):\n");
	for (i = 0; i < MAX_RD && rd_info[i].end; i++)
		early_print("start=%x, end=%x\n", 
			rd_info[i].start, rd_info[i].end);
}

