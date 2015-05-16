#ifndef _PAGE_ALLOC_H
#define _PAGE_ALLOC_H

#include <misc.h>

#include <lib/list.h>
#include <lib/atomic.h>

struct page {
	atomic_t count;
	struct list_head list;
};

extern struct page *mem_map;

#define PAGE_TO_PFN(x)	((unsigned long)((x)-mem_map))
#define PAGE_TO_PHY(x)	(PAGE_TO_PFN(x) << PAGE_SHIFT)
#define PAGE_TO_VIR(x)	__vir(PAGE_TO_PHY(x))

#define PFN_TO_PAGE(x)	((unsigned long)(x)+mem_map)
#define PHY_TO_PAGE(x)	PFN_TO_PAGE((unsigned long)(x) >> PAGE_SHIFT)
#define VIR_TO_PAGE(x)	PHY_TO_PAGE(__phy(x))

void page_alloc_init(void);

struct page *alloc_page(void);
struct page *alloc_pages(unsigned long order);
void free_page(struct page *page);
void free_pages(struct page *page, unsigned long order);

unsigned long page_alloc(void);
unsigned long pages_alloc(unsigned long order);
void page_free(unsigned long vir);
void pages_free(unsigned long vir, unsigned long order);

unsigned long zero_page_alloc(void);

struct page *__get_page(void);
void __put_page(struct page *page);

unsigned long get_zero_page(void);
static inline void put_page(unsigned long vir)
{
	__put_page(VIR_TO_PAGE(vir));
}

static inline void ref_page(struct page *page)
{
	atomic_inc(&page->count);
}

void free_init_area(void);


#endif	/* _PAGE_ALLOC_H */