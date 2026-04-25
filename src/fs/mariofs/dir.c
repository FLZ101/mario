#include <fs/fs.h>
#include <fs/mariofs/mariofs.h>
#include <fs/dirent.h>

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

	if (len > MARIO_NAME_LEN - 1 || len < 1)
		return 0;

	assert(!(len == 1 && name[0] == '.'));

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
		if (!strncmp(name, entry[i].name, len) && !entry[i].name[len]) {
			brelse(bh);	/* !!! */

			ino_t ino = block * dir->i_block_size + i * MARIO_ENTRY_SIZE;
			// For "." and ".." directory entries, the data field is the inode number
			if (len == 2 && name[0] == '.' && name[1] == '.') {
				ino = entry[i].data;
			}
			return ino;
		}
	}

	next_block = (int *)(bh->b_data + dir->i_block_size - 4);
	block = *next_block;
	brelse(bh);
	if (block)
		goto try;
	return 0;
}

// NOTE: dir is eaten
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

	if (len == 1 && name[0] == '.') {
		*res = dir;
		return 0;
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

	if (res)
		*res = NULL;

	block_size = dir->i_block_size;
	n = (block_size - 4) / MARIO_ENTRY_SIZE;	/* entries a block contains */
	block = dir->i_rdev;
	/*
	 * Get an empty mario_dir_entry
	 */
try:
	if (!(bh = bread(dir->i_dev, block)))
		return -EIO;

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
	if ((ret = mario_get_block(dir->i_sb, 1, &block, &block))) {
		brelse(bh);
		return ret;
	}
	*next_block = block;
	set_dirty(bh);
	brelse(bh);
	if (!(bh = bread(dir->i_dev, block)))
		return -EIO;
	entry = (struct mario_dir_entry *)bh->b_data;
	memset(entry + 1, 0, MARIO_ENTRY_SIZE);	/* end of dir */

get_entry_done:
	memcpy(entry, __entry, MARIO_ENTRY_SIZE);
	set_dirty(bh);
	brelse(bh);
	if (res)
		*res = iget(dir->i_sb, block * block_size +
			(char *)entry - bh->b_data);
	return 0;
}

int mario_create(struct inode *dir, char *name, int len, struct inode **res)
{
	int err = 0;
	struct mario_dir_entry entry;

	if (len > MARIO_NAME_LEN - 1) {
		err = -ENAMETOOLONG;
		goto tail;
	}

	entry.data = MARIO_ZERO_ENTRY;
	entry.mode = MODE_REG;
	entry.size = 0;
	entry.blocks = 0;
	strncpy(entry.name, name, len);
	entry.name[len] = '\0';	/* !!! */

	err = mario_add_entry(dir, &entry, res);
tail:
	iput(dir);
	return err;
}

/*
 * Whether a directory is empty?
 */
static int empty_dir(struct inode *dir)
{
	int i, n, count;
	int block, *next_block;
	struct buffer_head *bh;
	struct mario_dir_entry *entry;

	/* maximum mario_dir_entries a block could contain */
	n = (dir->i_block_size - 4) / MARIO_ENTRY_SIZE;
	count = 0;
	block = dir->i_rdev;
try:
	if (!(bh = bread(dir->i_dev, block)))
		return 0;
	entry = (struct mario_dir_entry *)bh->b_data;

	for (i = 0; i < n; i++) {
		/* end of dir? */
		if (!entry[i].data) {
			brelse(bh);
			return 1;
		}
		/* an unused entry? */
		if (entry[i].data == MARIO_FREE_ENTRY)
			continue;
		if (++count > 2) {
			brelse(bh);
			return 0;
		}
	}

	next_block = (int *)(bh->b_data + dir->i_block_size - 4);
	block = *next_block;
	brelse(bh);
	if (block)
		goto try;
	return 1;
}

/*
 * Delete an entry so that @mario_find_entry can not get it any more
 * If @free we also free blocks that entry occupies
 */
