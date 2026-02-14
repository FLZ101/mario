#include <mm/e820.h>

#include <types.h>
#include <misc.h>

/*
 * Build an e820 map based on the information the bootloader passed to us
 */

struct e820map e820;

/*
 * Used to fake an e820 map if we don't get one from the bootloader
 */
#define LOWER_MEM	0x080000	/* 512 KB */
#define UPPER_MEM	0xe00000	/* 14 MB */

/*
 * Minimum physical memory our kernel needs
 */
#define MIN_MEMORY	0x800000	/* 8M */

/*
 * Maximum physical memory supported
 */
#define MAX_MEMORY  KERNEL_BASE

void __tinit add_memory_region(__u64 addr, __u64 len, __u32 type)
{
	int i;
	__u64 start = addr, end = addr + len;

	/*
	 * We only care about memory region of type E820_RAM
	 */
	if (E820_RAM != type || start >= end || start >= MAX_MEMORY)
		return;
	if (addr >= MAX_MEMORY)
		return;
	if (addr >= end)
		return;
	if (end > MAX_MEMORY)
		len = MAX_MEMORY - addr;

	if ((i = e820.nr_map) >= E820MAX) {
		printk("Too many entries in the memory map!\n");
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

	printk("Available memory:\n");
	printk("base\t\tlength\n");

	for (i = 0; i < e820.nr_map; i++) {
		/*
		 * :) Don't forget the (unsigned int)
		 */
		printk("%x\t%x\n", (unsigned int)e820.map[i].addr,
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
		return;
	else
		e820.nr_map = 0;

	if (MB_FLAG_MEM & m->flags) {
		add_memory_region(0, m->mem_lower, E820_RAM);
		add_memory_region(0x100000, m->mem_upper, E820_RAM);
	}

	if (e820.nr_map == 2)
		return;
	else
		e820.nr_map = 0;

	/*
	 * We did not get a reliable memory map, let's fake one
	 */
	add_memory_region(0, LOWER_MEM, E820_RAM);
	add_memory_region(0x100000, UPPER_MEM, E820_RAM);
}

extern void ramdisk_setup(struct multiboot_info *m);

extern unsigned long max_pfn;

void __tinit setup_memory_region(struct multiboot_info *m)
{
	int i;

	make_e820_map(m);

	print_e820_map();

	/*
	 * Find number of highest page frame of type E820_RAM
	 */
	max_pfn = 0;
	for (i = 0; i < e820.nr_map; i++) {
		unsigned long j, k;
		j = PFN_UP(e820.map[i].addr);
		k = PFN_DOWN(e820.map[i].addr + e820.map[i].len);
		if (j >= k)
			continue;
		if (k > max_pfn)
			max_pfn = k;
	}
	printk("max_pfn=%x\n", max_pfn);

	if (max_pfn < PFN_DOWN(MIN_MEMORY))
		hang("More physical memory required!\n");

	ramdisk_setup(m);
}
