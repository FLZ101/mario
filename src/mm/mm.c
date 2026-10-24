#include <task.h>
#include <misc.h>

#include <mm/mm.h>
#include <mm/mman.h>
#include <mm/kmalloc.h>

#include <fs/fs.h>

#include <lib/stddef.h>

void print_mmap(struct mm_struct *mm)
{
	struct vm_area_struct *vma;

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		pr_debug("%x, %x, ", vma->vm_start, vma->vm_end);
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
		pr_debug("%c, %c\n", c1, c2);
	}
}

void merge_mmap(struct mm_struct *mm)
{
	struct vm_area_struct *p1, *p2;
	unsigned long flags;

	mm_lock(mm, &flags);
	for (p1 = mm->mmap; p1 && (p2 = p1->vm_next);) {
		if (p1->vm_end == p2->vm_start && p1->vm_flags == p2->vm_flags &&
			!(p1->vm_flags & VM_SHARED) && !p1->vm_file && !p2->vm_file &&
			!p1->vm_inode && !p2->vm_inode) {
			p1->vm_end = p2->vm_end;
			p1->vm_next = p2->vm_next;
			kfree(p2);
			continue;
		}
		p1 = p1->vm_next;
	}
	mm_unlock(mm, flags);
}

size_t get_vm_size(struct mm_struct *mm)
{
	size_t n = 0;
	struct vm_area_struct *vma;

	for (vma = mm->mmap ; vma ; vma = vma->vm_next) {
		n += vma->vm_end - vma->vm_start;
	}
	return n;
}

void verify_vm(struct mm_struct *mm)
{
	struct vm_area_struct *vma;

	for (vma = mm->mmap ; vma ; vma = vma->vm_next) {
		assert(vma->vm_start < vma->vm_end);
		assert(PAGE_ALIGNED(vma->vm_start));
		assert(PAGE_ALIGNED(vma->vm_end));
		if (vma->vm_next)
			assert(vma->vm_end <= vma->vm_next->vm_start);
	}
}

/* Return the first VMA whose end is above @addr without taking mm_lock. */
static struct vm_area_struct *find_vma_nolock(struct mm_struct *mm,
					       unsigned long addr)
{
	struct vm_area_struct *vma;

	for (vma = mm->mmap ; vma ; vma = vma->vm_next) {
		if (vma->vm_end > addr)
			return vma;
	}
	return NULL;
}

// return first vma whose end > addr
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
	struct vm_area_struct *vma;
	unsigned long flags;

	mm_lock(mm, &flags);
	vma = find_vma_nolock(mm, addr);
	mm_unlock(mm, flags);
	return vma;
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

/*
 * Insert @vma into @mm's sorted VMA list without taking mm_lock.  Used both by
 * the public wrapper and by do_mmap() while it holds mm_lock across address
 * allocation and insertion.
 */
static void insert_vm_struct_locked(struct mm_struct *mm,
				     struct vm_area_struct *vma)
{
	struct vm_area_struct **p, *mpnt;

	p = &mm->mmap;
	while ((mpnt = *p) != NULL) {
		if (mpnt->vm_start > vma->vm_start) {
			assert(mpnt->vm_start >= vma->vm_end);
			break;
		}
		assert(mpnt->vm_end <= vma->vm_start);
		p = &mpnt->vm_next;
	}
	vma->vm_next = mpnt;
	*p = vma;

	verify_vm(mm);
}

void insert_vm_struct(struct mm_struct *mm, struct vm_area_struct *vma)
{
	unsigned long flags;

	mm_lock(mm, &flags);
	insert_vm_struct_locked(mm, vma);
	mm_unlock(mm, flags);
}

void remove_shared_vm_struct(struct vm_area_struct *vma)
{
	unsigned long flags;

	mm_lock(vma->vm_mm, &flags);
	vma->vm_next_share->vm_prev_share = vma->vm_prev_share;
	vma->vm_prev_share->vm_next_share = vma->vm_next_share;
	vma->vm_next_share = vma;
	vma->vm_prev_share = vma;
	mm_unlock(vma->vm_mm, flags);
}