static int mario_del_entry(struct inode *i, int free)
{
	int ret = 0;
	unsigned long off, block;
	struct buffer_head *bh;
	struct mario_dir_entry *entry;

	off = i->i_ino;
	block = off / (i->i_block_size);
	off = off % (i->i_block_size);

	if (!(bh = bread(i->i_dev, block)))
		return -EIO;
	entry = (struct mario_dir_entry *)(bh->b_data + off);
	/* device file? */
	if (S_ISBLK(entry->mode) || S_ISCHR(entry->mode))
		goto tail_1;
	/* empty file? */
	if (entry->data == MARIO_ZERO_ENTRY)
		goto tail_1;
	if (!free)
		goto tail_1;
	/* goto the last block */
	if (1 > mario_nth_block(i, 0, (int*)&block)) {
		ret = -EIO;
		goto tail_2;
	}
	ret = mario_put_block(i->i_sb, entry->data, block);
	if (ret)
		goto tail_2;
tail_1:
	entry->data = MARIO_FREE_ENTRY;
	set_dirty(bh);
tail_2:
	brelse(bh);
	return ret;
}

int mario_rmdir(struct inode *dir, char *name, int len)
{
	int ino, ret = 0;
	struct inode *inode;

	if (len == 1 && name[0] == '.')
		return -EINVAL;

	down(&dir->i_sem);
	if (!(ino = mario_find_entry(dir, name, len))) {
		ret = -ENOENT;
		goto tail;
	}
	inode = iget(dir->i_sb, ino);
	if (!S_ISDIR(inode->i_mode)) {
		ret = -ENOTDIR;
		goto tail;
	}
	/*
	 * We can not remove the root directory
	 */
	if (inode->i_rdev == MARIO_ROOT_BLOCK) {
		ret = -EBUSY;
		goto tail;
	}

	if (inode->i_count > 1) {
		ret = -EBUSY;
		goto tail;
	}
	if (!empty_dir(inode)) {
		ret = -ENOTEMPTY;
		goto tail;
	}
	mario_del_entry(inode, 1);
	iput(inode);	/* This inode would not be got any more */
tail:
	up(&dir->i_sem);
	iput(dir);
	return ret;
}

int mario_mkdir(struct inode *dir, char *name, int len)
{
	int block, ret = 0;
	struct mario_dir_entry entry, *pe;
	struct buffer_head *bh;

	if (len > MARIO_NAME_LEN - 1) {
		ret = -ENAMETOOLONG;
		goto tail_1;
	}

	if (len == 1 && name[0] == '.')
		return -EINVAL;

	if (mario_find_entry(dir, name, len)) {
		ret = -EEXIST;
		goto tail_1;
	}
	if ((ret = mario_get_block(dir->i_sb, 1, &block, &block)))
		goto tail_1;

	entry.data = block;
	entry.mode = MODE_DIR;
	entry.size = 0;
	entry.blocks = 0;
	strncpy(entry.name, name, len);
	entry.name[len] = '\0';	/* !!! */

	if (!(bh = bread(dir->i_dev, block))) {
		ret = -EIO;
		goto tail_2;
	}
	pe = (struct mario_dir_entry *)bh->b_data;
	pe[0].data = 0; // inode number. To be set
	pe[0].mode = MODE_DIR;
	pe[0].size = 0;
	pe[0].blocks = 0;
	strcpy(pe[0].name, ".");
	pe[1].data = dir->i_ino; // inode number
	pe[1].mode = MODE_DIR;
	pe[1].size = 0;
	pe[1].blocks = 0;
	strcpy(pe[1].name, "..");
	memset(pe + 2, 0, MARIO_ENTRY_SIZE);	/* end of dir */

	struct inode *inode = NULL;
	ret = mario_add_entry(dir, &entry, &inode);
	if (ret)
		goto tail_3;
	pe[0].data = inode->i_ino;
	iput(inode);

	set_dirty(bh);
	brelse(bh);
	iput(dir);
	return 0;

tail_3:
	brelse(bh);
tail_2:
	mario_put_block(dir->i_sb, block, block);
tail_1:
	iput(dir);
	return ret;
}

