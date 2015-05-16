#include <task.h>
#include <misc.h>

#include <mm/mm.h>
#include <mm/mman.h>
#include <mm/kmalloc.h>

#include <fs/fs.h>

#include <lib/stddef.h>

struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
	struct vm_area_struct *vma;

	for (vma = mm->mmap ; ; vma = vma->vm_next) {
		if (!vma)
			return NULL;
		if (vma->vm_end > addr)
			return vma;
	}
}

struct vm_area_struct *find_vma_intersection(struct mm_struct *mm,
	unsigned long start_addr, unsigned long end_addr)
{
	struct vm_area_struct *vma;

	vma = find_vma(mm, start_addr);
	if (!vma || end_addr <= vma->vm_start)
		return NULL;
	return vma;
}

void insert_vm_struct(struct mm_struct *mm, struct vm_area_struct *vma)
{
	struct vm_area_struct **p, *mpnt;

	p = &mm->mmap;
	while ((mpnt = *p) != NULL) {
		if (mpnt->vm_start > vma->vm_start)
			break;
		if (mpnt->vm_end > vma->vm_start)
			early_print("%s: overlapping vm_areas\n", __FUNCTION__);
		p = &mpnt->vm_next;
	}
	vma->vm_next = mpnt;
	*p = vma;
}

void remove_shared_vm_struct(struct vm_area_struct *vma)
{
	vma->vm_next_share->vm_prev_share = vma->vm_prev_share;
	vma->vm_prev_share->vm_next_share = vma->vm_next_share;
	vma->vm_next_share = vma;
	vma->vm_prev_share = vma;
}

void exit_mmap(struct task_struct *t)
{
	struct vm_area_struct *vma, *next;

	vma = t->mm->mmap;
	t->mm->mmap = NULL;
	while (vma) {
		next = vma->vm_next;
		if (vma->vm_ops && vma->vm_ops->close)
			vma->vm_ops->close(vma);
		remove_shared_vm_struct(vma);
		if (vma->vm_inode)
			iput(vma->vm_inode);
		kfree(vma);
		vma = next;
	}
}

int dup_mmap(struct task_struct *t)
{
	struct vm_area_struct *vma, *tmp, **p;

	p = &t->mm->mmap;
	*p = NULL;
	for (vma = current->mm->mmap; vma; vma = vma->vm_next) {
		tmp = (struct vm_area_struct *)kmalloc(sizeof(struct vm_area_struct));
		if (!tmp) {
			exit_mmap(t);
			return -ENOMEM;
		}
		*tmp = *vma;
		tmp->vm_mm = t->mm;
		tmp->vm_next = NULL;
		if (tmp->vm_inode)
			iref(tmp->vm_inode);
		/* a shared area? */
		if (tmp->vm_flags & VM_SHARED) {
			tmp->vm_next_share->vm_prev_share = tmp;
			vma->vm_next_share = tmp;
			tmp->vm_prev_share = vma;
		} else {
			tmp->vm_next_share = tmp->vm_prev_share = tmp;
		}
		if (tmp->vm_ops && tmp->vm_ops->open)
			tmp->vm_ops->open(tmp);
		*p = tmp;
		p = &tmp->vm_next;
	}
	return 0;
}

/*
 * By the time this function is called, the area struct has been
 * removed from the process mapping list, so it needs to be
 * reinserted if necessary.
 *
 * The 4 main cases are:
 *    Unmapping the whole area
 *    Unmapping from the start of the segment to a point in it
 *    Unmapping from an intermediate point to the end
 *    Unmapping between to intermediate points, making a hole.
 *
 * Case 4 involves the creation of 2 new areas, for each side of
 * the hole.
 */
void unmap_fixup(struct vm_area_struct *vma, 
	unsigned long addr, unsigned long len)
{
	struct vm_area_struct *mpnt;
	unsigned long end = addr + len;

	/* Unmapping the whole area */
	if (addr == vma->vm_start && end == vma->vm_end) {
		if (vma->vm_ops && vma->vm_ops->close)
			vma->vm_ops->close(vma);
		if (vma->vm_inode)
			iput(vma->vm_inode);
		return;
	}
	/* Work out to one of the ends */
	if (end == vma->vm_end) {
		vma->vm_end = addr;
	} else if (addr == vma->vm_start) {
		vma->vm_offset += (end - vma->vm_start);
		vma->vm_start = end;
	} else {
		mpnt = (struct vm_area_struct *)kmalloc(sizeof(*mpnt));
		if (!mpnt)
			return;
		*mpnt = *vma;
		mpnt->vm_offset += (end - vma->vm_start);
		mpnt->vm_start = end;
		if (mpnt->vm_inode)
			iref(mpnt->vm_inode);
		if (mpnt->vm_ops && mpnt->vm_ops->open)
			mpnt->vm_ops->open(mpnt);
		vma->vm_end = addr;	/* Truncate area */
		insert_vm_struct(current->mm, mpnt);
	}
	/* construct whatever mapping is needed */
	mpnt = (struct vm_area_struct *)kmalloc(sizeof(*mpnt));
	if (!mpnt)
		return;
	*mpnt = *vma;
	if (mpnt->vm_ops && mpnt->vm_ops->open)
		mpnt->vm_ops->open(mpnt);
	if (vma->vm_ops && vma->vm_ops->close) {
		vma->vm_end = vma->vm_start;
		vma->vm_ops->close(vma);
	}
	insert_vm_struct(current->mm, mpnt);
}