void exit_mmap(struct task_struct *t)
{
	struct vm_area_struct *vma, *next;
	unsigned long flags;

	mm_lock(t->mm, &flags);
	vma = t->mm->mmap;
	t->mm->mmap = NULL;
	mm_unlock(t->mm, flags);
	while (vma) {
		next = vma->vm_next;
		if (vma->vm_ops && vma->vm_ops->close)
			vma->vm_ops->close(vma);
		remove_shared_vm_struct(vma);
		if (vma->vm_file)
			put_file(vma->vm_file);
		else if (vma->vm_inode)
			iput(vma->vm_inode);
		kfree(vma);
		vma = next;
	}
}

int dup_mmap(struct task_struct *t)
{
	struct vm_area_struct *vma, *tmp, **p;
	unsigned long flags;

	mm_lock(current->mm, &flags);
	p = &t->mm->mmap;
	*p = NULL;
	for (vma = current->mm->mmap; vma; vma = vma->vm_next) {
		tmp = (struct vm_area_struct *)kmalloc(sizeof(struct vm_area_struct));
		if (!tmp) {
			mm_unlock(current->mm, flags);
			exit_mmap(t);
			return -ENOMEM;
		}
		*tmp = *vma;
		tmp->vm_mm = t->mm;
		tmp->vm_next = NULL;
		if (tmp->vm_file)
			fref(tmp->vm_file);
		else if (tmp->vm_inode)
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
	mm_unlock(current->mm, flags);
	return 0;
}

void unmap_fixup(struct vm_area_struct *vma, unsigned long addr, unsigned long len)
{
	struct vm_area_struct *mpnt;
	unsigned long end = addr + len;

	/* Unmapping the whole area */
	if (addr == vma->vm_start && end == vma->vm_end) {
		if (vma->vm_ops && vma->vm_ops->close)
			vma->vm_ops->close(vma);
		if (vma->vm_file)
			put_file(vma->vm_file);
		else if (vma->vm_inode)
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
		assert(mpnt);

		*mpnt = *vma;
		/* The original vma is about to be freed.  Do not leave the split
		 * area's share ring pointing at it; keep the new vma self-linked
		 * exactly as a freshly created private mapping would be. */
		mpnt->vm_next_share = mpnt;
		mpnt->vm_prev_share = mpnt;
		mpnt->vm_offset += (end - vma->vm_start);
		mpnt->vm_start = end;

		if (mpnt->vm_file)
			fref(mpnt->vm_file);
		else if (mpnt->vm_inode)
			iref(mpnt->vm_inode);
		if (mpnt->vm_ops && mpnt->vm_ops->open)
			mpnt->vm_ops->open(mpnt);

		vma->vm_end = addr;	/* Truncate area */
		insert_vm_struct(current->mm, mpnt);
	}

	/* construct whatever mapping is needed */
	mpnt = (struct vm_area_struct *)kmalloc(sizeof(*mpnt));
	assert(mpnt);

	*mpnt = *vma;
	mpnt->vm_next_share = mpnt;
	mpnt->vm_prev_share = mpnt;
	if (mpnt->vm_file)
		fref(mpnt->vm_file);
	else if (mpnt->vm_inode)
		iref(mpnt->vm_inode);
	if (mpnt->vm_ops && mpnt->vm_ops->open)
		mpnt->vm_ops->open(mpnt);
	if (vma->vm_ops && vma->vm_ops->close) {
		vma->vm_end = vma->vm_start;
		vma->vm_ops->close(vma);
	}
	insert_vm_struct(current->mm, mpnt);

	/*
	 * The caller frees @vma after this function returns.  The inserted
	 * replacement VMA(s) took their own references above, so release the
	 * original VMA's file/inode ownership here.
	 */
	if (vma->vm_file)
		put_file(vma->vm_file);
	else if (vma->vm_inode)
		iput(vma->vm_inode);
	vma->vm_file = NULL;
	vma->vm_inode = NULL;
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
	} while (addr < end);
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

int do_munmap(unsigned long addr, unsigned long len)
{
	struct vm_area_struct *vma, *free, **p;
	unsigned long flags;

	if ((addr & ~PAGE_MASK) || addr > KERNEL_BASE || len > KERNEL_BASE - addr)
		return -EINVAL;
	if ((len = PAGE_ALIGN(len)) == 0)
		return 0;

	mm_lock(current->mm, &flags);
	free = NULL;
	for (vma = current->mm->mmap, p = &current->mm->mmap; vma; ) {
		assert(vma == *p);

		if (vma->vm_start >= addr + len)
			break;
		if (vma->vm_end <= addr) {
			p = &vma->vm_next;
			vma = vma->vm_next;
			continue;
		}
		// vma overlaps the range

		// remove vma from mm and add it to free list
		*p = vma->vm_next;
		vma->vm_next = free;
		free = vma;
		vma = *p;
	}
	mm_unlock(current->mm, flags);
	if (!free)
		return 0;

	while (free) {
		unsigned long start, end;

		// remove vma from free list
		vma = free;
		free = free->vm_next;

		remove_shared_vm_struct(vma);

		start = MAX(addr, vma->vm_start);
		end = MIN(addr + len, vma->vm_end);

		if (vma->vm_ops && vma->vm_ops->unmap)
			vma->vm_ops->unmap(vma, start, end - start);

		unmap_fixup(vma, start, end - start);
		kfree(vma);
	}

	unmap_page_range(addr, len);
	return 0;
}

int sys_munmap(unsigned long addr, unsigned long len)
{
	return do_munmap(addr, len);
}

/*
 * Flush shared writable mappings in [addr, addr+len).  The current kernel has
 * no background writeback, so MS_ASYNC and MS_SYNC are equivalent.
 */
int sys_msync(unsigned long addr, unsigned long len, int flags)
{
	struct vm_area_struct *vma;
	unsigned long start, stop, end, cursor;

	if (addr & ~PAGE_MASK)
		return -EINVAL;
	if (addr > KERNEL_BASE || len > KERNEL_BASE - addr)
		return -EINVAL;
	if ((flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) ||
	    ((flags & MS_SYNC) && (flags & MS_ASYNC)))
		return -EINVAL;
	if (!len)
		return 0;

	len = PAGE_ALIGN(len);
	end = addr + len;
	cursor = addr;

	for (vma = find_vma(current->mm, cursor);
	     vma && vma->vm_start < end;
	     vma = vma->vm_next) {
		/* A gap means the caller asked to sync an unmapped area. */
		if (vma->vm_start > cursor)
			return -ENOMEM;

		start = MAX(cursor, vma->vm_start);
		stop = MIN(end, vma->vm_end);
		if (vma->vm_ops && vma->vm_ops->sync &&
		    (vma->vm_flags & VM_SHARED) &&
		    (vma->vm_flags & VM_WRITE))
			vma->vm_ops->sync(vma, start, stop - start, flags);
		cursor = MAX(cursor, vma->vm_end);
	}

	if (cursor < end)
		return -ENOMEM;
	return 0;
}

static unsigned long get_page_prot(unsigned long flags);

/*
 * Update the protection bits on already-present pages in [addr, addr+len).
 * Pages not yet faulted in pick up the new VMA protection from do_no_page().
 * This keeps PROT_NONE -> PROT_READ|PROT_WRITE transitions, as used by musl
 * pthread_create's guard-page setup, effective immediately.
 */
static void protect_page_range(unsigned long addr, unsigned long len,
			       unsigned long prot)
{
	unsigned long end = addr + len;
	unsigned long attr = get_page_prot(prot & (VM_READ | VM_WRITE | VM_EXEC));
	unsigned long rw_user = attr & (PG_RW | PG_USER);

	while (addr < end) {
		pde_t *pd = pde_offset(current->mm->pd, addr);
		unsigned long pde_end = (addr + PGDIR_SIZE) & PGDIR_MASK;

		if (pde_end > end)
			pde_end = end;
		if (__pe_present(pd)) {
			pte_t *pt = pte_offset(__vir(*pd), addr);
			unsigned long a;

			for (a = addr; a < pde_end; a += PAGE_SIZE, pt++) {
				if (__pe_present(pt) && !__pte_bad(pt))
					*pt = (*pt & ~(PG_RW | PG_USER)) | rw_user;
			}
		}
		addr = pde_end;
	}
	flush_tlb();
}

/*
 * Return a free region at or above @addr.  Callers hold current->mm->mm_lock
 * for the whole allocate-and-insert sequence so two threads cannot choose the
 * same anonymous stack range and then collide in insert_vm_struct().
 */
static unsigned long get_unmapped_area_locked(unsigned long addr,
					       unsigned long len)
{
	struct vm_area_struct *vma;
	unsigned long result = 0;

	if (len > KERNEL_BASE)
		return 0;
	if (!addr)
		addr = KERNEL_BASE / 2;	/* Is it OK? */
	addr = PAGE_ALIGN(addr);

	for (vma = current->mm->mmap; ; vma = vma->vm_next) {
		// addr is not in any previous vm area

		if (KERNEL_BASE - len < addr)
			break;
		if (!vma) {
			result = addr;
			break;
		}
		if (addr > vma->vm_end)
			continue;
		if (addr + len > vma->vm_start) {
			addr = vma->vm_end;
			continue;
		}

		// can be fit in the gap
		result = addr;
		break;
	}
	return result;
}

/*
 * In i386, VM_WRITE implies VM_READ, and VM_READ and VM_EXEC implies each other
 */
static unsigned long get_page_prot(unsigned long flags)
{
	if (flags & VM_WRITE)
		return PG_PRESENT | PG_USER | PG_RW;
	if ((flags & VM_READ) || (flags & VM_EXEC))
		return PG_PRESENT | PG_USER;
	return PG_PRESENT;
}

static struct vm_area_struct *alloc_vma(unsigned long addr, unsigned long len,
					unsigned long prot, unsigned long flags,
					unsigned long off, struct file *file)
{
	struct vm_area_struct *vma;

	vma = (struct vm_area_struct *)kmalloc(sizeof(*vma));
	if (!vma)
		return NULL;

	vma->vm_mm = current->mm;
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
	vma->vm_offset = file ? off : 0;
	vma->vm_file = NULL;
	vma->vm_inode = NULL;
	return vma;
}

unsigned long do_mmap(unsigned long addr, unsigned long len, unsigned long prot,
	unsigned long flags, unsigned int fd, unsigned long off)
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
	if (addr > KERNEL_BASE || len > KERNEL_BASE || addr > KERNEL_BASE - len)
		return -EINVAL;

	if (flags & MAP_FIXED) {
		if (addr & ~PAGE_MASK)
			return -EINVAL;
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
			/* no break */
	case MAP_PRIVATE:
		if (file && !(file->f_mode & 1))
			return -EACCES;
		break;
	default:
		return -EINVAL;
	}

	if (!(flags & MAP_FIXED)) {
		unsigned long mm_flags;

		/*
		 * Allocate and insert the VMA atomically with respect to other
		 * CLONE_VM threads.  Otherwise two pthread_create calls can both
		 * choose the same free gap, and the second insert_vm_struct() sees
		 * an overlapping VMA and trips the sort-order assert.
		 */
		mm_lock(current->mm, &mm_flags);
		addr = get_unmapped_area_locked(addr, len);
		if (!addr) {
			mm_unlock(current->mm, mm_flags);
			return -ENOMEM;
		}

		vma = alloc_vma(addr, len, prot, flags, off, file);
		if (!vma) {
			mm_unlock(current->mm, mm_flags);
			return -ENOMEM;
		}

		if (file) {
			error = file->f_op->mmap(file->f_inode, file, vma);
			if (error) {
				if (vma->vm_file)
					put_file(vma->vm_file);
				kfree(vma);
				mm_unlock(current->mm, mm_flags);
				return error;
			}
		}

		insert_vm_struct_locked(current->mm, vma);
		mm_unlock(current->mm, mm_flags);
		merge_mmap(current->mm);
		return addr;
	}

	vma = alloc_vma(addr, len, prot, flags, off, file);
	if (!vma)
		return -ENOMEM;

	do_munmap(addr, len);	/* Clear old maps */

	if (file) {
		error = file->f_op->mmap(file->f_inode, file, vma);
		if (error) {
			if (vma->vm_file)
				put_file(vma->vm_file);
			kfree(vma);
			return error;
		}
	}

	insert_vm_struct(current->mm, vma);
	merge_mmap(current->mm);
	return addr;
}

