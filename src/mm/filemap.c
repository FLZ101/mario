#include <errno.h>
#include <lib/string.h>

#include <mm/kmalloc.h>
#include <mm/mm.h>
#include <mm/mman.h>
#include <mm/uaccess.h>
#include <lib/spinlock.h>

#include <fs/fs.h>

#define FILEMAP_MAX_PAGES 8192
#define FILEMAP_HASH_BITS 10
#define FILEMAP_HASH_SIZE (1UL << FILEMAP_HASH_BITS)

enum filemap_page_state {
	FILEMAP_LOADING,
	FILEMAP_UPTODATE,
	FILEMAP_INVALIDATED,
	FILEMAP_ERROR,
};

enum filemap_fill_source {
	FILEMAP_FILL_READ,
	FILEMAP_FILL_MMAP,
	FILEMAP_FILL_MMAP_ZERO,
};

struct filemap_page {
	struct list_head list;
	struct list_head hash;
	struct inode *inode;
	unsigned long index;
	unsigned long page;
	int state;
	int error;
	int fill_source;
	wait_queue_t wait;
};

static LIST_HEAD(filemap_pages);
static struct list_head filemap_hash[FILEMAP_HASH_SIZE];
static spinlock_t filemap_lock = SPINLOCK_UNLOCKED;
static int filemap_initialized;
static unsigned long filemap_page_count;
static struct filemap_stats filemap_stats;

static unsigned long filemap_hash_index(struct inode *inode,
					unsigned long index)
{
	return (((unsigned long)inode >> 4) ^ index) &
		(FILEMAP_HASH_SIZE - 1);
}

static void filemap_init_locked(void)
{
	unsigned long index;

	if (filemap_initialized)
		return;
	INIT_LIST_HEAD(&filemap_pages);
	for (index = 0; index < FILEMAP_HASH_SIZE; index++)
		INIT_LIST_HEAD(&filemap_hash[index]);
	filemap_initialized = 1;
}

static struct filemap_page *filemap_lookup(struct inode *inode,
					   unsigned long index)
{
	struct list_head *pos;
	struct filemap_page *entry;
	struct list_head *head;

	filemap_init_locked();
	head = &filemap_hash[filemap_hash_index(inode, index)];
	list_for_each(pos, head) {
		entry = list_entry(pos, struct filemap_page, hash);
		if (entry->inode == inode && entry->index == index)
			return entry;
	}
	return NULL;
}

static void filemap_remove_locked(struct filemap_page *entry)
{
	list_del(&entry->list);
	list_del(&entry->hash);
	filemap_page_count--;
	if (entry->page)
		put_page(entry->page);
}

static void filemap_touch_locked(struct filemap_page *entry)
{
	list_del(&entry->list);
	list_add(&entry->list, &filemap_pages);
}

static int filemap_evict_one_locked(void)
{
	struct list_head *pos;
	struct filemap_page *entry;

	for (pos = filemap_pages.prev; pos != &filemap_pages;
	     pos = pos->prev) {
		entry = list_entry(pos, struct filemap_page, list);
		if (entry->state != FILEMAP_UPTODATE || !entry->page)
			continue;
		if (atomic_read(&VIR_TO_PAGE(entry->page)->count) != 1)
			continue;
		filemap_remove_locked(entry);
		kfree(entry);
		filemap_stats.evictions++;
		return 1;
	}
	return 0;
}

static void filemap_note_uncached(unsigned long bytes)
{
	unsigned long flags;

	spin_lock_irqsave(&filemap_lock, flags);
	filemap_stats.uncached_bytes += bytes;
	filemap_stats.temporary_fallbacks++;
	spin_unlock_irqrestore(&filemap_lock, flags);
}

void filemap_get_stats(struct filemap_stats *stats)
{
	unsigned long flags;

	if (!stats)
		return;
	spin_lock_irqsave(&filemap_lock, flags);
	*stats = filemap_stats;
	stats->cached_pages = filemap_page_count;
	spin_unlock_irqrestore(&filemap_lock, flags);
}

void filemap_reset_stats(void)
{
	unsigned long flags;

	spin_lock_irqsave(&filemap_lock, flags);
	memset(&filemap_stats, 0, sizeof(filemap_stats));
	spin_unlock_irqrestore(&filemap_lock, flags);
}

/*
 * Invalidate cached bytes at and beyond @from.  A loader remains published
 * until it wakes its waiters, but an invalidated loader may never publish its
 * page.
 */
