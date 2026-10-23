#include <errno.h>
#include <lib/string.h>

#include <mm/kmalloc.h>
#include <mm/mm.h>
#include <mm/mman.h>
#include <mm/uaccess.h>
#include <lib/spinlock.h>

#include <fs/fs.h>

/*
 * File-backed mmap pages are cached in this global list.  The cache is kept
 * deliberately small: one entry per cached page, keyed by inode and file page
 * index.  This gives independent MAP_SHARED mappings of the same file a way to
 * share the same physical page, which the fork-only VMA share ring cannot do.
 */
struct filemap_page {
	struct list_head list;
	struct inode *inode;
	unsigned long index;
	unsigned long page;
};

static LIST_HEAD(filemap_pages);

/*
 * Protects the global file-backed page cache.  Lookup, insertion, and
 * invalidation all mutate the same list, so a plain irq_save would still let
 * a page fault on one CPU race truncate on another once APs are online.
 */
static spinlock_t filemap_lock = SPINLOCK_UNLOCKED;

static struct filemap_page *filemap_lookup(struct inode *inode,
					   unsigned long index)
{
	struct list_head *pos;
	struct filemap_page *entry;

	list_for_each(pos, &filemap_pages) {
		entry = list_entry(pos, struct filemap_page, list);
		if (entry->inode == inode && entry->index == index)
			return entry;
	}
	return NULL;
}

static void filemap_free_entry(struct filemap_page *entry)
{
	list_del(&entry->list);
	put_page(entry->page);
	kfree(entry);
}

/*
 * Truncate the page cache for @inode to @from bytes.  Pages wholly beyond the
 * new EOF are dropped.  The page containing the new EOF is kept, but its tail
 * is zeroed so a later page fault or writeback cannot expose stale bytes.
 */
void filemap_invalidate_pages(struct inode *inode, unsigned long from)
{
	struct list_head *pos, *next;
	struct filemap_page *entry;
	unsigned long page_start;
	unsigned long page_end;
	unsigned long flags;

	spin_lock_irqsave(&filemap_lock, flags);
	for (pos = filemap_pages.next; pos != &filemap_pages; pos = next) {
		next = pos->next;
		entry = list_entry(pos, struct filemap_page, list);
		if (entry->inode != inode)
			continue;

		page_start = entry->index * PAGE_SIZE;
		page_end = page_start + PAGE_SIZE;
		if (page_start >= from) {
			filemap_free_entry(entry);
		} else if (page_end > from) {
			memset((void *)(entry->page + from - page_start), 0,
			       PAGE_SIZE - (from - page_start));
		}
	}
	spin_unlock_irqrestore(&filemap_lock, flags);
}

/* Release every cached page owned by @inode. */
void filemap_invalidate_inode(struct inode *inode)
{
	struct list_head *pos, *next;
	struct filemap_page *entry;
	unsigned long flags;

	spin_lock_irqsave(&filemap_lock, flags);
	for (pos = filemap_pages.next; pos != &filemap_pages; pos = next) {
		next = pos->next;
		entry = list_entry(pos, struct filemap_page, list);
		if (entry->inode == inode)
			filemap_free_entry(entry);
	}
	spin_unlock_irqrestore(&filemap_lock, flags);
}

/*
 * Read file bytes into a kernel page without changing the caller's file
 * position.  Filesystem read methods copy through %fs, so temporarily switch
 * %fs to KERNEL_DS while handing them a kernel buffer.
 */
static int filemap_read_page(struct file *file, unsigned long pos,
			     unsigned long page, unsigned long bytes)
{
	struct file f;
	unsigned long old_fs;
	int ret;

	if (!file || !file->f_op || !file->f_op->read)
		return -EIO;

	f = *file;
	f.f_pos = pos;
	f.f_mode = 1;
	f.f_flags = 0;

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	ret = f.f_op->read(f.f_inode, &f, (char *)page, bytes);
	set_fs(old_fs);
	return ret;
}

/*
 * Write a kernel page back to a file at an explicit offset.  As with the read
 * helper, %fs must be KERNEL_DS because filesystem write methods copy from
 * user space with memcpy_fromfs().
 */
static int filemap_write_page(struct file *file, unsigned long pos,
			      unsigned long page, unsigned long bytes)
{
	struct file f;
	unsigned long old_fs;
	int ret;

	if (!file || !file->f_op || !file->f_op->write)
		return -EIO;

	f = *file;
	f.f_pos = pos;
	f.f_mode = 2;
	f.f_flags = 0;

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	ret = f.f_op->write(f.f_inode, &f, (char *)page, bytes);
	set_fs(old_fs);
	return ret;
}

/*
 * Resolve a file-mapping page fault.  The returned page has one reference for
 * the caller's page table entry.  Pages are inserted into the shared cache so
 * subsequent mappings of the same file offset reuse them.
 */
