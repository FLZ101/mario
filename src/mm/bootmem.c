#include <misc.h>

#include <mm/bootmem.h>

/*
 * Very simple boot-time physical memory allocator
 */

// Reserve 8M boot-time memory (used before page tables are completely setup).
// Note that the whole kernel image (code and data, not including ramdisks)
// must be inside the first 16M, since we only setup mappings for 0~16M in start.S

#define BOOTMEM_SIZE (8 * 1024 * 1024)

__attribute__((section(".bss.boot")))
static unsigned char bootmem[BOOTMEM_SIZE];

unsigned long bootmem_j = (unsigned long) bootmem;
unsigned long bootmem_end = (unsigned long) bootmem + BOOTMEM_SIZE;

void *__alloc_bootmem(unsigned long size, unsigned long p2align)
{
	unsigned long tmp;

	tmp = (1UL << p2align) - 1;
	tmp = bootmem_j = (bootmem_j + tmp) & ~tmp;
	bootmem_j = bootmem_j + size;

	if (bootmem_j > bootmem_end)
		hang("Boot-time memory is exhausted!");

	return (void *)tmp;
}

void *alloc_bootmem(unsigned long size)
{
	return __alloc_bootmem(size, 0);
}

void *alloc_page_bootmem(void)
{
	return __alloc_bootmem(PAGE_SIZE, PAGE_SHIFT);
}