void unmap_pte_range(pde_t *pd, unsigned long addr, unsigned long len)
{
	pte_t *pt;
	unsigned long end;

	if (!__pe_present(pd))
		return;
	pt = pte_offset(__vir(*pd), addr);
	addr &= ~PGDIR_MASK;
	end = addr + len;
	if (end >= PGDIR_SIZE)
		end = PGDIR_SIZE;
	do {
		pte_t tmp = *pt;
		*pt = 0;
		if (__pe_present(&tmp) && !__pte_bad(&tmp)) {
			__put_page(PHY_TO_PAGE(tmp));
		}
		addr += PAGE_SIZE;
		pt++;
	} while (addr < len);
}

int unmap_page_range(unsigned long addr, unsigned long len)
{
	pde_t *pd;
	unsigned long end = addr + len;

	pd = pde_offset(current->mm->pd, addr);
	while (addr < end) {
		unmap_pte_range(pd, addr, end - addr);
		addr = (addr + PGDIR_SIZE) & PGDIR_MASK;
		pd++;
	}
	flush_tlb();
	return 0;
}

/*
 * Munmap is split into 2 main parts -- this part which finds
 * what needs doing, and the areas themselves, which do the
 * work.  This now handles partial unmappings.
 * Jeremy Fitzhardine <jeremy@sw.oz.au>
 */
int do_munmap(unsigned long addr, unsigned long len)
{
	struct vm_area_struct *vma, *free, **p;

	if ((addr & ~PAGE_MASK) || addr > KERNEL_BASE || len > KERNEL_BASE-addr)
		return -EINVAL;
	if ((len = PAGE_ALIGN(len)) == 0)
		return 0;
	/*
	 * Check if this memory area is ok - put it on the temporary
	 * list if so..  The checks here are pretty simple --
	 * every area affected in some way (by any overlap) is put
	 * on the list.  If nothing is put on, nothing is affected.
	 */
	free = NULL;
	for (vma = current->mm->mmap, p = &current->mm->mmap; vma; ) {
		if (vma->vm_start >= addr+len)
			break;
		if (vma->vm_end <= addr) {
			vma = vma->vm_next;
			continue;
		}
		*p = vma->vm_next;
		vma->vm_next = free;
		free = vma;
		vma = *p;
	}
	if (!free)
		return 0;
	/*
	 * Ok - we have the memory areas we should free on the 'free' list,
	 * so release them, and unmap the page range..
	 * If the one of the segments is only being partially unmapped,
	 * it will put new vm_area_struct(s) into the address space.
	 */
	while (free) {
		unsigned long st, end;

		vma = free;
		free = free->vm_next;

		remove_shared_vm_struct(vma);

		st = addr < vma->vm_start ? vma->vm_start : addr;
		end = addr+len;
		end = end > vma->vm_end ? vma->vm_end : end;

		if (vma->vm_ops && vma->vm_ops->unmap)
			vma->vm_ops->unmap(vma, st, end-st);

		unmap_fixup(vma, st, end-st);
		kfree(vma);
	}
	unmap_page_range(addr, len);
	return 0;
}

int sys_munmap(unsigned long addr, unsigned long len)
{
	return do_munmap(addr, len);
}

unsigned long get_unmapped_area(unsigned long addr, unsigned long len)
{
	struct vm_area_struct * vma;

	if (len > KERNEL_BASE)
		return 0;
	if (!addr)
		addr = KERNEL_BASE / 2;	/* Is it OK? */
	addr = PAGE_ALIGN(addr);

	for (vma = current->mm->mmap; ; vma = vma->vm_next) {
		if (KERNEL_BASE - len < addr)
			return 0;
		if (!vma)
			return addr;
		if (addr > vma->vm_end)
			continue;
		if (addr + len > vma->vm_start) {
			addr = vma->vm_end;
			continue;
		}
		return addr;
	}
}
#if 0
static inline void 
zeromap_pde_range(pde_t *pd, unsigned long addr, unsigned long len, pte_t zero_pte)
{
	pte_t *pt;
	unsigned long end;

	pt = pte_offset(__vir(*pd), addr);
	addr &= ~PGDIR_MASK;
	end = addr + len;
	if (end >= PGDIR_SIZE)
		end = PGDIR_SIZE;
	do {
		*pt = zero_pte;
		addr += PAGE_SIZE;
		pt++;
	} while (addr < end);
}