void filemap_invalidate_pages(struct inode *inode, unsigned long from)
{
	struct list_head *pos, *next;
	struct filemap_page *entry;
	unsigned long page_start;
	unsigned long page_end;
	unsigned long flags;

	spin_lock_irqsave(&filemap_lock, flags);
	filemap_init_locked();
	for (pos = filemap_pages.next; pos != &filemap_pages; pos = next) {
		next = pos->next;
		entry = list_entry(pos, struct filemap_page, list);
		if (entry->inode != inode)
			continue;

		page_start = entry->index * PAGE_SIZE;
		page_end = page_start + PAGE_SIZE;
		if (page_start >= from) {
			filemap_stats.invalidations++;
			if (entry->state == FILEMAP_LOADING) {
				entry->state = FILEMAP_INVALIDATED;
				continue;
			}
			filemap_remove_locked(entry);
			kfree(entry);
		} else if (page_end > from && entry->state == FILEMAP_UPTODATE) {
			memset((void *)(entry->page + from - page_start), 0,
			       PAGE_SIZE - (from - page_start));
		}
	}
	spin_unlock_irqrestore(&filemap_lock, flags);
}

void filemap_invalidate_inode(struct inode *inode)
{
	struct list_head *pos, *next;
	struct filemap_page *entry;
	unsigned long flags;

	spin_lock_irqsave(&filemap_lock, flags);
	filemap_init_locked();
	for (pos = filemap_pages.next; pos != &filemap_pages; pos = next) {
		next = pos->next;
		entry = list_entry(pos, struct filemap_page, list);
		if (entry->inode != inode)
			continue;
		filemap_stats.invalidations++;
		if (entry->state == FILEMAP_LOADING) {
			entry->state = FILEMAP_INVALIDATED;
			continue;
		}
		filemap_remove_locked(entry);
		kfree(entry);
	}
	spin_unlock_irqrestore(&filemap_lock, flags);
}

static int filemap_fill_page(struct filemap_page *entry, struct file *file,
			     off64_t offset, enum filemap_access_kind access,
			     unsigned long *result)
{
	struct inode *inode = file->f_inode;
	unsigned long page;
	unsigned long valid = 0;
	unsigned long flags;
	int ret = 0;
	int source;

	page = get_zero_page();
	if (!page) {
		ret = -ENOMEM;
		goto finish;
	}

	down(&inode->i_sem);
	if ((uint64_t)offset < (uint64_t)inode->i_size) {
		valid = PAGE_SIZE;
		if ((uint64_t)inode->i_size - (uint64_t)offset < PAGE_SIZE)
			valid = (unsigned long)((uint64_t)inode->i_size -
						(uint64_t)offset);
		if (!file->f_op->read_page)
			ret = -EIO;
		else
			ret = file->f_op->read_page(inode, offset, (char *)page,
						    valid);
		if (ret == (int)valid)
			ret = 0;
		else if (ret >= 0)
			ret = -EIO;
	}
	up(&inode->i_sem);

finish:
	spin_lock_irqsave(&filemap_lock, flags);
	if (entry->state == FILEMAP_INVALIDATED) {
		filemap_remove_locked(entry);
		wake_up_all(&entry->wait);
		spin_unlock_irqrestore(&filemap_lock, flags);
		if (page)
			put_page(page);
		kfree(entry);
		return -EAGAIN;
	}
	if (ret) {
		entry->state = FILEMAP_ERROR;
		entry->error = ret;
		filemap_stats.loader_errors++;
		wake_up_all(&entry->wait);
		spin_unlock_irqrestore(&filemap_lock, flags);
		if (page)
			put_page(page);
		return ret;
	}

	source = access == FILEMAP_ACCESS_READ ? FILEMAP_FILL_READ :
		(valid ? FILEMAP_FILL_MMAP : FILEMAP_FILL_MMAP_ZERO);
	entry->page = page;
	entry->fill_source = source;
	entry->state = FILEMAP_UPTODATE;
	filemap_touch_locked(entry);
	ref_page(VIR_TO_PAGE(page));
	wake_up_all(&entry->wait);
	spin_unlock_irqrestore(&filemap_lock, flags);
	*result = page;
	return 0;
}

int filemap_get_page(struct file *file, off64_t offset,
		     enum filemap_access_kind access, unsigned long *page)
{
	struct inode *inode;
	struct filemap_page *entry;
	unsigned long index;
	unsigned long flags;
	int error;

