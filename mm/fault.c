#include <mm/mm.h>

#include <task.h>
#include <trap.h>

#include <lib/string.h>

/*
 * oom() prints a message (so that the user knows why the process died),
 * and gives the process an untrappable SIGKILL.
 */
void oom(struct task_struct *p)
{
	early_print("\nOut of memory for %s.\n", current->comm);
	p->sigaction[SIGKILL-1].sa_handler = NULL;
	p->blocked &= ~(1<<(SIGKILL-1));
	send_sig(SIGKILL, p, 1);
}

void do_wp_page(struct vm_area_struct *vma, unsigned long addr,
	int write_access)
{
	pde_t *pd;
	pte_t *pt;
	unsigned long tmp;
	struct page *page;

	pd = pde_offset(vma->vm_mm->pd, addr);
	pt = pte_offset(__vir(*pd), addr);

	if (vma->vm_flags & VM_SHARED) {
		set_pe_rw(pt);
		return;
	}
	page = VIR_TO_PAGE(*pt);

	/* There is only one using that page */
	if (atomic_read(&page->count) == 1) {
		set_pe_rw(pt);
		return;
	}
	__put_page(page);
	tmp = get_zero_page();
	if (!tmp) {
		oom(current);
		*pt = BAD_PAGE;
		return;
	}
	*pt = mk_pte(tmp, vma->vm_page_prot);
	memcpy(page, (void *)(*pt & PAGE_MASK), PAGE_SIZE);
}

static unsigned long get_one(struct vm_area_struct *mpnt, unsigned long addr)
{
	pde_t *pd;
	pte_t *pt;

	pd = pde_offset(mpnt->vm_mm->pd, addr);
	if (!*pd)
		return 0;
	pt = pte_offset(__vir(*pd), addr);
	if (__pe_present(pt) && !__pte_bad(pt))
		return (unsigned long)(*pt) & PAGE_MASK;
	return 0;

}

/*
 * This function should only be called when no-page fault
 */
static unsigned long get_shared_page(struct vm_area_struct *vma, unsigned long addr)
{
	struct vm_area_struct *mpnt;

	if (!(vma->vm_flags & VM_SHARED))
		return 0;
	for (mpnt = vma->vm_next_share; mpnt != vma; mpnt = mpnt->vm_next_share) {
		unsigned long tmp;
		if ((tmp = get_one(mpnt, addr - vma->vm_start + mpnt->vm_start))) {
			ref_page(VIR_TO_PAGE(tmp));	/* !!! */
			return tmp;
		}
	}
	return 0;
}

void do_no_page(struct vm_area_struct *vma, unsigned long addr,
	int write_access)
{
	pde_t *pd;
	pte_t *pt;
	unsigned long page;

	pd = pde_offset(vma->vm_mm->pd, addr);
	/*
	 * a pde can be a valid value or 0
	 */
	if (!*pd) {
		if (!(pt = alloc_pt()))
			return;
		*pd = mk_pde(pt, _PDE);
	}
	pt = pte_offset(__vir(*pd), addr);
	if (!vma->vm_ops || !vma->vm_ops->nopage) {
		unsigned long tmp = get_zero_page();
		if (!tmp)
			goto fail;
		*pt = mk_pte(tmp, vma->vm_page_prot);
		return;
	}
	/*
	 * Try to get the page we want from the share ring
	 */
	page = get_shared_page(vma, addr);
	if (page)
		goto ok_page;
	page = get_zero_page();
	if (!page)
		goto fail;
	if (vma->vm_ops->nopage(vma, addr, page)) {
		put_page(page);
		goto fail;
	}
ok_page:
	*pt = mk_pte(page, vma->vm_page_prot);
	return;
fail:
	oom(current);
	*pt = BAD_PAGE;
}

/*
 * This routine handles page faults.  It determines the address,
 * and the problem, and then passes it off to one of the appropriate
 * routines.
 *
 * error_code:
 *	bit 0 == 0 means no page found, 1 means protection fault
 *	bit 1 == 0 means read, 1 means write
 *	bit 2 == 0 means kernel, 1 means user-mode
 */
void do_page_fault(struct trap_frame tr)
{
	unsigned long error_code;
	unsigned long addr, page;
	struct vm_area_struct *vma;

	error_code = tr.error_code;
	__asm__("movl %%cr2, %0":"=r"(addr));

	vma = find_vma(current->mm, addr);
	if (!vma)
		goto bad_area;
	if (vma->vm_start <= addr)
		goto good_area;
	if (!(vma->vm_flags & VM_GROWSDOWN))
		goto bad_area;
	if (vma->vm_end - addr > current->rlim[RLIMIT_STACK].rlim_cur)
		goto bad_area;
	vma->vm_offset -= vma->vm_start - (addr & PAGE_MASK);
	vma->vm_start = (addr & PAGE_MASK);
/*
 * Ok, we have a good vm_area for this memory access, so
 * we can handle it..
 */
good_area:
	/*
	 * was it a write?
	 */
	if (error_code & 2) {
		if (!(vma->vm_flags & VM_WRITE))
			goto bad_area;
	} else {
		/* read with protection fault? */
		if (error_code & 1)
			goto bad_area;
		if (!(vma->vm_flags & (VM_READ | VM_EXEC)))
			goto bad_area;
	}
	if (error_code & 1) {
		do_wp_page(vma, addr, error_code & 2);
		return;
	}
	do_no_page(vma, addr, error_code & 2);
	return;

/*
 * Something tried to access memory that isn't in our memory map..
 * Fix it, but check if it's kernel or user first..
 */
bad_area:
	if (error_code & 4) {
		current->thread.cr2 = addr;
		current->thread.error_code = error_code;
		current->thread.trap_no = 14;
		send_sig(SIGSEGV, current, 1);
	}
	__asm__("movl %%cr3, %0" : "=r" (page));
	page = ((unsigned long *) page)[addr >> 22];
	early_print("*pde = %x\n", page);
	if (page & 1) {
		page &= PAGE_MASK;
		addr &= 0x003ff000;
		page = ((unsigned long *) page)[addr >> PAGE_SHIFT];
		early_print("*pte = %x\n", page);
	}
	print_tr(&tr);
	early_hang("KERNEL CRASH");
}
