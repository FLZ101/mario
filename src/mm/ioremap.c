#include <lib/stddef.h>
#include <mm/ioremap.h>
#include <mm/mm.h>
#include <mm/page_alloc.h>
#include <mm/pagetable.h>

#define IOREMAP_MAX_MAPPINGS 16

static struct {
	unsigned long address;
	unsigned int first;
	unsigned int pages;
} mappings[IOREMAP_MAX_MAPPINGS];
static pte_t *mapping_table;
static int sealed;

void *ioremap(unsigned long phys, size_t size)
{
	unsigned long offset = phys & ~PAGE_MASK;
	unsigned int first = 0, free_pages = 0;
	unsigned int pages, index, slot;

	if (sealed || !phys || !size || size - 1 > ~phys ||
	    size > PGDIR_SIZE - offset)
		return NULL;
	pages = (offset + size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	for (slot = 0; slot < IOREMAP_MAX_MAPPINGS; slot++)
		if (!mappings[slot].address)
			break;
	if (slot == IOREMAP_MAX_MAPPINGS)
		return NULL;

	if (!mapping_table) {
		pde_t *directory = pde_offset(swapper_pg_dir, IOREMAP_BASE);

		if (*directory)
			return NULL;
		mapping_table = alloc_pt();
		if (!mapping_table)
			return NULL;
		set_pde(directory, mk_pde(mapping_table, PG_PRESENT | PG_RW));
	}

	for (index = 0; index < PTES_PER_PT; index++) {
		if (mapping_table[index]) {
			free_pages = 0;
			first = index + 1;
		} else if (++free_pages == pages) {
			break;
		}
	}
	if (free_pages != pages)
		return NULL;

	phys &= PAGE_MASK;
	for (index = 0; index < pages; index++, phys += PAGE_SIZE)
		set_pte(mapping_table + first + index,
			__mk_pte(phys, PG_PRESENT | PG_RW | PG_PCD | PG_PWT));
	flush_tlb();
	mappings[slot].first = first;
	mappings[slot].pages = pages;
	mappings[slot].address = IOREMAP_BASE + first * PAGE_SIZE + offset;
	return (void *)mappings[slot].address;
}

void iounmap(void *virt)
{
	if (sealed || !virt)
		return;
	for (unsigned int slot = 0; slot < IOREMAP_MAX_MAPPINGS; slot++) {
		if (mappings[slot].address != (unsigned long)virt)
			continue;
		for (unsigned int index = 0; index < mappings[slot].pages; index++)
			set_pte(mapping_table + mappings[slot].first + index, 0);
		mappings[slot].address = 0;
		flush_tlb();
		return;
	}
}

void ioremap_seal(void)
{
	sealed = 1;
}
