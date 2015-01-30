#include <types.h>
#include <multiboot.h>
#include <text.h>
#include <string.h>

void mario(__u32 eax, struct multiboot_info *ebx)
{
	init_text();

	if (eax != MB_MAGIC_EAX)
		while (1);

	char *cp = (char *)ebx->boot_loader_name;

	if (MB_FLAG_LOADER & ebx->flags && (0x3f00 == *(__u16 *)cp)) {
		set_cursor(*(unsigned char*)(cp + 2), *(unsigned char*)(cp + 3));
		printf("bootloader : %s\n", cp + 4);
	}
	else {
		clear();
	}

	if (MB_FLAG_MMAP & ebx->flags) {
		printf("There is a memory map : Base %x /Length %x\n", \
			ebx->mmap_addr, ebx->mmap_length);

		printf("base\t\tlength\t\ttype\n");

		struct multiboot_mmap_entry *map = (struct \
			multiboot_mmap_entry *)ebx->mmap_addr;

		while (ebx->mmap_length) {
			printf("%x\t%x\t%s\n", (unsigned int )map->addr,\
				(unsigned int)map->len, (map->type == 1)\
					? "Available" : "Reserved");

			ebx->mmap_length -= map->size + sizeof(unsigned int);

			cp = (char *)map;
			cp += map->size + sizeof(unsigned int);
			map = (struct multiboot_mmap_entry *)cp;
		}
	}

	if (MB_FLAG_MEM & ebx->flags)
		printf("Low %u KB/Upper %u KB\n", ebx->mem_lower, \
			ebx->mem_upper);

	if (MB_FLAG_MODULE & ebx->flags) {
		printf("%d module(s) in module list at %x\n", \
			ebx->mods_count, ebx->mods_addr);
		struct multiboot_mod_list *mod = (struct multiboot_mod_list \
			*)ebx->mods_addr;
		while (ebx->mods_count) {
			char *s = (char *)mod->string;
			s[7] = '\0';
			if (!strcmp("/initrd", s))
				printf("start: %x, end: %x\n\t%s\n", mod->mod_start, \
					mod->mod_end, (char *)mod->mod_start);
			ebx->mods_count--;
			mod++;
		}
	}
}

