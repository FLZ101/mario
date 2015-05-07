#include <fs/fs.h>

extern struct inode_operations mario_file_iops;
extern struct inode_operations mario_dir_iops;

extern struct file_operations mario_file_fops;
extern struct file_operations mario_dir_fops;

/*
 * Try to get block number of the @nth block of that chain;
 * if error occurs
 *   a negative value is returned
 * else
 *   return the block we touched
 * NOTE:
 *   @inode:	a directory or a file not empty
 *   @n:	0 means the last block
 */
int mario_nth_block(struct inode *inode, int n, int *block_nr)
{
	int i, j, next_block;
	struct buffer_head *bh;

	*block_nr = 0;

	for (i = 1, j = inode->i_rdev; n != i; i++) {
		if (!(bh = bread(inode->i_dev, j)))
			return -EIO;
		next_block = *(int *)(bh->b_data + inode->i_block_size - 4);
		brelse(bh);
		if (!next_block)
			break;
		j = next_block;
	}
	*block_nr = j;
	return i;
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
	unsigned long off, block;
	struct super_block *sb;
	struct buffer_head *bh;
	struct mario_dir_entry *entry;

	sb = i->i_sb;
	off = i->i_ino;
	block = off / (sb->s_block_size);
	off = off % (sb->s_block_size);

	if (!(bh = bread(sb->s_dev, block)))
		return -EIO;

	entry = (struct mario_dir_entry *)(bh->b_data + off);
	i->i_rdev = entry->data;
	i->i_mode = entry->mode;
	i->i_size = entry->size;
	i->i_nr_block = entry->blocks;
	i->i_block_size = sb->s_block_size;
	strncpy(MARIO_INODE_NAME(i), entry->name, MARIO_NAME_LEN);

	if (S_ISREG(i->i_mode)) {
		i->i_op = &mario_file_iops;
		i->i_fop = &mario_file_fops;
	} else if (S_ISDIR(i->i_mode)) {
		i->i_op = &mario_dir_iops;
		i->i_fop = &mario_dir_fops;
	} else if (S_ISBLK(i->i_mode)) {
		i->i_op = NULL;
		i->i_fop = NULL;
	} else if (S_ISCHR(i->i_mode)) {
		i->i_op = NULL;
		i->i_fop = NULL;
	}

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
	entry->data = i->i_rdev;
	entry->mode = i->i_mode;
	entry->size = i->i_size;
	entry->blocks = i->i_nr_block;
	strncpy(entry->name, MARIO_INODE_NAME(i), MARIO_NAME_LEN);
	set_dirty(bh);
	brelse(bh);
	return 0;
}

static struct super_operations mario_sops = {
	mario_read_inode, 
	mario_write_inode,
	NULL
};

static struct super_block *mario_read_super(struct super_block *sb, void *data)
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
