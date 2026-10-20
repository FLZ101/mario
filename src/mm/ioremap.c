#include <misc.h>

#include <mm/ioremap.h>
#include <mm/page_alloc.h>
#include <mm/pagetable.h>
#include <mm/mm.h>

#include <lib/stddef.h>

#define IOREMAP_BASE	0xf0000000UL

static unsigned long iomap_virt;
static unsigned long iomap_size;

/*
 * MMIO mappings are not backed by normal RAM, so this is a small identity
 * helper that installs kernel-only present pages into the swapper page table.
 * The driver currently needs only one AHCI BAR, hence the single active
 * mapping restriction.
 */
void *ioremap(unsigned long phys, size_t size)
{
	unsigned long virt;
	unsigned long end;
	unsigned long page_phys;
	unsigned long page_virt;

	if (!phys || !size)
		return NULL;
	if (iomap_virt)
		return NULL;

	phys &= PAGE_MASK;
	size = (size + PAGE_SIZE - 1) & PAGE_MASK;
	virt = IOREMAP_BASE;
	end = virt + size;

	for (page_phys = phys, page_virt = virt;
	     page_virt < end;
	     page_phys += PAGE_SIZE, page_virt += PAGE_SIZE) {
		pde_t *pde = pde_offset(swapper_pg_dir, page_virt);
		pte_t *pt;
		pte_t *pte;

		if (!__pe_present(pde)) {
			pt = (pte_t *)alloc_pt();
			if (!pt)
				return NULL;
			set_pde(pde, mk_pde(pt, _PDE));
		} else {
			pt = pte_offset(__vir(*pde), page_virt);
		}

		pte = pte_offset(pt, page_virt);
		set_pte(pte, __mk_pte(page_phys, PG_PRESENT | PG_RW));
	}

	flush_tlb();
	iomap_virt = virt;
	iomap_size = end - virt;
	return (void *)virt;
}

void iounmap(void *virt)
{
	unsigned long address = (unsigned long)virt;

	if (address != iomap_virt)
		return;

	for (unsigned long page = address; page < address + iomap_size;
	     page += PAGE_SIZE) {
		pde_t *pde = pde_offset(swapper_pg_dir, page);

		if (__pe_present(pde)) {
			pte_t *pt = pte_offset(__vir(*pde), page);
			pte_t *pte = pte_offset(pt, page);

			set_pte(pte, 0);
		}
	}

	flush_tlb();
	iomap_virt = 0;
	iomap_size = 0;
}
