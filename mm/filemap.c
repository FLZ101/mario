#include <mm/mm.h>
#include <mm/mman.h>

#include <fs/fs.h>

static int filemap_sync_pte_range(pte_t *pt, struct vm_area_struct *vma,
	unsigned long addr, unsigned long len, struct file *f)
{
	unsigned long start;

	if (!__pe_present(pt) || __pte_bad(pt))
		return 0;
	addr &= ~PAGE_MASK;
	if (addr + len >= PAGE_SIZE);
		len = PAGE_SIZE - addr;
	if (f->f_pos >= f->f_inode->i_size)
		return 1;
	if (f->f_pos + len >= f->f_inode->i_size)
		len = f->f_inode->i_size - f->f_pos;

	start = __vir((unsigned long)(*pt) & PAGE_MASK) + addr;
	/* write fails? */
	if (0 == f->f_op->write(f->f_inode, f, (char *)start, len))
		return 1;
	return 0;
}

static int filemap_sync_pde_range(pde_t *pd, struct vm_area_struct *vma,
	unsigned long addr, unsigned long len, struct file *f)
{
	pte_t *pt;
	unsigned long end;

	if (!__pe_present(pd))
		return 0;
	addr &= ~PGDIR_MASK;
	end = addr + len;
	if (end >= PGDIR_SIZE)
		end = PGDIR_SIZE;

	pt = pte_offset(__vir(*pd), addr);
	do {
		if (filemap_sync_pte_range(pt, vma, addr, end - addr, f))
			return 1;
		addr += PAGE_SIZE;
		pt++;
	} while (addr < len);
	return 0;
}

/*
 * Write bytes of a memory-mapped file back
 * Currently only MS_SYNC supported
 * Hope this works :)
 */
static void filemap_sync(struct vm_area_struct *vma,
	unsigned long addr, unsigned long len, int flags)
{
	pde_t *pd;
	struct file f;
	struct inode *i;
	unsigned long end = addr + len;

	i = vma->vm_inode;
	f.f_inode = i;
	f.f_mode = 2;	/* write-only */
	f.f_op = i->i_fop;
	f.f_pos = addr - vma->vm_start + vma->vm_offset;

	pd = pde_offset(vma->vm_mm->pd, addr);
	while (addr < end) {
		if (filemap_sync_pde_range(pd, vma, addr, end - addr, &f))
			break;
		addr = (addr + PGDIR_SIZE) & PGDIR_MASK;
		pd++;
	}
}

static void filemap_close(struct vm_area_struct *vma)
{
	filemap_sync(vma, vma->vm_start, vma->vm_end - vma->vm_start, MS_SYNC);
}

static void filemap_unmap(struct vm_area_struct *vma,
	unsigned long start, unsigned long len)
{
	filemap_sync(vma, start, len, MS_SYNC);
}

/*
 * Load bytes of a memory-mapped file into memory
 * Hope this works :)
 */
static int filemap_nopage(struct vm_area_struct *vma,
	unsigned long addr, unsigned long page)
{
	struct inode *i;
	struct file f;

	addr &= PAGE_MASK;
	i = vma->vm_inode;
	f.f_inode = i;
	f.f_mode = 1;	/* read-only */
	f.f_op = i->i_fop;	/* NULL? */
	f.f_pos = vma->vm_offset + addr - vma->vm_start;
	if (f.f_op->read(i, &f, (char *)page, 4096) < 0)
		return -1;
	return 0;

}

static struct vm_operations_struct file_shared_mmap = {
	NULL,			/* open */
	filemap_close,		/* close */
	filemap_sync,		/* sync */
	filemap_unmap,		/* unmap */
	filemap_nopage,		/* nopage */
	NULL,			/* wppage */
};

static struct vm_operations_struct file_private_mmap = {
	NULL,			/* open */
	NULL,			/* close */
	NULL,			/* unmap */
	NULL,			/* sync */
	filemap_nopage,		/* nopage */
	NULL,			/* wppage */
};

/* This is used for a general mmap of a disk file */
int generic_file_mmap(struct inode *inode,
	struct file *file, struct vm_area_struct *vma)
{
	struct vm_operations_struct *ops;

	if (vma->vm_flags & VM_SHARED)
		ops = &file_shared_mmap;
	else
		ops = &file_private_mmap;

	if (!inode->i_sb || !S_ISREG(inode->i_mode))
		return -EACCES;

	iref(inode);
	vma->vm_inode = inode;
	vma->vm_ops = ops;
	return 0;
}