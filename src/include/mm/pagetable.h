#ifndef _PAGETABLE_H
#define _PAGETABLE_H

#include <misc.h>

#define alloc_pd()	(pde_t *)zero_page_alloc()
#define alloc_pt()	(pte_t *)get_zero_page()

#define free_pd(pd)	page_free((unsigned long)(pd))
#define free_pt(pt)	__put_page(PHY_TO_PAGE(pt))

#define PGDIR_SHIFT	22
#define PGDIR_SIZE	(1UL << PGDIR_SHIFT)
#define PGDIR_MASK	(~(PGDIR_SIZE-1))

#define PDES_PER_PD	1024
#define PTES_PER_PT	1024

#define PG_RW 0x002
#define PG_USER 0x004
#define PG_DIRTY 0x040
#define PG_PRESENT 0x001
#define PG_ACCESSED 0x020

#define _PDE (PG_PRESENT | PG_USER | PG_RW)

#define _PTE_KERNEL (PG_PRESENT)

typedef unsigned long pde_t;
typedef unsigned long pte_t;

#define __pde(x)	(x)
#define __pte(x)	(x)

#define set_pde(p, val) *(p) = (val)
#define set_pte(p, val) *(p) = (val)

/*
 * @phy: physical address of a page frame
 */
#define __mk_pde(phy, attr) __pde((unsigned long)(phy) | (attr))
#define __mk_pte(phy, attr) __pte((unsigned long)(phy) | (attr))

#define mk_pde(vir, attr) __mk_pde(__phy(vir), attr)
#define mk_pte(vir, attr) __mk_pte(__phy(vir), attr)

#define __pe_val(p)		(*(p))	
#define __pe_attr(p)		(*(p) & ~PAGE_MASK)

#define __pe_rw(p)		(*(p) & PG_RW)
#define __pe_present(p)		(*(p) & PG_PRESENT)

#define set_pe_rw(p)		*(p) |= PG_RW
#define set_pe_present(p)	*(p) |= PG_PRESENT

#define clear_pe_rw(p)		*(p) &= ~PG_RW
#define clear_pe_present(p)	*(p) &= ~PG_PRESENT

#define __pde_offset(vir) \
(((unsigned long)(vir) >> PGDIR_SHIFT) & (PDES_PER_PD - 1))
#define pde_offset(pd, vir) \
((pde_t *)(((unsigned long)(pd) & PAGE_MASK) + __pde_offset(vir) * 4))

#define __pte_offset(vir) \
(((unsigned long)(vir) >> PAGE_SHIFT) & (PTES_PER_PT - 1))
#define pte_offset(pt, vir) \
((pte_t *)(((unsigned long)(pt) & PAGE_MASK) + __pte_offset(vir) * 4))

extern pde_t swapper_pg_dir[1024];
extern pte_t empty_zero_page[1024];
extern pte_t empty_bad_page_table[1024];
extern char empty_bad_page[PAGE_SIZE];

pte_t __bad_page(void);
pde_t __bad_pagetable(void);

#define BAD_PAGETABLE __bad_pagetable()
#define BAD_PAGE __bad_page()
#define ZERO_PAGE ((unsigned long) empty_zero_page)

#define __pte_bad(p) (((*p) & PAGE_MASK) == (unsigned long)empty_bad_page)

#endif	/* _PAGETABLE_H */