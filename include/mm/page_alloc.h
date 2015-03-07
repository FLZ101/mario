#ifndef _PAGE_ALLOC_H
#define _PAGE_ALLOC_H

#include <misc.h>

#include <lib/atomic.h>
#include <lib/list.h>

struct page {
	struct list_head list;
	atomic_t count;
};

extern struct page *mem_map;

#define PAGE_TO_PFN(x)	(unsigned long)((x)-mem_map)
#define PAGE_TO_PHY(x)	(PAGE_TO_PFN(x) << PAGE_SHIFT)
#define PAGE_TO_VIR(x)	__vir(PAGE_TO_PHY(x))

#define PFN_TO_PAGE(x)	((unsigned long)(x)+mem_map)
#define PHY_TO_PAGE(x)	PFN_TO_PAGE((unsigned long)(x) >> PAGE_SHIFT)
#define VIR_TO_PAGE(x)	PHY_TO_PAGE(__phy(x))

#define set_page_count(p, v)	atomic_set(&(p)->count, v)

void free_list_print(unsigned long order);

void page_alloc_init(void);

struct page *alloc_page(void);
struct page *alloc_pages(unsigned long order);
unsigned long page_alloc(void);
unsigned long pages_alloc(unsigned long order);

void free_page(struct page *page);
void free_pages(struct page *page, unsigned long order);
void page_free(unsigned long vir);
void pages_free(unsigned long vir, unsigned long order);

void free_init_area(void);

#endif	/* _PAGE_ALLOC_H */