	if (!file || !file->f_inode || !file->f_op || !page ||
	    (unsigned long)offset & ~PAGE_MASK)
		return -EINVAL;
	inode = file->f_inode;
	index = (unsigned long)offset / PAGE_SIZE;

	spin_lock_irqsave(&filemap_lock, flags);
	if (access == FILEMAP_ACCESS_READ)
		filemap_stats.read_acquires++;
	else
		filemap_stats.mmap_acquires++;
	spin_unlock_irqrestore(&filemap_lock, flags);

retry:
	spin_lock_irqsave(&filemap_lock, flags);
	entry = filemap_lookup(inode, index);
	if (entry) {
		if (entry->state == FILEMAP_UPTODATE) {
			ref_page(VIR_TO_PAGE(entry->page));
			filemap_touch_locked(entry);
			if (access == FILEMAP_ACCESS_READ) {
				filemap_stats.read_hits++;
				if (entry->fill_source != FILEMAP_FILL_READ)
					filemap_stats.read_hits_mmap_filled++;
			} else {
				filemap_stats.mmap_hits++;
				if (entry->fill_source == FILEMAP_FILL_READ)
					filemap_stats.mmap_hits_read_filled++;
				if (entry->fill_source == FILEMAP_FILL_MMAP_ZERO)
					filemap_stats.mmap_zero_hits++;
			}
			*page = entry->page;
			spin_unlock_irqrestore(&filemap_lock, flags);
			return 0;
		}
		if (entry->state == FILEMAP_ERROR) {
			error = entry->error;
			filemap_remove_locked(entry);
			spin_unlock_irqrestore(&filemap_lock, flags);
			kfree(entry);
			return error;
		}
		filemap_stats.waiters++;
		sleep_on(&entry->wait, TASK_UNINTERRUPTIBLE, &filemap_lock);
		goto retry;
	}
	spin_unlock_irqrestore(&filemap_lock, flags);

	entry = kmalloc(sizeof(*entry));
	if (!entry)
		return -ENOMEM;
	entry->inode = inode;
	entry->index = index;
	entry->page = 0;
	entry->state = FILEMAP_LOADING;
	entry->error = 0;
	entry->fill_source = FILEMAP_FILL_READ;
	INIT_LIST_HEAD(&entry->hash);
	init_wait_queue(&entry->wait);

	spin_lock_irqsave(&filemap_lock, flags);
	if (filemap_lookup(inode, index)) {
		spin_unlock_irqrestore(&filemap_lock, flags);
		kfree(entry);
		goto retry;
	}
	if (filemap_page_count >= FILEMAP_MAX_PAGES &&
	    !filemap_evict_one_locked()) {
		filemap_stats.budget_full++;
		spin_unlock_irqrestore(&filemap_lock, flags);
		kfree(entry);
		return -ENOSPC;
	}
	list_add(&entry->list, &filemap_pages);
	list_add(&entry->hash,
		 &filemap_hash[filemap_hash_index(inode, index)]);
	filemap_page_count++;
	filemap_stats.misses++;
	filemap_stats.loader_starts++;
	spin_unlock_irqrestore(&filemap_lock, flags);

	error = filemap_fill_page(entry, file, offset, access, page);
	if (error == -EAGAIN)
		goto retry;
	return error;
}

static int filemap_read_uncached(struct file *file, off64_t offset,
				 unsigned long *page)
{
	struct inode *inode = file->f_inode;
	unsigned long valid;
	int ret;

	*page = get_zero_page();
	if (!*page)
		return -ENOMEM;
	down(&inode->i_sem);
	if ((uint64_t)offset >= (uint64_t)inode->i_size) {
		up(&inode->i_sem);
		return 0;
	}
	valid = PAGE_SIZE;
	if ((uint64_t)inode->i_size - (uint64_t)offset < PAGE_SIZE)
		valid = (unsigned long)((uint64_t)inode->i_size - (uint64_t)offset);
	ret = file->f_op->read_page(inode, offset, (char *)*page, valid);
	up(&inode->i_sem);
	if (ret == (int)valid)
		return 0;
	put_page(*page);
	*page = 0;
	return ret < 0 ? ret : -EIO;
}

