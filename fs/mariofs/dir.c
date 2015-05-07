#include <fs/fs.h>
#include <fs/mariofs/mariofs.h>

extern int mario_get_block(struct super_block *sb, int nr, int *head, int *tail);
extern int mario_put_block(struct super_block *sb, int head, int tail);

/*
 * Find an entry in @dir, return the offset into rd (which is used as the 
 * inode number) of that entry 
 */
static int mario_find_entry(struct inode *dir, char *name, int len)
{
	int i, n;
	int block, *next_block;
	struct buffer_head *bh;
	struct mario_dir_entry *entry;

	if (len > MARIO_NAME_LEN - 1)
		return 0;

	/* maximum mario_dir_entries a block could contain */
	n = dir->i_block_size / MARIO_ENTRY_SIZE;

	block = dir->i_rdev;
try:
	if (!(bh = bread(dir->i_dev, block)))
		return 0;
	entry = (struct mario_dir_entry *)bh->b_data;

	for (i = 0; i < n; i++) {
		/* end of dir? */
		if (!entry[i].data)
			break;
		/* an unused entry? */
		if (entry[i].data == MARIO_FREE_ENTRY)
			continue;
		if (!strncmp(name, entry[i].name, len))
			return block * dir->i_block_size + i * MARIO_ENTRY_SIZE;
	}

	next_block = (int *)(bh->b_data + dir->i_block_size - 4);
	block = *next_block;
	brelse(bh);
	if (block)
		goto try;
	return 0;
}

static int mario_lookup(struct inode *dir, char *name, int len, struct inode **res)
{
	int ino;
	int ret = 0;

	*res = NULL;
	if (!dir)
		return -ENOENT;
	if (!S_ISDIR(dir->i_mode)) {
		ret = -ENOENT;
		goto tail;
	}
	if (!(ino = mario_find_entry(dir, name, len))) {
		ret = -ENOENT;
		goto tail;
	}
	if (!(*res = iget(dir->i_sb, ino)))
		ret = -EACCES;
tail:
	iput(dir);
	return ret;
}

int mario_create(struct inode *dir, char *name, int len, struct inode **res)
{
	int i, n, offset, ret = 0;
	int block, block_size, *next_block;
	struct buffer_head *bh;
	struct mario_dir_entry *entry;

	*res = NULL;
	if (len > MARIO_NAME_LEN - 1) {
		ret = -ENAMETOOLONG;
		goto tail;
	}

	block_size = dir->i_block_size;
	n = block_size / MARIO_ENTRY_SIZE;	/* entries a block contains */
	block = dir->i_rdev;
	/*
	 * Get an empty mario_dir_entry
	 */
try:
	if (!(bh = bread(dir->i_dev, block))) {
		ret = -EIO;
		goto tail;
	}
	entry = (struct mario_dir_entry *)bh->b_data;

	for (i = 0; i < n; i++)
		if (!entry[i].data || entry[i].data == MARIO_FREE_ENTRY) {
			offset = i * MARIO_ENTRY_SIZE;
			goto get_entry_done;
		}

	next_block = (int *)(bh->b_data + block_size - 4);
	if (*next_block) {
		block = *next_block;
		brelse(bh);
		goto try;
	}
	/*
	 * Expand that directory
	 */
	if (mario_get_block(dir->i_sb, 1, &block, &block)) {
		brelse(bh);
		ret = -ENOSPC;
		goto tail;
	}
	*next_block = block;
	set_dirty(bh);
	brelse(bh);
	if (!(bh = bread(dir->i_dev, block))) {
		ret = -EIO;
		goto tail;
	}
	entry = (struct mario_dir_entry *)bh->b_data;
	offset = 0;

get_entry_done:
	entry->data = MARIO_ZERO_ENTRY;
	entry->mode = MODE_REG;
	entry->size = 0;
	entry->blocks = 0;
	strncpy(entry->name, name, len);
	set_dirty(bh);
	brelse(bh);
	*res = iget(dir->i_sb, block * block_size + offset);
tail:
	iput(dir);
	return ret;
}

struct inode_operations mario_dir_iops = {
	mario_lookup,
	mario_create,
	NULL
};