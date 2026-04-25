#include <fs/fs.h>

extern int mario_get_block(struct super_block *sb, int nr, int *head, int *tail);
extern int mario_put_block(struct super_block *sb, int head, int tail);
extern int mario_nth_block(struct inode *inode, int n, int *block_nr);

/*
 * This function doesn't eat inode
 */
static int mario_file_truncate(struct inode *i, int size)
{
	int error, tmp;
	int head, tail, *next_block;
	unsigned long n, max_used, blocks;
	struct buffer_head *bh;

	if (!S_ISREG(i->i_mode))
		return -EINVAL;
	if (size < 0)
		return -EINVAL;
	max_used = i->i_block_size - 4;
	n = (size + max_used - 1) / max_used;
	blocks = i->i_nr_block;

	if (n > blocks) {
		error = mario_get_block(i->i_sb, n - blocks, &head, &tail);
		if (error)
			return error;
		if (!blocks) {	/* an empty file? */
			i->i_rdev = head;
			goto end;
		}
		/* go to the end of that block chain */
		if (1 > mario_nth_block(i, 0, &tmp))
			return -EIO;
		if (!(bh = bread(i->i_dev, tmp))) {
			mario_put_block(i->i_sb, head, tail);
			return -EIO;
		}
		next_block = (int *)(bh->b_data + max_used);
		*next_block = head;
		set_dirty(bh);
		brelse(bh);
		goto end;
	}
	if (n < blocks) {
		/* go to the last block */
		mario_nth_block(i, 0, &tail);
		if (n) {
			/* go to the nth block */
			if (n != mario_nth_block(i, n, &head))
				return -EIO;
			if (!(bh = bread(i->i_dev, head)))
				return -EIO;
			next_block = (int *)(bh->b_data + max_used);
			head = *next_block;
			*next_block = 0;	/* truncate that block chain */
			set_dirty(bh);
			brelse(bh);
		} else {
			head = i->i_rdev;
			i->i_rdev = MARIO_ZERO_ENTRY;
		}
		mario_put_block(i->i_sb, head, tail);
	}
end:
	i->i_size = size;
	i->i_nr_block = n;
	set_bit(I_Dirty, &i->i_state);
	return 0;
}

int mario_readlink(struct inode *inode, char *buf, int count)
{
	int error = 0;
	if (!S_ISLNK(inode->i_mode)) {
		error = -EINVAL;
		goto tail_1;
	}

	int block = inode->i_rdev;
	struct buffer_head *bh = bread(inode->i_dev, block);
	if (!bh)
		goto tail_1;

	int n = 0;
	while (bh->b_data[n] && n < inode->i_block_size - 4)
		++n;
	assert(!bh->b_data[n] && "Invalid MarioFS symlink");

	count = MIN(count, n);
	memcpy_tofs(buf, bh->b_data, count);
	brelse(bh);
	error = n;

tail_1:
	iput(inode);
	return error;
}

int mario_follow_link(struct inode *dir, struct inode *inode, int flags,
	struct inode **res_inode)
{
	int error;
	struct buffer_head * bh;

	*res_inode = NULL;
	if (!dir) {
		dir = current->fs->root;
		iref(dir);
	}
	if (!inode) {
		iput(dir);
		return -ENOENT;
	}
	if (!S_ISLNK(inode->i_mode)) {
		iput(dir);
		*res_inode = inode;
		return 0;
	}
	if (current->link_count > MAX_LINK_COUNT) {
		iput(dir);
		iput(inode);
		return -ELOOP;
	}

	int block = inode->i_rdev;
	if (!(bh = bread(inode->i_dev, block))) {
		iput(inode);
		iput(dir);
		return -EIO;
	}
	iput(inode);
	current->link_count++;
	error = open_namei(bh->b_data, flags, res_inode, dir);
	current->link_count--;
	brelse(bh);
	return error;
}

struct inode_operations mario_file_iops = {
	.truncate = mario_file_truncate
};

struct inode_operations mario_link_iops = {
	.readlink = mario_readlink,
	.follow_link = mario_follow_link,
};

int mario_file_read(struct inode *i, struct file *f, char *buf, int count)
{
	int n, block, res = 0;
	unsigned int start, end, read;
	struct buffer_head *bh;

	if (!i)
		return -EINVAL;
	if (!S_ISREG(i->i_mode))
		return -EINVAL;

	start = f->f_pos;
	end = i->i_size;
	if (start >= end)
		return 0;
	if (end > start + count)
		end = start + count;
	/* bytes of data a block contains */
	n = i->i_block_size - 4;
	// No more to read
	if (start/n + 1 != mario_nth_block(i, start/n + 1, &block))
		return 0;
read_a_block:
	if (!(bh = bread(i->i_dev, block)))
		return res;
	read = n - start % n;
	if (read > end - start)
		read = end - start;
	memcpy_tofs(buf, bh->b_data + start % n, read);
	block = *(int *)(bh->b_data + n);
	brelse(bh);
	res += read;
	buf += read;
	start += read;
	f->f_pos += read;
	if (start == end)
		return res;
	if (!block) {
		printk("%s%s", __FUNCTION__, ": Corrupt mariofs\n");
		return res;
	}
	goto read_a_block;
	return 0;
}

/*
 * Ugly, Ugly...
 */
int mario_file_write(struct inode *i, struct file *f, char *buf, int count)
{
	int n, block, nr, res = 0, ret = 0;
	unsigned int start, end, write;
	struct buffer_head *bh = NULL;

	if (!i)
		return -EINVAL;
	if (!S_ISREG(i->i_mode))
		return -EINVAL;

	down(&i->i_sem);
	if (f->f_flags & O_APPEND)
		start = i->i_size;
	else
		start = f->f_pos;
	end = start + count;
	if (!i->i_nr_block) {	/* empty file? */
		if ((ret = mario_get_block(i->i_sb, 1, &block, &block)))
			goto tail_3;
		i->i_rdev = block;
		i->i_nr_block++;
	}
	n = i->i_block_size - 4;
	nr = start/n + 1;

	block = i->i_rdev;
write_a_block:
	if (!(bh = bread(i->i_dev, block)))
		goto tail_2;
	if (nr && --nr)
		goto new_block;

	write = n - start % n;

	if (write > end - start)
		write = end - start;
	memcpy_fromfs(bh->b_data + start % n, buf, write);
	set_dirty(bh);
	res += write;
	buf += write;
	start += write;
	f->f_pos += write;
	if (start == end)
		goto tail_1;
new_block:
	block = *(int *)(bh->b_data + n);
	if (block)
		goto next_1;

	if ((ret = mario_get_block(i->i_sb, 1, &block, &block)))
		goto tail_1;

	*(int *)(bh->b_data + n) = block;
	set_dirty(bh); // !!! MUST ensure bh's dirty bit is set after modifying it

	i->i_nr_block++;
next_1:
	brelse(bh);
	goto write_a_block;
tail_1:
	brelse(bh);
tail_2:
	if (i->i_size < f->f_pos)
		i->i_size = f->f_pos;
	set_bit(I_Dirty, &i->i_state);
tail_3:
	up(&i->i_sem);
	if (ret && res == 0)
		return ret;

	if (i->i_nr_block > 0)
		assert((i->i_nr_block - 1) * n < i->i_size);
	assert(i->i_size <= i->i_nr_block * n);

	return res;
}

struct file_operations mario_file_fops = {
	.read = mario_file_read,
	.write = mario_file_write,
	.mmap = generic_file_mmap,
};