int generic_file_read(struct inode *inode, struct file *file, char *buf,
		      int count)
{
	unsigned long page;
	unsigned long offset;
	unsigned long chunk;
	unsigned long available;
	int done = 0;
	int error;

	if (!inode || !file || !S_ISREG(inode->i_mode))
		return -EINVAL;
	if (count <= 0)
		return 0;

	while (done < count) {
		down(&inode->i_sem);
		if ((uint64_t)file->f_pos >= (uint64_t)inode->i_size) {
			up(&inode->i_sem);
			break;
		}
		available = (unsigned long)((uint64_t)inode->i_size -
						  (uint64_t)file->f_pos);
		up(&inode->i_sem);
		offset = (unsigned long)file->f_pos & PAGE_MASK;
		chunk = PAGE_SIZE - ((unsigned long)file->f_pos & ~PAGE_MASK);
		if (chunk > available)
			chunk = available;
		if (chunk > (unsigned long)(count - done))
			chunk = count - done;

		error = filemap_get_page(file, offset, FILEMAP_ACCESS_READ, &page);
		if (error == -ENOSPC) {
			filemap_note_uncached(chunk);
			error = filemap_read_uncached(file, offset, &page);
		}
		if (error)
			return done ? done : error;
		memcpy_tofs(buf + done,
			    (char *)page + ((unsigned long)file->f_pos & ~PAGE_MASK),
			    chunk);
		put_page(page);
		file->f_pos += chunk;
		done += chunk;
	}
	return done;
}

void filemap_update_cached_range(struct inode *inode, off64_t offset,
				 const char *bytes, unsigned long count)
{
	struct filemap_page *entry;
	unsigned long page_offset;
	unsigned long chunk;
	unsigned long flags;

	if (!inode || !bytes)
		return;

	spin_lock_irqsave(&filemap_lock, flags);
	filemap_init_locked();
	while (count) {
		page_offset = (unsigned long)offset & PAGE_MASK;
		chunk = PAGE_SIZE - ((unsigned long)offset & ~PAGE_MASK);
		if (chunk > count)
			chunk = count;
		entry = filemap_lookup(inode, page_offset / PAGE_SIZE);
		if (entry && entry->state == FILEMAP_UPTODATE)
			memcpy((char *)entry->page +
			       ((unsigned long)offset & ~PAGE_MASK), bytes, chunk);
		offset += chunk;
		bytes += chunk;
		count -= chunk;
	}
	spin_unlock_irqrestore(&filemap_lock, flags);
}

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

static unsigned long filemap_nopage(struct vm_area_struct *vma,
				    unsigned long addr)
{
	struct file *file = vma->vm_file;
	unsigned long pos;
	unsigned long page;
	int error;

	addr &= PAGE_MASK;
	if (!file || !file->f_inode)
		return 0;
	pos = vma->vm_offset + addr - vma->vm_start;
	error = filemap_get_page(file, pos, FILEMAP_ACCESS_MMAP, &page);
	return error ? 0 : page;
}

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
	struct filemap_page *entry;

	pd = pde_offset(vma->vm_mm->pd, addr);
	if (!__pe_present(pd))
		return 0;

	pt = pte_offset(__vir(*pd), addr);
	if (!__pe_present(pt) || __pte_bad(pt) || !__pe_dirty(pt))
		return 0;

	offset = addr & ~PAGE_MASK;
	page = __vir((unsigned long)(*pt) & PAGE_MASK) + offset;
	pos = vma->vm_offset + addr - vma->vm_start;

	spin_lock_irqsave(&filemap_lock, flags);
	entry = filemap_lookup(file->f_inode, pos / PAGE_SIZE);
	if (!entry || entry->state != FILEMAP_UPTODATE) {
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

static void filemap_sync(struct vm_area_struct *vma,
			 unsigned long addr, unsigned long len, int flags)
{
	struct file *file = vma->vm_file;
	unsigned long end = addr + len;
	unsigned long chunk;

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

int generic_file_mmap(struct inode *inode,
		      struct file *file, struct vm_area_struct *vma)
{
	struct vm_operations_struct *ops;

	if (!inode || !file || !inode->i_sb || !S_ISREG(inode->i_mode))
		return -EACCES;
	if (!file->f_op || !file->f_op->read_page)
		return -EACCES;

	if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_WRITE)) {
		if (!file->f_op->write)
			return -EACCES;
		if (inode->i_sb->s_flags & MS_RDONLY)
			return -EROFS;
	}

	ops = (vma->vm_flags & VM_SHARED) ? &file_shared_mmap :
					  &file_private_mmap;
	fref(file);
	vma->vm_file = file;
	vma->vm_inode = inode;
	vma->vm_ops = ops;
	return 0;
}
