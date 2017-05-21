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

struct inode_operations mario_file_iops = {
	NULL,
	NULL,
	mario_file_truncate,
	NULL,
	NULL,
	NULL
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
		early_print("%s%s", __FUNCTION__, ": Corrupt mariofs\n");
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
	int n, block, nr, res = 0;
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
		if (mario_get_block(i->i_sb, 1, &block, &block))
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

	if (mario_get_block(i->i_sb, 1, &block, &block))
		goto tail_1;

	*(int *)(bh->b_data + n) = block;
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
	return res;
}

struct file_operations mario_file_fops = {
	NULL,	/* open */
	NULL,	/* release */
	NULL,	/* lseek - default */
	mario_file_read,
	mario_file_write,
	NULL,	/* readdir */
	generic_file_mmap
};
