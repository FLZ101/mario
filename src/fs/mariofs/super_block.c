#include <fs/fs.h>
#include <mm/kmalloc.h>

extern struct inode_operations mario_file_iops;
extern struct inode_operations mario_dir_iops;

extern struct inode_operations mario_link_iops;

extern struct file_operations mario_file_fops;
extern struct file_operations mario_dir_fops;

extern struct file_operations blkdev_fops;

extern struct file_operations chrdev_fops;

#define MARIO_BLOCK_MAP_KMALLOC_MAX	1020

static unsigned long mario_block_map_order(unsigned long bytes)
{
	unsigned long pages = (bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
	unsigned long order = 0;

	while ((1UL << order) < pages)
		order++;
	return order;
}

void mario_destroy_block_map(struct inode *inode)
{
	if (MARIO_INODE_BLOCK_MAP(inode)) {
		if (MARIO_INODE_BLOCK_MAP_KMALLOC(inode))
			kfree(MARIO_INODE_BLOCK_MAP(inode));
		else
			pages_free((unsigned long)MARIO_INODE_BLOCK_MAP(inode),
				MARIO_INODE_BLOCK_MAP_ORDER(inode));
	}

	MARIO_INODE_BLOCK_MAP(inode) = NULL;
	MARIO_INODE_BLOCK_MAP_LEN(inode) = 0;
	MARIO_INODE_BLOCK_MAP_ORDER(inode) = 0;
	MARIO_INODE_BLOCK_MAP_KMALLOC(inode) = 0;
}

static int mario_build_block_map(struct inode *inode)
{
	unsigned long blocks, bytes, block, i;
	unsigned long *map;
	struct buffer_head *bh;
	int next;

	if (MARIO_INODE_BLOCK_MAP(inode))
		return MARIO_INODE_BLOCK_MAP_LEN(inode);

	blocks = inode->i_nr_block;
	if (!blocks)
		return 0;
	if (blocks > MARIO_NR_BLOCKS(inode->i_sb))
		return -EIO;

	bytes = blocks * sizeof(unsigned long);
	if (bytes <= MARIO_BLOCK_MAP_KMALLOC_MAX) {
		map = kmalloc(bytes);
		if (!map)
			return -ENOMEM;
		MARIO_INODE_BLOCK_MAP_KMALLOC(inode) = 1;
		MARIO_INODE_BLOCK_MAP_ORDER(inode) = 0;
	} else {
		unsigned long order = mario_block_map_order(bytes);

		map = (unsigned long *)pages_alloc(order);
		if (!map)
			return -ENOMEM;
		MARIO_INODE_BLOCK_MAP_KMALLOC(inode) = 0;
		MARIO_INODE_BLOCK_MAP_ORDER(inode) = order;
	}

	MARIO_INODE_BLOCK_MAP(inode) = map;
	MARIO_INODE_BLOCK_MAP_LEN(inode) = 0;

	block = inode->i_rdev;
	for (i = 0; i < blocks; i++) {
		map[i] = block;
		MARIO_INODE_BLOCK_MAP_LEN(inode) = i + 1;
		if (i + 1 == blocks)
			break;

		if (!(bh = bread(inode->i_dev, block))) {
			mario_destroy_block_map(inode);
			return -EIO;
		}
		next = *(int *)(bh->b_data + inode->i_block_size - 4);
		brelse(bh);
		if (!next)
			break;
		block = next;
	}

	return MARIO_INODE_BLOCK_MAP_LEN(inode);
}

static int mario_chain_nth_block(struct inode *inode, int n, int *block_nr)
{
	unsigned long block = inode->i_rdev;
	struct buffer_head *bh;
	int next, i = 1;

	*block_nr = 0;
	if (!block)
		return 0;

	for (; n != i; i++) {
		if (!(bh = bread(inode->i_dev, block)))
			return -EIO;
		next = *(int *)(bh->b_data + inode->i_block_size - 4);
		brelse(bh);
		if (!next)
			break;
		block = next;
	}

	*block_nr = block;
	return i;
}

/*
 * Try to get block number of the @nth block of that chain.
 * Regular files use a lazily built full block map.  Directories and
 * non-regular files keep the simple chain walk.
 */
int mario_nth_block(struct inode *inode, int n, int *block_nr)
{
	unsigned long idx;
	int len;

	*block_nr = 0;
	if (n < 0)
		return -EINVAL;

	if (!S_ISREG(inode->i_mode))
		return mario_chain_nth_block(inode, n, block_nr);

	len = mario_build_block_map(inode);
	if (len < 0)
		return len;
	if (!len)
		return 0;

	if (n == 0)
		idx = len - 1;
	else if (n > len)
		return len;
	else
		idx = n - 1;

	*block_nr = MARIO_INODE_BLOCK_MAP(inode)[idx];
	return n ? n : len;
}

/*
 * Get a free block chain which contains @nr free blocks;
 * if this functions succeeds, *head will be block number of the first
 * block and *tail will be block number of the last one
 */
int mario_get_block(struct super_block *sb, int nr, int *head, int *tail)
{
	int i, j, k, n, *next_block;
	struct buffer_head *bh;

	*head = 0;
	*tail = 0;
	if (nr < 1 || !sb || sb->s_magic != MARIO_MAGIC)
		return -EINVAL;
	if (MARIO_NR_FREE(sb) < nr)
		return -ENOSPC;

	n = nr;
	down(&sb->s_sem);
	for (i = j = MARIO_FREE(sb); ; ) {
		k = j;
		if (!j) {	/* terribly bad */
			up(&sb->s_sem);
			printk("%s%s", __FUNCTION__, ": Corrupt mariofs\n");
			return -EIO;
		}
		/*
		 * get next block number
		 */
		if (!(bh = bread(sb->s_dev, j))) {
			up(&sb->s_sem);
			return -EIO;
		}
		next_block = (int *)(bh->b_data + sb->s_block_size - 4);
		j = *next_block;

		if (!--nr) {
			*next_block = 0;	/* may be useful */
			set_dirty(bh);
			brelse(bh);
			break;
		}
		brelse(bh);
	}
	MARIO_FREE(sb) = j;
	MARIO_NR_FREE(sb) -= n;
	set_bit(SB_Dirty, &(sb)->s_state);
	up(&sb->s_sem);
	*head = i;
	*tail = k;
	return 0;
}

/*
 * Insert a block chain into the free block chain;
 * head is block number of the first block and *tail is
 * block number of the last one
 */
int mario_put_block(struct super_block *sb, int head, int tail)
{
	struct buffer_head *bh;

	if (!(bh = bread(sb->s_dev, tail)))
		return -EIO;

	down(&sb->s_sem);
	*(int *)(bh->b_data + sb->s_block_size - 4) = MARIO_FREE(sb);
	MARIO_FREE(sb) = head;
	set_bit(SB_Dirty, &(sb)->s_state);
	up(&sb->s_sem);

	set_dirty(bh);
	brelse(bh);
	return 0;
}

static int mario_read_inode(struct inode *i)
{
	struct super_block *sb;
	struct buffer_head *bh = NULL;
	struct mario_dir_entry *entry;

	sb = i->i_sb;

	if (i->i_ino == MARIO_ROOT_INO) {
		entry = &MARIO_ROOT(sb);
	} else {
		unsigned long off, block;

		off = i->i_ino;
		block = off / (sb->s_block_size);
		off = off % (sb->s_block_size);

		if (!(bh = bread(sb->s_dev, block)))
			return -EIO;

		entry = (struct mario_dir_entry *)(bh->b_data + off);
	}

	i->i_rdev = entry->data;
	i->i_mode = entry->mode;
	i->i_size = entry->size;
	i->i_nr_block = entry->blocks;
	i->i_block_size = sb->s_block_size;
	strcpy(MARIO_INODE_NAME(i), entry->name);
	mario_destroy_block_map(i);

	if (S_ISREG(i->i_mode)) {
		i->i_op = &mario_file_iops;
		i->i_fop = &mario_file_fops;
	} else if (S_ISDIR(i->i_mode)) {
		i->i_op = &mario_dir_iops;
		i->i_fop = &mario_dir_fops;
	} else if (S_ISBLK(i->i_mode)) {
		i->i_op = NULL;
		i->i_fop = &blkdev_fops;
	} else if (S_ISCHR(i->i_mode)) {
		i->i_op = NULL;
		i->i_fop = &chrdev_fops;
	} else if (S_ISFIFO(i->i_mode)) {
		/* Named FIFOs reuse the anonymous pipe read/write machinery. */
		i->i_op = NULL;
		i->i_fop = &fifo_fops;
	} else if (S_ISLNK(i->i_mode)) {
		i->i_op = &mario_link_iops;
		i->i_fop = NULL;
	}

	i->i_atime = MARIO_TIME_BASE(i->i_sb) + entry->atime;
	i->i_mtime = MARIO_TIME_BASE(i->i_sb) + entry->mtime;
	i->i_ctime = MARIO_TIME_BASE(i->i_sb) + entry->mtime;

	if (bh)
		brelse(bh);
	return 0;
}

static int mario_write_inode(struct inode *i)
{
	unsigned long off, block;
	struct buffer_head *bh;
	struct mario_dir_entry *entry;

	off = i->i_ino;
	block = off / (i->i_block_size);
	off = off % (i->i_block_size);

	if (!(bh = bread(i->i_dev, block)))
		return -EIO;
	entry = (struct mario_dir_entry *)(bh->b_data + off);
	if (strcmp(entry->name, MARIO_INODE_NAME(i))) {
		/*
		 * The directory slot has been reused for another name while
		 * an old inode object was still being released.  Do not let
		 * the stale inode overwrite the new directory entry.
		 */
		brelse(bh);
		return 0;
	}
	entry->data = i->i_rdev;
	entry->mode = i->i_mode;
	entry->size = i->i_size;
	entry->blocks = i->i_nr_block;

	entry->atime = i->i_atime - MARIO_TIME_BASE(i->i_sb);
	entry->mtime = i->i_mtime - MARIO_TIME_BASE(i->i_sb);

	strcpy(entry->name, MARIO_INODE_NAME(i));
	set_dirty(bh);
	brelse(bh);
	return 0;
}

int mario_write_super(struct super_block *sb)
{
	struct buffer_head *bh;
	struct mario_super_block *mario_sb;

	if (sb->s_magic != MARIO_MAGIC)
		return -EINVAL;
	if (!(bh = bread(sb->s_dev, 0)))
		return -EIO;
	mario_sb = (struct mario_super_block *)bh->b_data;
	mario_sb->nr_free = MARIO_NR_FREE(sb);
	mario_sb->free = MARIO_FREE(sb);
	brelse(bh);
	return 0;
}

static void mario_clear_inode(struct inode *i)
{
	mario_destroy_block_map(i);
}

static struct super_operations mario_sops = {
	.read_inode = mario_read_inode,
	.write_inode = mario_write_inode,
	.write_super = mario_write_super,
	.clear_inode = mario_clear_inode
};

static struct super_block *mario_read_super(struct super_block *sb)
{
	struct buffer_head *bh;
	struct mario_super_block *mario_sb;

	if (!sb->s_dev)
		return NULL;

	if (!(bh = bread(sb->s_dev, 0)))
		return NULL;

	mario_sb = (struct mario_super_block *)bh->b_data;
	if (mario_sb->magic != MARIO_MAGIC)
		goto fail;

	/* Currently sector_size and block_size must be 512 */
	if (mario_sb->sector_size != 512 || mario_sb->block_size != 512)
		goto fail;

	MARIO_SEC_PER_BLOCK(sb) = 1;
	MARIO_NR_BLOCKS(sb) = mario_sb->nr_blocks;
	MARIO_NR_FREE(sb) = mario_sb->nr_free;
	MARIO_FREE(sb) = mario_sb->free;
	MARIO_TIME_BASE(sb) = mario_sb->time_base;
	MARIO_ROOT(sb) = mario_sb->root;

	sb->s_block_size = mario_sb->block_size;
	sb->s_magic = MARIO_MAGIC;
	sb->s_op = &mario_sops;
	sb->s_mounted = iget(sb, MARIO_ROOT_INO);
	if (!sb->s_mounted)
		goto fail;
	brelse(bh);
	return sb;
fail:
	brelse(bh);
	return NULL;
}

struct file_system_type mariofs = {
	mario_read_super,
	LIST_HEAD_INIT(mariofs.list),
	"mariofs"
};
