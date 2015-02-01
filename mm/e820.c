#include <multiboot.h>
#include <types.h>
#include <misc.h>
#include <e820.h>

struct e820map __dinit e820;

void __tinit add_memory_region(__u64 addr, __u64 len, __u32 type)
{
	int i;
	__u64 start = addr, end = addr + len;

	/*
	 * We only care about memory region of type E820_RAM
	 */
	if (E820_RAM != type || start >= end || start >= MAX_MEMORY)
		return;

	if (end > MAX_MEMORY)
		len = MAX_MEMORY - addr;

	if ((i = e820.nr_map) >= E820MAX) {
		early_print("Too many entries in the memory map!\n");
		return;
	}

	e820.map[i].addr = addr;
	e820.map[i].len = len;
	e820.map[i].type = type;
	e820.nr_map++;
}

void __tinit print_e820_map(void)
{
	int i;

	early_print("Available memory:\n");
	early_print("base\t\tlength\n");

	for (i = 0; i < e820.nr_map; i++) {
		/*
		 * :) Don't forget the (unsigned int)
		 */
		early_print("%x\t%x\n", (unsigned int)e820.map[i].addr, 
			(unsigned int)e820.map[i].len);
	}
}

void __tinit make_e820_map(struct multiboot_info *m)
{
	e820.nr_map = 0;

	/*
	 * Did the bootloader give us a memory map?
	 */
	if (MB_FLAG_MMAP & m->flags) {
		struct multiboot_mmap_entry *e = 
				(struct multiboot_mmap_entry *)m->mmap_addr;

		while (m->mmap_length) {
			add_memory_region(e->addr, e->len, e->type);

			m->mmap_length -= e->size + 4;
			e = (struct multiboot_mmap_entry *)
				((char *)e + e->size + 4);
		}
	}
	
	/*
	 * A correct memory map should contain at least 2 memory regions 
	 * of type E820_RAM, one is 0 ~ somewhere below 1M, the other one 
	 * is 1M ~ XXOO
	 */
	if (e820.nr_map >= 2)
		goto done;
	else
		e820.nr_map = 0;

	if (MB_FLAG_MEM & m->flags) {
		add_memory_region(0, m->mem_lower, E820_RAM);
		add_memory_region(0x100000, m->mem_upper, E820_RAM);
	}

	if (e820.nr_map == 2)
		goto done;
	else
		e820.nr_map = 0;

	/*
	 * We did not get a reliable memory map, let's fake one
	 */
	add_memory_region(0, FAKE_LOWER_MEM, E820_RAM);
	add_memory_region(0x100000, FAKE_UPPER_MEM, E820_RAM);

done:
	print_e820_map();
}