/*
 * For mariofs `unlink' is `delete'
 */
int mario_unlink(struct inode *dir, char *name, int len)
{
	int ino, ret = 0;
	struct inode *inode;

	if (len == 1 && name[0] == '.')
		return -EINVAL;

	down(&dir->i_sem);
	if (!(ino = mario_find_entry(dir, name, len))) {
		ret = -ENOENT;
		goto tail;
	}
	inode = iget(dir->i_sb, ino);
	if (S_ISDIR(inode->i_mode)) {
		ret = -EPERM;
		goto tail;
	}
	if (inode->i_count > 1) {
		ret = -EBUSY;
		goto tail;
	}
	mario_del_entry(inode, 1);
	iput(inode);	/* This inode would not be got any more */
tail:
	up(&dir->i_sem);
	iput(dir);
	return ret;
}

static
int do_mario_rename(struct inode *old_dir, char *old_name, int old_len,
	struct inode *new_dir, char *new_name, int new_len)
{
	int ino, error = 0;
	struct inode *old_inode, *new_inode;
	struct mario_dir_entry entry;

	if (old_len > MARIO_NAME_LEN - 1 || new_len > MARIO_NAME_LEN - 1)
		return -ENAMETOOLONG;
	if (!(ino = mario_find_entry(old_dir, old_name, old_len)))
		return -ENOENT;
	old_inode = iget(old_dir->i_sb, ino);

	if (!(ino = mario_find_entry(new_dir, new_name, new_len)))
		goto next_1;
	new_inode = iget(new_dir->i_sb, ino);

	if (new_inode == old_inode) {
		iput(new_inode);
		iput(old_inode);
		return 0;
	}

	/* do some checks */
	error = 0;
	if (S_ISDIR(new_inode->i_mode)) {
		if (!S_ISDIR(old_inode->i_mode))
			error = -EISDIR;
		if (!empty_dir(new_inode))
			error = -ENOTEMPTY;
	} else {
		if (S_ISDIR(old_inode->i_mode))
			error = -ENOTDIR;
	}
	if (error) {
		iput(new_inode);
		iput(old_inode);
		return error;
	}

	/* delete new_inode */
	down(&new_dir->i_sem);
	if (new_inode->i_count > 1)
		error = -EBUSY;
	else
		error = mario_del_entry(new_inode, 1);
	iput(new_inode);
	up(&new_dir->i_sem);
	if (error) {
		iput(old_inode);
		return error;
	}

next_1:
	/* delete old_inode but not free blocks occupied */
	down(&old_dir->i_sem);
	if (old_inode->i_count > 1) {
		error = -EBUSY;
		goto tail_1;
	}

	error = mario_del_entry(old_inode, 0);
	if (error)
		goto tail_1;
	/* add old_inode to new_dir */
	entry.data = old_inode->i_rdev;
	entry.mode = old_inode->i_mode;
	entry.size = old_inode->i_size;
	entry.blocks = old_inode->i_nr_block;
	strncpy(entry.name, new_name, new_len);
	entry.name[new_len] = '\0';
	/*
	 * If old_inode is a directory we need to change its parent
	 */
	if (S_ISDIR(old_inode->i_mode) && old_dir != new_dir) {
		struct buffer_head *bh;
		struct mario_dir_entry *ent;

		bh = bread(old_inode->i_dev, old_inode->i_rdev);
		if (!bh) {
			error = -EIO;
			goto tail_1;
		}
		ent = (struct mario_dir_entry *)bh->b_data;
		ent[1].data = new_dir->i_rdev;	/* .. */
		set_dirty(bh);
		brelse(bh);
	}
	error = mario_add_entry(new_dir, &entry, NULL);
tail_1:
	iput(old_inode);
	up(&old_dir->i_sem);
	return error;
}

