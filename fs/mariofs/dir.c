#include <fs/fs.h>
#include <fs/mariofs/mariofs.h>

extern int mario_get_block(struct super_block *sb, int nr, int *head, int *tail);
extern int mario_put_block(struct super_block *sb, int head, int tail);
extern int mario_nth_block(struct inode *inode, int n, int *block_nr);
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
	n = (dir->i_block_size - 4) / MARIO_ENTRY_SIZE;

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
	*res = iget(dir->i_sb, ino);
tail:
	iput(dir);
	return ret;
}

static int mario_add_entry(struct inode *dir, struct mario_dir_entry *__entry,
	struct inode **res)
{
	int i, n, ret = 0;
	int block, block_size, *next_block;
	struct buffer_head *bh;
	struct mario_dir_entry *entry;

	*res = NULL;

	block_size = dir->i_block_size;
	n = (block_size - 4) / MARIO_ENTRY_SIZE;	/* entries a block contains */
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
			entry += i;
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

get_entry_done:
	memcpy(entry, __entry, MARIO_ENTRY_SIZE);
	set_dirty(bh);
	brelse(bh);
	*res = iget(dir->i_sb, block * block_size + 
			(char *)entry - bh->b_data);
tail:
	iput(dir);
	return ret;
}

int mario_create(struct inode *dir, char *name, int len, struct inode **res)
{
	struct mario_dir_entry entry;

	if (len > MARIO_NAME_LEN - 1) {
		iput(dir);
		return -ENAMETOOLONG;
	}

	entry.data = MARIO_ZERO_ENTRY;
	entry.mode = MODE_REG;
	entry.size = 0;
	entry.blocks = 0;
	strncpy(entry.name, name, len);

	return mario_add_entry(dir, &entry, res);
}

struct inode_operations mario_dir_iops = {
	mario_lookup,
	mario_create,
	NULL
};

int mario_readdir(struct inode *dir, struct file *f, void *dirent, filldir_t filldir)
{
	int i, n, size;
	int block, start;
	struct buffer_head *bh;
	struct mario_dir_entry *entry;

	size = dir->i_block_size;
	n = (size - 4) / MARIO_ENTRY_SIZE;
	if (f->f_pos % MARIO_ENTRY_SIZE)
		return -EINVAL;
	start = f->f_pos / MARIO_ENTRY_SIZE;
	if (start/n + 1 != mario_nth_block(dir, start/n + 1, &block))
		return -EIO;
read_a_block:
	if (!(bh = bread(dir->i_dev, block)))
		return 0;
	entry = (struct mario_dir_entry *)bh->b_data;
	for (i = start % n; i < n; i++) {
		/* end of dir? */
		if (!entry[i].data)
			break;
		/* an unused entry? */
		if (entry[i].data == MARIO_FREE_ENTRY)
			continue;
		if (filldir(dirent, entry[i].name, strlen(entry[i].name), f->f_pos, 
			block * size + i * MARIO_ENTRY_SIZE)) {
			brelse(bh);
			return 0;	
		}
		f->f_pos += MARIO_ENTRY_SIZE;
		start++;
	}
	brelse(bh);
	block = *(int *)(bh->b_data + size - 4);
	if (block)
		goto read_a_block;
	return 0;
}

struct file_operations mario_dir_fops = {
	NULL,	/* open */
	NULL,	/* release */
	NULL,	/* lseek - default */
	NULL,	/* read */
	NULL,	/* write */
	mario_readdir
};
