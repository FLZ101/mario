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
	unsigned long *map; // buddy bitmap. 0: there is no buddy; 1: there is a buddy
	unsigned long map_size; // number of buddies
} free_area[MAX_ORDER];

unsigned long max_pfn;

struct page *mem_map;

#define __free_list_del(page) list_del(&(page)->list)

#define __free_list_add(page, order) do { \
	assert(PAGE_TO_PFN(page) < max_pfn); \
	list_add_tail(&(page)->list, &free_area[order].free_list); \
} while (0)

#define __free_list_entry(ptr) list_entry((ptr), struct page, list)

void print_free_area()
{
	int order;
	struct list_head *pos;

	for (order = 0; order < MAX_ORDER; ++order) {
		int size = 0;
		list_for_each(pos, &free_area[order].free_list) {
			++size;
		}
		printk("(%d %d) ", order, size);
	}
	printk("\n");
}

int get_free_area_size()
{
	int n = 0;
	int order;
	struct list_head *pos;

	for (order = 0; order < MAX_ORDER; ++order) {
		list_for_each(pos, &free_area[order].free_list) {
			n += (1 << order);
		}
	}
	return n;
}

extern unsigned long last_rd_end;

extern unsigned long bootmem_j;
extern unsigned long bootmem_end;

void page_alloc_init(void)
{
	unsigned long i, j;

	mem_map = __alloc_bootmem(max_pfn * sizeof(struct page), 2);

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

		// skip address 0
		if (!addr)
			addr = 1;

		// skip ramdisks
		if (0x100000 == addr) {
			addr = last_rd_end - KERNEL_BASE;
			len = 0x100000 + len - addr;
		}

		for (j = PFN_UP(addr); j < PFN_DOWN(addr + len); j++)
			free_page(mem_map + j);
	}

	// free unused boot-time memory
	for (j = PFN_UP(bootmem_j - KERNEL_BASE); j < PFN_DOWN(bootmem_end - KERNEL_BASE); j++)
		free_page(mem_map + j);
}

struct page *alloc_pages(unsigned long order)
{
	unsigned long n;
	irq_save();
	// int free_before = get_free_area_size();
	// int free_expected = free_before - (1 << order);

	for (n = order; list_empty(&free_area[n].free_list); n++) {
		if (n == MAX_ORDER - 1) {
			irq_restore();
			return NULL;
		}
	}

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

	// assert(free_expected == get_free_area_size());
	irq_restore();
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
		return PAGE_TO_VIR(page);
	return 0;
}

unsigned long page_alloc(void)
{
	return pages_alloc(0);
}

unsigned long zero_page_alloc(void)
{
	unsigned long tmp;

	if ((tmp = page_alloc()))
		memset((void *)tmp, 0, PAGE_SIZE);
	return tmp;
}

struct page *expand(struct page *page, unsigned long order)
{
	// lower page number of the pair
	unsigned long nr = PAGE_TO_PFN(page) >> (order + 1) << (order + 1);
	struct page *res = PFN_TO_PAGE(nr);

	// remove its buddy from list
	if (page == res) {
		// @page is the lower
		__free_list_del(page + (1 << order));
	} else {
		// @page is the higher
		__free_list_del(page - (1 << order));
	}

	return res;
}

void free_pages(struct page *page, unsigned long order)
{
	irq_save();
	// int free_before = get_free_area_size();
	// int free_expected = free_before + (1 << order);

	while (1) {
		unsigned long nr = PAGE_TO_PFN(page) >> (order + 1); // buddy number
		// if there is no buddy (i.e. the bit is 0), the bit is set, and we insert @page into this list;
		// else the bit is cleared, and @page and the buddy will be in the next list
		if (!test_and_change_bit(nr, free_area[order].map))
			break;
		if (order == MAX_ORDER - 1)
			break;
		page = expand(page, order);
		order++;
	}
	__free_list_add(page, order);

	// assert(free_expected == get_free_area_size());
	irq_restore();
}

void free_page(struct page *page)
{
	free_pages(page, 0);
}

void pages_free(unsigned long vir, unsigned long order)
{
	free_pages(VIR_TO_PAGE(vir), order);
}

void page_free(unsigned long vir)
{
	assert(PAGE_ALIGNED(vir));
	free_page(VIR_TO_PAGE(vir));
}

struct page *__get_page(void)
{
	struct page *page;

	if ((page = alloc_page()))
		atomic_set(&page->count, 1);
	return page;
}

void __put_page(struct page *page)
{
	if (atomic_dec_and_test(&page->count))
		free_page(page);
}

unsigned long get_zero_page(void)
{
	unsigned long tmp = 0;
	struct page *page;

	if ((page = __get_page())) {
		tmp = PAGE_TO_VIR(page);
		memset((void *)tmp, 0, PAGE_SIZE);
	}
	return tmp;
}
