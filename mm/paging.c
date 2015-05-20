#include <misc.h>
#include <errno.h>
#include <task.h>

#include <mm/page_alloc.h>
#include <mm/bootmem.h>
#include <mm/mm.h>

#define SET_PAGE_DIR(p, x) \
do { \
	(p)->mm->pd = (pde_t *)(x); \
	if ((p) == current) \
		switch_pd(x); \
} while (0)

extern unsigned long max_pfn;

/*
 * pagetable_init() sets up the page tables - note that the first 8MB
 * are already mapped by start.S
 */
void __tinit pagetable_init(void)
{
	unsigned long pfn;
	pde_t *pd;
	pte_t *pt;

	/* unmap 0~8M */
	pd = swapper_pg_dir;
	pd[0] = 0;
	pd[1] = 0;

	for (pd += __pde_offset(KERNEL_BASE), pfn = 0; pfn < max_pfn; pd++) {
		if (*pd) {
			pfn += PTES_PER_PT;
			continue;
		}
		pt = (pte_t *)alloc_page_bootmem();
		set_pde(pd, mk_pde(pt, _PDE));

		int i;
		for (i = 0; i < PTES_PER_PT && pfn < max_pfn; i++, pfn++, pt++)
			set_pte(pt, __mk_pte(pfn*PAGE_SIZE, _PTE_KERNEL));
	}
}

void __tinit paging_init(void)
{
	pagetable_init();
	flush_tlb();
}

static inline void copy_one_pte(pte_t *old_pte, pte_t *new_pte)
{
	if (!*old_pte)
		return;

	if (__pe_present(old_pte)) {
		ref_page(PHY_TO_PAGE(*old_pte));
		clear_pe_rw(old_pte);
	}

	*new_pte = *old_pte;
}

static inline int copy_one_pde(pde_t *old_pde, pde_t *new_pde)
{
	int i;
	pte_t *old_pt, *new_pt;

	if (!*old_pde)
		return 0;
	if (!__pe_present(old_pde)) {
		*new_pde = *old_pde;
		return 0;
	}
	if (!(new_pt = alloc_pt()))
		return -ENOMEM;
	set_pde(new_pde, mk_pde(new_pt, __pe_attr(old_pde)));

	old_pt = pte_offset(__vir(*old_pde), 0);
	for (i = 0; i < PTES_PER_PT; i++, old_pt++, new_pt++)
		copy_one_pte(old_pt, new_pt);
	return 0;
}

void free_page_tables(struct task_struct *);
int copy_page_tables(struct task_struct *p)
{
	int i, error;
	pde_t *old_pd, *new_pd;

	if (!(new_pd = alloc_pd()))
		return -ENOMEM;
	p->mm->pd = new_pd;

	old_pd = current->mm->pd;
	for (i = 0; i < PDES_PER_PD; i++, old_pd++, new_pd++) {
		/* into the kernel space? */
		if (i >= __pde_offset(KERNEL_BASE)) {
			*new_pd = *old_pd;
			continue;
		}
		error = copy_one_pde(old_pd, new_pd);
		if (error) {
			free_page_tables(p);
			return error;
		}
	}
	return 0;
}

static inline void free_one_pte(pte_t *pte)
{
	if (!*pte)
		return;

	if (__pe_present(pte)) {
		__put_page(PHY_TO_PAGE(*pte));
		*pte = 0;
	}
}

static void free_one_pde(pde_t *pde)
{
	int i;
	pte_t *pt;

	if (!*pde)
		return;
	if (!__pe_present(pde))
		return;
	pt = pte_offset(__vir(*pde), 0);
	set_pde(pde, 0);

	for (i = 0; i < PTES_PER_PT; i++)
		free_one_pte(pt + i);
	free_pt(pt);
}

void free_page_tables(struct task_struct *p)
{
	int i;
	pde_t *pd;

	pd = p->mm->pd;
	SET_PAGE_DIR(p, swapper_pg_dir);

	for (i = 0; i < PDES_PER_PD; i++) {
		/* into the kernel space? */
		if (i >= __pde_offset(KERNEL_BASE)) {
			pd[i] = 0;
			continue;
		}
		free_one_pde(pd + i);
	}
	free_pd(pd);
	flush_tlb();
}

void clear_page_tables(struct task_struct *p)
{
	int i;
	pde_t *pd;

	pd = p->mm->pd;
	for (i = 0; i < __pde_offset(KERNEL_BASE); i++)
		free_one_pde(pd + i);
	flush_tlb();
}

pte_t __bad_page(void)
{
	__asm__ __volatile__(
		"cld ; rep ; stosl"
		:
		:"a" (0),
		 "D" ((long) empty_bad_page),
		 "c" (1024)
		:);
	return mk_pte(empty_bad_page, PG_RW | PG_USER | PG_PRESENT);
}

pde_t __bad_pagetable(void)
{
	__asm__ __volatile__(
		"cld ; rep ; stosl"
		:
		:"a" (BAD_PAGE),
		 "D" ((long) empty_bad_page_table),
		 "c" (1024)
		:);
	return mk_pde(empty_bad_page_table, PG_RW | PG_USER | PG_PRESENT);
}

extern void oom(struct task_struct *);

/*
 * Map a page into an address space: needed by do_execve() for the
 * initial stack and environment pages
 */
int set_page(struct mm_struct *mm, unsigned long page, unsigned long addr)
{
	pde_t *pd;
	pte_t *pt;

	pd = pde_offset(mm->pd, addr);
	if (!__pe_present(pd)) {
		pt = alloc_pt();
		if (!pt) {
			oom(current);
			return 1;
		}
		set_pde(pd, mk_pde(pt, _PDE));
	}
	pt = pte_offset(__vir(*pd), addr);
	set_pte(pt, mk_pte(page, _PDE));
	return 0;
}