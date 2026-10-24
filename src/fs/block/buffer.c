#include <misc.h>
#include <sched.h>
#include <wait.h>

#include <errno.h>

#include <fs/buffer.h>
#include <fs/blkdev.h>

#include <mm/page_alloc.h>

#include <lib/spinlock.h>
#include <lib/string.h>
#include <lib/stddef.h>
#include <lib/list.h>

/*
 * A buffer, which is described by a buffer_head, is mapping a sector.
 * Currently only sector size of 512 is supported.
 */

/* the number of pages reserved for buffer-cache */
#define NR_PAGE	16
#define NR_PAGE_ORDER	4

/* the number of BHs reserved for buffer-cache */
#define NR_BH	(NR_PAGE * 8)

static struct buffer_head all[NR_BH];

static struct list_head lru;

static wait_queue_t buffer_wait;

static spinlock_t buffer_lock;

void buffer_init(void)
{
	int i;
	char *buf;

	buf = (char *)pages_alloc(NR_PAGE_ORDER);
	if (!buf)
		hang("buffer_init fails");

	INIT_LIST_HEAD(&lru);

	for (i = 0; i < NR_BH; i++) {
		all[i].b_dev = 0;
		all[i].b_state = 0;
		all[i].b_count = 0;

		init_MUTEX(&all[i].b_sem);
		all[i].b_data = buf + i*512;
		list_add_tail(&all[i].b_lru, &lru);
	}

	init_wait_queue(&buffer_wait);
	INIT_LOCK(&buffer_lock);
}

static struct buffer_head *find_buffer(dev_t dev, unsigned long sector)
{
	int i;
	struct buffer_head *res = NULL;

	for (i = 0; i < NR_BH; i++)
		if (all[i].b_dev == dev && all[i].b_sector == sector) {
			res = all + i;
			/*
			 * Reserve this buffer;
			 * This is the only thing we can do to a buffer
			 * we didn't get.
			 */
			res->b_count++;
			break;
		}
	return res;
}

struct buffer_head *get_buffer(dev_t dev, unsigned long sector)
{
	int size;
	struct buffer_head *bh;

	if (get_sector_size(dev, &size) || size != 512)
		return NULL;

try:
	ACQUIRE_LOCK(&buffer_lock);
	if ((bh = find_buffer(dev, sector)))
		goto tail;

	bh = list_entry(lru.next, struct buffer_head, b_lru);
	if (bh->b_count) {
		sleep_on(&buffer_wait, TASK_UNINTERRUPTIBLE, &buffer_lock);
		goto try;
	}

	bh->b_count++;
	bh->b_dev = dev;
	bh->b_sector = sector;
	/*
	 * A newly recycled buffer still has the previous sector's state and
	 * data.  Clear both while holding buffer_lock, before publishing the new
	 * (dev, sector) pair.  Otherwise another CPU can find_buffer() the new
	 * sector and, if it wins the b_sem first, see the stale Up2date bit and
	 * return the old sector's bytes for this block.
	 */
	clear_dirty(bh);
	clear_up2date(bh);

tail:
	list_del(&bh->b_lru);
	list_add_tail(&bh->b_lru, &lru);
	RELEASE_LOCK(&buffer_lock);
	down(&bh->b_sem);
	return bh;
}

/* load a @sector in @dev into a buffer */
struct buffer_head *bread(dev_t dev, unsigned long sector)
{
	int tmp;
	struct buffer_head *bh;

	bh = get_buffer(dev, sector);
	if (!bh)
		return NULL;
	if (buffer_up2date(bh))
		return bh;
	tmp = blkdev_read(bh->b_dev, bh->b_sector, 1, bh->b_data);
	if (tmp) {
		brelse(bh);
		return NULL;
	}
	set_up2date(bh);
	clear_dirty(bh);
	return bh;
}

int bsync(struct buffer_head *bh)
{
	if (buffer_up2date(bh) && buffer_dirty(bh)) {
		clear_dirty(bh);
		return blkdev_write(bh->b_dev, bh->b_sector, 1, bh->b_data);
	}
	return 0;
}

/*
 * make @bh the last 'free' (which means @bh->b_count is 0) one in lru
 */
static void into_lru_list(struct buffer_head *bh)
{
	struct list_head *pos;
	struct buffer_head *tmp;

	list_for_each(pos, &lru) {
		tmp = list_entry(pos, struct buffer_head, b_lru);
		if (tmp->b_count)
			break;
	}
	list_del(&bh->b_lru);
	list_add_tail(&bh->b_lru, pos);
}

int brelse(struct buffer_head *bh)
{
	int res = 0, sync = 0;

	ACQUIRE_LOCK(&buffer_lock);
	if (!--bh->b_count) {
		/*
		 * The final user is releasing this buffer, so it must be flushed
		 * before the slot can be recycled.  bsync() may block on block-device
		 * I/O and therefore cannot run under buffer_lock.  Keep a temporary
		 * reference while flushing so get_buffer() cannot choose this same
		 * buffer for a different (dev, sector) pair until the flush is done.
		 * Without this, another CPU could change b_dev/b_sector while
		 * bsync() was still using them, writing stale data to a freshly
		 * recycled buffer.
		 */
		bh->b_count = 1;
		sync = 1;
	}
	RELEASE_LOCK(&buffer_lock);

	if (sync) {
		res = bsync(bh);

		/*
		 * Drop the temporary flush reference.  If another lookup for the old
		 * sector arrived while we were flushing it now owns the buffer, so
		 * leave the buffer busy; otherwise it becomes reusable.
		 */
		ACQUIRE_LOCK(&buffer_lock);
		if (!--bh->b_count) {
			into_lru_list(bh);
			wake_up_all(&buffer_wait);
		}
		RELEASE_LOCK(&buffer_lock);
	}

	up(&bh->b_sem);
	return res;
}
