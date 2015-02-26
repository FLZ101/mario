#include <misc.h>

#include <lib/string.h>
#include <lib/bitops.h>
#include <lib/stddef.h>
#include <lib/list.h>

#include <mm/page_alloc.h>
#include <mm/bootmem.h>
#include <mm/e820.h>

#define MAX_ORDER 11

struct free_area {
	struct list_head free_list;
	unsigned long *map, map_size;
} free_area[MAX_ORDER];

unsigned long max_pfn __dinit;

struct page *mem_map;

#define __free_list_del(page) list_del(&(page)->list)

#define __free_list_add(page, order) \
	list_add_tail(&(page)->list, &free_area[order].free_list)

#define __free_list_entry(ptr) list_entry((ptr), struct page, list)

void free_list_print(unsigned long order)
{
	struct list_head *p;
	early_print("%u: ", order);
	list_for_each(p, &free_area[order].free_list) {
		early_print("%x | ", PAGE_TO_PHY(__free_list_entry(p)));
	}
	early_print("\n");
}

extern unsigned long end;
void __tinit page_alloc_init(void)
{
	unsigned long i, j;

	mem_map = __alloc_bootmem(max_pfn * sizeof(struct page), 2);

	for (i = 0; i < max_pfn; i++)
		atomic_set(&(mem_map + i)->count, 0);

	for (i = 0; i < MAX_ORDER; i++) {
		free_area[i].map_size = max_pfn >> (i + 1);

		unsigned long size = (free_area[i].map_size + 31) >> 5;
		if (size) {
			free_area[i].map = __alloc_bootmem(size * 4, 2);
			memsetl(free_area[i].map, 0, size);
		}

		INIT_LIST_HEAD(&free_area[i].free_list);
	}

	for (i = 0; i < e820.nr_map; i++) {
		unsigned long addr = e820.map[i].addr;
		unsigned long len = e820.map[i].len;

		if (0x100000 == addr) {
			addr = end;
			len = 0x100000 + len - end;
		}

		for (j = PFN_UP(addr); j < PFN_DOWN(addr + len); j++)
			free_page(mem_map + j);
	}
}

struct page *alloc_pages(unsigned long order)
{
	unsigned long flags, n;
	save_flags(flags);
	cli();

	for (n = order; list_empty(&free_area[n].free_list); n++)
		if (n == MAX_ORDER - 1)
			return NULL;

	struct page *page = __free_list_entry(free_area[n].free_list.next);
	unsigned long pfn = PAGE_TO_PFN(page);
	__free_list_del(page);
	while (1) {
		change_bit(pfn >> (n + 1), free_area[n].map);
		if (n == order)
			break;
		n--;
		__free_list_add(page + (1 << n), n);
	}
	restore_flags(flags);
	return page;
}

struct page *alloc_page(void)
{
	return alloc_pages(0);
}

unsigned long pages_alloc(unsigned long order)
{
	struct page *page = alloc_pages(order);

	if (page)
		return PAGE_TO_PHY(page);
	else
		return INVALID_PHY;
}

unsigned long page_alloc(void)
{
	struct page *page = alloc_page();
	
	if (page)
		return PAGE_TO_PHY(page);
	else
		return INVALID_PHY;
}

struct page *expand(struct page *page, unsigned long order)
{
	unsigned long nr = PAGE_TO_PFN(page) >> (order + 1) << (order + 1);
	struct page *res = PFN_TO_PAGE(nr);

	if (page == res)
		__free_list_del(page + (1 << order));
	else
		__free_list_del(page - (1 << order));

	return res;
}

void free_pages(struct page *page, unsigned long order)
{
	unsigned long flags;
	save_flags(flags);
	cli();

	while (1) {
		unsigned long nr = PAGE_TO_PFN(page) >> (order + 1);
		if (!test_and_change_bit(nr, free_area[order].map))
			break;
		if (order == MAX_ORDER - 1)
			break;
		page = expand(page, order);
		order++;
	}
	__free_list_add(page, order);

	restore_flags(flags);
}

void free_page(struct page *page)
{
	free_pages(page, 0);
}

void pages_free(unsigned long phy, unsigned long order)
{
	free_pages(PHY_TO_PAGE(phy), order);
}

void page_free(unsigned long phy)
{
	free_page(PHY_TO_PAGE(phy));
}

extern unsigned long _init, _bss;
void __tinit free_init_area(void)
{
	unsigned long i;
	for (i = PFN_UP(&_init); i < PFN_DOWN(&_bss); i++)
		free_page(mem_map + i);
}
