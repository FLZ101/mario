#ifndef _BUFFER_H
#define _BUFFER_H

#include <lib/list.h>
#include <lib/bitops.h>
#include <lib/atomic.h>

#include <semaphore.h>

struct buffer_head {
	dev_t b_dev;
	char *b_data;
	unsigned long b_count;
	unsigned long b_state;
	unsigned long b_sector;
	struct list_head b_lru;
	/* a buffer is used by only one process at a time */
	struct semaphore b_sem;
};

/* bh state bits */
#define BH_Dirty	0	/* the buffer is dirty */
#define BH_Up2date	1	/* the buffer contains valid data */

#define buffer_up2date(bh) 	test_bit(BH_Up2date, &(bh)->b_state)
#define buffer_dirty(bh) 	test_bit(BH_Dirty, &(bh)->b_state)

#define clear_up2date(bh)	clear_bit(BH_Up2date, &(bh)->b_state)
#define clear_dirty(bh)		clear_bit(BH_Dirty, &(bh)->b_state)

#define set_up2date(bh)		set_bit(BH_Up2date, &(bh)->b_state)
#define set_dirty(bh)		set_bit(BH_Dirty, &(bh)->b_state)

void buffer_init(void);

struct buffer_head *get_buffer(dev_t dev, unsigned long sector);
struct buffer_head *bread(dev_t dev, unsigned long sector);
int brelse(struct buffer_head *bh);

#endif /* _BUFFER_H */