int sys_mmap(unsigned long addr, unsigned long len, unsigned long prot,
	unsigned long flags, int fd, unsigned long off)
{
	return do_mmap(addr, len, prot, flags, fd, off);
}

int sys_mmap2(unsigned long addr, unsigned long len, unsigned long prot,
	unsigned long flags, int fd, unsigned long off)
{
	return do_mmap(addr, len, prot, flags, fd, off * 4096);
}

unsigned long sys_brk(unsigned long brk)
{
	unsigned long rlim;
	unsigned long newbrk, oldbrk;

	if (!brk)
		return current->mm->brk;

	assert(brk >= current->mm->start_brk);

	newbrk = PAGE_ALIGN(brk);
	oldbrk = PAGE_ALIGN(current->mm->brk);
	if (oldbrk == newbrk)
		return current->mm->brk = brk;

	/*
	 * Always allow shrinking brk
	 */
	if (brk < current->mm->brk) {
		int err = do_munmap(newbrk, oldbrk - newbrk);
		if (err)
			return err;
		current->mm->brk = brk;
		return brk;
	}

	/*
	 * Check against rlimit
	 */
	 rlim = current->rlim[RLIMIT_DATA].rlim_cur;
	 if (brk - current->mm->end_code > rlim)
	 	return -ENOMEM;

	/*
	 * Check against existing mmap mappings
	 */
	if (find_vma_intersection(current->mm, oldbrk, newbrk + PAGE_SIZE))
		return -ENOMEM;

	int err = do_mmap(oldbrk, newbrk - oldbrk, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (err < 0)
		return err;
	current->mm->brk = brk;
	return brk;
}

int sys_mprotect(void *addr, size_t len, int prot)
{
	unsigned long start = (unsigned long)addr;
	unsigned long end;
	struct vm_area_struct *vma;
	unsigned long flags;
	unsigned long vma_prot = prot & (VM_READ | VM_WRITE | VM_EXEC);

	if (start > KERNEL_BASE || len > KERNEL_BASE - start)
		return -EINVAL;
	start &= PAGE_MASK;
	len = PAGE_ALIGN(len);
	if (!len)
		return 0;
	end = start + len;

	/*
	 * Update whole overlapping VMAs rather than splitting them.  That is
	 * sufficient for musl's anonymous thread-stack mapping; the guard page
	 * therefore also becomes writable in this minimal implementation.
	 */
	mm_lock(current->mm, &flags);
	for (vma = current->mm->mmap; vma && vma->vm_start < end;
	     vma = vma->vm_next) {
		if (vma->vm_end <= start)
			continue;
		vma->vm_flags =
			(vma->vm_flags & ~(VM_READ | VM_WRITE | VM_EXEC)) |
			vma_prot;
		vma->vm_page_prot = get_page_prot(vma->vm_flags);
	}
	mm_unlock(current->mm, flags);

	protect_page_range(start, len, vma_prot);
	return 0;
}