int zeromap_page_range(unsigned long addr, unsigned long len, unsigned long prot)
{
	int error;
	pde_t *pd;
	pte_t zero_pte;
	unsigned long end;

	pd = pde_offset(current->mm->pd, addr);
	zero_pte = mk_pte(ZERO_PAGE, prot & ~PG_RW);
	while (addr < end) {
		pte_t *tmp = alloc_pt();
		if (!tmp)
			return -ENOMEM;
		*pd = mk_pde(tmp, _PDE);
		error = zeromap_pde_range(pd, addr, end - addr, zero_pte);
		if (error)
			break;
		addr = (addr + PGDIR_SIZE) & PGDIR_MASK;
		pd++;
	}
}

static int anon_map(struct inode *ino, struct file *file, struct vm_area_struct *vma)
{
	if (zeromap_page_range(vma->vm_start, vma->vm_end - vma->vm_start, vma->vm_page_prot))
		return -ENOMEM;
	return 0;
}
#endif
/*
 * In i386, VM_WRITE implies VM_READ, and VM_READ and VM_EXEC implies each other
 */
static unsigned long get_page_prot(unsigned long flags)
{
	if (flags & VM_WRITE)
		return PG_PRESENT | PG_USER | PG_RW;
	if (flags & VM_READ || flags & VM_EXEC)
		return PG_PRESENT | PG_USER;
	return PG_PRESENT;
}

unsigned long do_mmap(unsigned long addr, unsigned long len, unsigned long prot, 
	unsigned long flags, unsigned long fd, unsigned long off)
{
	int error;
	struct file *file = NULL;
	struct vm_area_struct *vma;

	if (flags & MAP_ANONYMOUS) {
		if (fd != (unsigned long)-1)
			return -EINVAL;
	} else {
		if (fd >= NR_OPEN || !(file = current->files->fd[fd]))
			return -EBADF;
	}

	if ((len = PAGE_ALIGN(len)) == 0)
		return -EINVAL;
	if (addr > KERNEL_BASE || len > KERNEL_BASE || addr > KERNEL_BASE-len)
		return -EINVAL;

	if (flags & MAP_FIXED) {
		if (addr & ~PAGE_MASK)
			return -EINVAL;
		if (len > KERNEL_BASE || addr > KERNEL_BASE - len)
			return -EINVAL;
	} else {
		addr = get_unmapped_area(addr, len);
		if (!addr)
			return -ENOMEM;
	}

	if (file) {
		if (!file->f_op || !file->f_op->mmap)
			return -ENODEV;
		if (off & ~PAGE_MASK)
			return -EINVAL;
		/* offset overflow? */
		if (off + len < off)
			return -EINVAL;
	}


	switch (flags & MAP_TYPE) {
	case MAP_SHARED:
		if (file && (prot & PROT_WRITE) && !(file->f_mode & 2))
			return -EACCES;
	case MAP_PRIVATE:
		if (file && !(file->f_mode & 1))
			return -EACCES;
		break;
	default:
		return -EINVAL;
	}
	
	vma = (struct vm_area_struct *)kmalloc(sizeof(*vma));
	if (!vma)
		return -ENOMEM;
	vma->vm_mm= current->mm;
	vma->vm_start = addr;
	vma->vm_end = addr + len;
	vma->vm_flags = prot & (VM_READ | VM_WRITE | VM_EXEC);
	if (flags & VM_GROWSDOWN)
		vma->vm_flags |= VM_GROWSDOWN;
	if ((flags & MAP_TYPE) == MAP_SHARED)
		vma->vm_flags |= VM_SHARED;

	/* initialize the share ring */
	vma->vm_next_share = vma->vm_prev_share = vma;
	vma->vm_page_prot = get_page_prot(vma->vm_flags);
	vma->vm_ops = NULL;
	if (file)
		vma->vm_offset = off;
	else
		vma->vm_offset = 0;
	vma->vm_inode = NULL;
	do_munmap(addr, len);	/* Clear old maps */
	if (file) {
		error = file->f_op->mmap(file->f_inode, file, vma);
		if (error) {
			kfree(vma);
			return error;
		}
	}
	insert_vm_struct(current->mm, vma);
	/* merge_segments(current->mm, vma->vm_start, vma->vm_end); */
	return addr;
}

int sys_mmap(struct mmap_arg_struct *arg)
{
	int error;
	struct mmap_arg_struct a;

	error = verify_area(VERIFY_READ, arg, sizeof(*arg));
	if (error)
		return error;
	memcpy_fromfs(&a, arg, sizeof(*arg));
	return do_mmap(a.addr, a.len, a.prot, a.flags, a.fd, a.offset);
}

void print_mmap(struct mm_struct *mm)
{
	struct vm_area_struct *vma;

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		early_print("%x, %x, ", vma->vm_start, vma->vm_end);
		char c1, c2;
		if (vma->vm_flags & VM_WRITE)
			c1 = 'w';
		else if (vma->vm_flags & (VM_READ | VM_EXEC))
			c1 = 'r';
		else
			c1 = 'o';
		if (vma->vm_flags & VM_SHARED)
			c2 = 'S';
		else
			c2 = 'P';
		early_print("%c, %c\n", c1, c2);
	}
}