static unsigned long filemap_nopage(struct vm_area_struct *vma,
				    unsigned long addr)
{
	struct file *file = vma->vm_file;
	struct inode *inode = file ? file->f_inode : vma->vm_inode;
	struct filemap_page *entry;
	unsigned long page;
	unsigned long pos;
	unsigned long index;
	unsigned long bytes;
	unsigned long file_size;
	int ret;
	unsigned long flags;

	addr &= PAGE_MASK;
	if (!file || !inode)
		return 0;

	pos = vma->vm_offset + addr - vma->vm_start;
	index = pos / PAGE_SIZE;

	spin_lock_irqsave(&filemap_lock, flags);
	entry = filemap_lookup(inode, index);
	if (entry) {
		ref_page(VIR_TO_PAGE(entry->page));
		spin_unlock_irqrestore(&filemap_lock, flags);
		return entry->page;
	}
	spin_unlock_irqrestore(&filemap_lock, flags);

	page = get_zero_page();
	if (!page)
		return 0;

	file_size = (unsigned long)inode->i_size;
	if (pos >= file_size) {
		/* Fully past EOF: the zero page is already correct. */
		bytes = 0;
	} else {
		bytes = PAGE_SIZE;
		if (file_size - pos < PAGE_SIZE)
			bytes = file_size - pos;
		ret = filemap_read_page(file, pos, page, bytes);
		if (ret != (int)bytes) {
			put_page(page);
			return 0;
		}
	}

	/*
	 * Cache the page even if it is entirely past EOF.  A later shared
	 * write to that zero page must extend the file through the normal
	 * writeback path.
	 */
	entry = kmalloc(sizeof(*entry));
	if (!entry)
		return page;
	spin_lock_irqsave(&filemap_lock, flags);
	/*
	 * Another mapping may have inserted this exact (inode, index) while the
	 * file read above was running.  Reuse that cached page instead of
	 * creating a duplicate entry.
	 */
	if (filemap_lookup(inode, index)) {
		struct filemap_page *existing = filemap_lookup(inode, index);
		ref_page(VIR_TO_PAGE(existing->page));
		spin_unlock_irqrestore(&filemap_lock, flags);
		put_page(page);
		kfree(entry);
		return existing->page;
	}
	entry->inode = inode;
	entry->index = index;
	entry->page = page;
	list_add(&entry->list, &filemap_pages);

	/* One reference for the cache, one for the installing PTE. */
	ref_page(VIR_TO_PAGE(page));
	spin_unlock_irqrestore(&filemap_lock, flags);
	return page;
}

/*
 * Write one dirty present PTE back to its file position.  Only the range
 * actually selected by the caller is written; partial pages therefore write
 * only the requested tail.
 */
static int filemap_sync_pte(struct vm_area_struct *vma, unsigned long addr,
			    unsigned long len, struct file *file)
{
	pde_t *pd;
	pte_t *pt;
	unsigned long offset;
	unsigned long page;
	unsigned long pos;
	int ret;
	unsigned long flags;

	pd = pde_offset(vma->vm_mm->pd, addr);
	if (!__pe_present(pd))
		return 0;

	pt = pte_offset(__vir(*pd), addr);
	if (!__pe_present(pt) || __pte_bad(pt) || !__pe_dirty(pt))
		return 0;

	offset = addr & ~PAGE_MASK;
	page = __vir((unsigned long)(*pt) & PAGE_MASK) + offset;
	pos = vma->vm_offset + addr - vma->vm_start;

	/*
	 * If truncate invalidated this page, do not write it back.  The PTE may
	 * still be present and dirty until unmap_page_range() drops it, but the
	 * page no longer belongs to the file's current size.
	 */
	spin_lock_irqsave(&filemap_lock, flags);
	if (!filemap_lookup(file->f_inode, pos / PAGE_SIZE)) {
		spin_unlock_irqrestore(&filemap_lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&filemap_lock, flags);

	ret = filemap_write_page(file, pos, page, len);
	if (ret != (int)len)
		return -EIO;

	clear_pe_dirty(pt);
	return 0;
}

/*
 * Flush a range of a shared writable file mapping.  This iterates the actual
 * page tables and uses the hardware dirty bit, so clean pages are not written
 * back unless another mapping has dirtied the same page.
 */
static void filemap_sync(struct vm_area_struct *vma,
			 unsigned long addr, unsigned long len, int flags)
{
	struct file *file = vma->vm_file;
	unsigned long end = addr + len;
	unsigned long chunk;

	/* Private mappings are intentionally not written back here. */
	if (!file || !(vma->vm_flags & VM_SHARED) || !(vma->vm_flags & VM_WRITE))
		return;

	while (addr < end) {
		chunk = PAGE_SIZE - (addr & ~PAGE_MASK);
		if (chunk > end - addr)
			chunk = end - addr;
		filemap_sync_pte(vma, addr, chunk, file);
		addr += chunk;
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

static struct vm_operations_struct file_shared_mmap = {
	.close = filemap_close,
	.sync = filemap_sync,
	.unmap = filemap_unmap,
	.nopage = filemap_nopage,
};

static struct vm_operations_struct file_private_mmap = {
	.nopage = filemap_nopage,
};

/*
 * Attach a regular file to a VMA.  The mapping owns a reference to the file so
 * the user can close the original descriptor without invalidating the mapping.
 */
int generic_file_mmap(struct inode *inode,
		      struct file *file, struct vm_area_struct *vma)
{
	struct vm_operations_struct *ops;

	if (!inode || !file || !inode->i_sb || !S_ISREG(inode->i_mode))
		return -EACCES;
	if (!file->f_op || !file->f_op->read)
		return -EACCES;

	if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_WRITE)) {
		if (!file->f_op->write)
			return -EACCES;
		if (inode->i_sb->s_flags & MS_RDONLY)
			return -EROFS;
	}

	ops = (vma->vm_flags & VM_SHARED) ? &file_shared_mmap
					  : &file_private_mmap;

	/* The file reference keeps the inode alive; vm_inode is non-owning. */
	fref(file);
	vma->vm_file = file;
	vma->vm_inode = inode;
	vma->vm_ops = ops;
	return 0;
}
