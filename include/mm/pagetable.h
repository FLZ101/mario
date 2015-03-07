#ifndef _PAGETABLE_H
#define _PAGETABLE_H

typedef unsigned long pde_t;
typedef unsigned long pte_t;

#define __pde(x)	((pde_t)(x))
#define __pte(x)	((pte_t)(x))

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

#define _PDE_USER \
(PG_PRESENT | PG_USER | PG_RW)
#define _PDE_KERNEL \
(PG_PRESENT | PG_USER | PG_RW)

#define _PTE_USER \
(PG_PRESENT | PG_USER | PG_RW)
#define _PTE_KERNEL \
(PG_PRESENT)

#define __pde_offset(vir) \
(((unsigned long)(vir) >> PGDIR_SHIFT) & (PDES_PER_PD - 1))
#define pde_offset(pd, vir) \
(pd + __pde_offset(vir))

#define __pte_offset(vir) \
(((unsigned long)(vir) >> PAGE_SHIFT) & (PTES_PER_PT - 1))
#define pte_offset(pt, vir) \
(pt + __pte_offset(vir))

#define set_pde(p, val) *(p) = val
#define set_pte(p, val) *(p) = val

/*
 * 'phy' should be physical address of a page frame
 */
#define mk_pde(phy, attr) __pde((unsigned long)(phy) | (attr))
#define mk_pte(phy, attr) __pte((unsigned long)(phy) | (attr))

extern pde_t swapper_pg_dir[1024];

#endif	/* _PAGETABLE_H */