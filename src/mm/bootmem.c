#include <misc.h>

#include <mm/bootmem.h>

/*
 * Very simple boot-time physical memory allocator
 */

unsigned long end;

void *__alloc_bootmem(unsigned long size, unsigned long p2align)
{
	unsigned long tmp;

	tmp = (1UL << p2align) - 1;
	tmp = end = (end + tmp) & ~tmp;
	end = end + size;

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
