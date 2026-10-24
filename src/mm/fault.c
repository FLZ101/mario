#include <mm/mm.h>

#include <task.h>
#include <trap.h>

#include <lib/string.h>

extern void die(char *str, struct trap_frame *tr, long err);
extern void do_exit(long code);
extern void print_mmap(struct mm_struct *mm);

/*
 * Serialises copy-on-write page faults that hit the same physical page.
 * Without this, two CPUs can both see page->count == 2, both decrement it,
 * and both allocate/copy a replacement.  The decrement/free race lets the
 * original page return to the buddy allocator while one CPU is still copying
 * from it, which corrupts private file-backed data pages when several
 * processes exec the same binary at once.
 */
static spinlock_t cow_lock = SPINLOCK_UNLOCKED;

/*
 * oom() prints a message (so that the user knows why the process died),
 * and gives the process an untrappable SIGKILL.
 */
void oom(struct task_struct *p)
{
	pr_warn("Out of memory for %s\n", current->comm);
	p->sigaction[SIGKILL-1].sa_handler = SIG_DFL;
	p->blocked &= ~(1<<(SIGKILL-1));
	send_sig(SIGKILL, p, 1);
}

void do_wp_page(struct vm_area_struct *vma, unsigned long addr,
		int write_access)
{
	pde_t *pd;
	pte_t *pt;
	unsigned long tmp, pg;
	unsigned long flags;
	struct page *page;

	pd = pde_offset(vma->vm_mm->pd, addr);
	pt = pte_offset(__vir(*pd), addr);

	if (vma->vm_flags & VM_SHARED) {
		set_pe_rw(pt);
		return;
	}
	pg = __vir((unsigned long)(*pt) & PAGE_MASK);
	page = VIR_TO_PAGE(pg);

	// printk("do_wp_page %x %d\n", addr, page->count.counter);

	/* There is only one using that page */
	if (atomic_read(&page->count) == 1) {
		set_pe_rw(pt);
		return;
	}

	spin_lock_irqsave(&cow_lock, flags);

	/* The page may have been copied by another CPU while we waited. */
	if (atomic_read(&page->count) == 1) {
		set_pe_rw(pt);
		spin_unlock_irqrestore(&cow_lock, flags);
		return;
	}

	tmp = get_zero_page();
	if (!tmp) {
		oom(current);
		*pt = BAD_PAGE;
		spin_unlock_irqrestore(&cow_lock, flags);
		return;
	}
	/*
	 * Copy the original page while this PTE still owns one reference to it.
	 * A concurrent exit/unmap on another CPU may drop its own reference at
	 * any time, but it cannot drop ours, so the source page cannot return to
	 * the buddy allocator until after the copy completes.  Only then is this
	 * PTE switched to the new page and its old reference released.
	 */
	memcpy((void *)tmp, (void *)pg, PAGE_SIZE);
	*pt = mk_pte(tmp, vma->vm_page_prot);
	__put_page(page);
	spin_unlock_irqrestore(&cow_lock, flags);
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
	unsigned long pte_prot;

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
		page = get_zero_page();
		if (!page)
			goto fail;
		*pt = mk_pte(page, vma->vm_page_prot);
		return;
	}
	/*
	 * Try to get the page we want from the share ring
	 */
	page = get_shared_page(vma, addr);
	if (!page)
		page = vma->vm_ops->nopage(vma, addr);
	if (!page)
		goto fail;

	/*
	 * A private writable file mapping starts read-only so the first write
	 * goes through the normal copy-on-write path instead of modifying the
	 * cached file page in place.
	 */
	pte_prot = vma->vm_page_prot;
	if (vma->vm_file && (vma->vm_flags & VM_WRITE) &&
	    !(vma->vm_flags & VM_SHARED))
		pte_prot &= ~PG_RW;

	*pt = mk_pte(page, pte_prot);
	return;
fail:
	if (vma->vm_file)
		send_sig(SIGBUS, current, 1);
	else
		oom(current);
	*pt = BAD_PAGE;
}

/*
 * error_code:
 *	bit 0: 0 - no page found, 1 - protection fault
 *	bit 1: 0 - read, 1 - write
 *	bit 2: 0 - kernel, 1 - user-mode
 */
void do_page_fault(struct trap_frame *tr, long error_code)
{
	unsigned long addr, page, pde, pte;
	struct vm_area_struct *vma;

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
	if (error_code & 1)
		do_wp_page(vma, addr, error_code & 2);
	else
		do_no_page(vma, addr, error_code & 2);
	flush_tlb();
	return;

bad_area:
	pr_debug("[page fault] %d %x; ", current->pid, addr);

	if (error_code & 4) {
		current->thread.cr2 = addr;
		current->thread.error_code = error_code;
		current->thread.trap_no = 14;
		send_sig(SIGSEGV, current, 1);
	}
	__asm__("movl %%cr3, %0" : "=r" (page));
	page = __vir(page);
	pde = ((unsigned long *) page)[addr >> 22];
	pr_debug("pde = %x ", pde);

	if (pde & 1) {
		pde &= PAGE_MASK;
		addr &= 0x003ff000;
		pde = __vir(pde);
		pte = ((unsigned long *) pde)[addr >> PAGE_SHIFT];
		pr_debug("pte = %x", pte);
	}
	pr_debug("\n");
	// print_mmap(current->mm);

	if (userland(tr)) {
		print_tr(tr);
		/* Let do_signal deliver SIGSEGV instead of force-killing the task. */
		return;
	} else {
		die("Oops", tr, error_code);
	}
}
