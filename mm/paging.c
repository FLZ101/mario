#include <misc.h>

#include <mm/page_alloc.h>
#include <mm/bootmem.h>
#include <mm/paging.h>

extern unsigned long max_pfn;

/*
 * pagetable_init() sets up the page tables - note that the first 8MB
 * are already mapped by start.S
 */
void __tinit pagetable_init(void)
{
	pde_t *pd = swapper_pg_dir + __pde_offset(KERNEL_BASE);
	
	unsigned long pfn;
	for (pfn = 0; pfn < max_pfn; pd++) {
		if (*pd) {
			pfn += PTES_PER_PT;
			continue;
		}

		pte_t *pt = (pte_t *)alloc_page_bootmem();
		set_pde(pd, mk_pde(pt, _PDE_KERNEL));
		
		int i;
		for (i = 0; i < PTES_PER_PT; i++, pfn++, pt++)
			set_pte(pt, (pfn < max_pfn) ? 
				mk_pte(pfn*PAGE_SIZE, _PTE_KERNEL) : 0);
	}
}

void __tinit paging_init(void)
{
	pagetable_init();
	flush_tlb();
}


