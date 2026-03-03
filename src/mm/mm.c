#include <task.h>
#include <misc.h>

#include <mm/mm.h>
#include <mm/mman.h>
#include <mm/kmalloc.h>

#include <fs/fs.h>

#include <lib/stddef.h>

void print_mmap(struct mm_struct *mm, char *tag)
{
	struct vm_area_struct *vma;

	if (tag)
		printk("%s\n", tag);
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma->vm_next && vma->vm_next->vm_next)
			continue;
		printk("%x, %x, ", vma->vm_start, vma->vm_end);
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
		printk("%c, %c\n", c1, c2);
	}
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

// return first vma whose end > addr
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
	struct vm_area_struct *vma;

	for (vma = mm->mmap ; vma ; vma = vma->vm_next) {
		if (vma->vm_end > addr)
			return vma;
	}
	return NULL;
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

void unmap_fixup(struct vm_area_struct *vma, unsigned long addr, unsigned long len)
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
		assert(mpnt);

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
	assert(mpnt);

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

	if ((addr & ~PAGE_MASK) || addr > KERNEL_BASE || len > KERNEL_BASE - addr)
		return -EINVAL;
	if ((len = PAGE_ALIGN(len)) == 0)
		return 0;

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

static unsigned long get_unmapped_area(unsigned long addr, unsigned long len)
{
	struct vm_area_struct *vma;

	if (len > KERNEL_BASE)
		return 0;
	if (!addr)
		addr = KERNEL_BASE / 2;	/* Is it OK? */
	addr = PAGE_ALIGN(addr);

	for (vma = current->mm->mmap; ; vma = vma->vm_next) {
		// addr is not in any previous vm area

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

		// can be fit in the gap
		return addr;
	}
	return 0;
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

unsigned long do_mmap(unsigned long addr, unsigned long len, unsigned long prot,
	unsigned long flags, int fd, unsigned long off)
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
			/* no break */
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

unsigned long sys_brk(unsigned long brk)
{
	unsigned long rlim;
	unsigned long newbrk, oldbrk;

	if (brk < current->mm->end_code)
		return current->mm->brk;
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