int mario_rename(struct inode *old_dir, char *old_name, int old_len,
	struct inode *new_dir, char *new_name, int new_len)
{
	/* To simplify things */
	static struct semaphore sem = INIT_MUTEX(sem);
	int ret;

	down(&sem);
	ret = do_mario_rename(old_dir, old_name, old_len,
		new_dir, new_name, new_len);
	iput(old_dir);
	iput(new_dir);
	up(&sem);
	return ret;
}

int mario_mknod(struct inode *dir, char *name, int len, int mode, int rdev)
{
	int ino, error = 0;
	struct mario_dir_entry entry;

	if (len > MARIO_NAME_LEN - 1) {
		error = -ENAMETOOLONG;
		goto tail_1;
	}

	if (len == 1 && name[0] == '.')
		return -EINVAL;

	down(&dir->i_sem);
	if ((ino = mario_find_entry(dir, name, len))) {
		error = -EEXIST;
		goto tail_2;
	}
	entry.mode = mode;
	if (entry.mode == MODE_REG)
		entry.data = MARIO_ZERO_ENTRY;
	else
		entry.data = rdev;
	entry.size = 0;
	entry.blocks = 0;
	strncpy(entry.name, name, len);
	entry.name[len] = '\0';
	error = mario_add_entry(dir, &entry, NULL);
tail_2:
	up(&dir->i_sem);
tail_1:
	iput(dir);
	return error;
}

int mario_symlink(struct inode *dir, char *basename, int namelen, char *oldname)
{
	int ino, error = 0;
	struct mario_dir_entry entry;

	int n = strlen(oldname);
	if (n + 1 > dir->i_block_size - 4) {
		error = -ENAMETOOLONG;
		goto tail_1;
	}

	if (namelen > MARIO_NAME_LEN - 1) {
		error = -ENAMETOOLONG;
		goto tail_1;
	}

	down(&dir->i_sem);
	if ((ino = mario_find_entry(dir, basename, namelen))) {
		error = -EEXIST;
		goto tail_2;
	}
	entry.mode = MODE_LNK;
	entry.data = MARIO_ZERO_ENTRY;
	entry.size = 0;
	entry.blocks = 0;
	strncpy(entry.name, basename, namelen);
	entry.name[namelen] = '\0';

	struct inode *inode = NULL; // inode for the new entry
	error = mario_add_entry(dir, &entry, &inode);
	if (error)
		goto tail_2;

	// Write oldname to the inode created
	int block = 0;
	error = mario_get_block(inode->i_sb, 1, &block, &block);
	if (error)
		goto tail_3;
	inode->i_rdev = block;
	inode->i_nr_block++;

	struct buffer_head *bh = bread(inode->i_dev, block);
	if (!bh)
		goto tail_3;
	memcpy(bh->b_data, oldname, n);
	bh->b_data[n] = '\0';

	set_dirty(bh);
	brelse(bh);

	goto tail_2;

tail_3:
	mario_del_entry(inode, 1);
tail_2:
	up(&dir->i_sem);
tail_1:
	iput(dir);
	return error;
}

struct inode_operations mario_dir_iops = {
	.lookup = mario_lookup,
	.create = mario_create,
	.rmdir = mario_rmdir,
	.mkdir = mario_mkdir,
	.link = NULL,	/* mariofs doesn't support hard-link */
	.unlink = mario_unlink,
	.rename = mario_rename,
	.mknod = mario_mknod,
	.symlink = mario_symlink,
};

int mario_readdir(struct inode *dir, struct file *f, void *dirent, filldir_t fn)
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
	// No more to read
	if (start/n + 1 != mario_nth_block(dir, start/n + 1, &block))
		return 0;
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
        if (fn(dirent, entry[i].name, strlen(entry[i].name), f->f_pos,
               block * size + i * MARIO_ENTRY_SIZE, IFTODT(entry[i].mode))) {
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
	.readdir = mario_readdir
};
