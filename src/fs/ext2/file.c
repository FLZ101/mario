#include <fs/fs.h>
#include <fs/ext2/ext2.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

extern uint64_t current_time(void);

#define EXT2_ADDRS_PER_BLOCK(sb) (EXT2_BLOCK_SIZE(sb) / sizeof(uint32_t))

/*
 * Read a symlink target into a kernel buffer.  Small ext2 symlinks are stored
 * inline in the inode's block pointers; larger ones use the same block mapping
 * as a regular file but do not need the regular-file operation table.
 */
static int ext2_read_symlink_target(struct inode *inode, char *buf, size_t size)
{
	struct ext2_inode *raw = &EXT2_INODE(inode).raw;
	uint32_t copied;

	if (size > (size_t)inode->i_size)
		size = inode->i_size;

	/* Fast symlinks store the target directly in i_block[]. */
	if (!inode->i_nr_block) {
		copied = MIN(size, (size_t)sizeof(raw->i_block));
		memcpy(buf, raw->i_block, copied);
		buf[copied] = '\0';
		return copied;
	}

	copied = 0;
	while (copied < size) {
		uint32_t block_index = copied / EXT2_BLOCK_SIZE(inode->i_sb);
		uint32_t offset = copied % EXT2_BLOCK_SIZE(inode->i_sb);
		uint32_t chunk = EXT2_BLOCK_SIZE(inode->i_sb) - offset;
		uint32_t physical = ext2_bmap(inode, block_index, 0);
		uint8_t *block_buf;
		int error;

		if (chunk > size - copied)
			chunk = size - copied;
		if (!physical)
			return -EIO;

		block_buf = kmalloc(EXT2_BLOCK_SIZE(inode->i_sb));
		if (!block_buf)
			return -ENOMEM;
		error = ext2_read_block(inode->i_sb, physical, block_buf);
		if (error) {
			kfree(block_buf);
			return error;
		}
		memcpy(buf + copied, block_buf + offset, chunk);
		kfree(block_buf);
		copied += chunk;
	}

	buf[copied] = '\0';
	return copied;
}

static int ext2_file_read(struct inode *inode, struct file *file, char *buf,
			  int count)
{
	uint32_t total;
	uint32_t done;

	if (!inode || !S_ISREG(inode->i_mode))
		return -EINVAL;
	if (file->f_pos >= inode->i_size)
		return 0;

	done = 0;
	total = MIN((uint64_t)count, (uint64_t)(inode->i_size - file->f_pos));
	while (done < total) {
		uint32_t block_index = file->f_pos / EXT2_BLOCK_SIZE(inode->i_sb);
		uint32_t offset = file->f_pos % EXT2_BLOCK_SIZE(inode->i_sb);
		uint32_t chunk = EXT2_BLOCK_SIZE(inode->i_sb) - offset;
		uint32_t physical = ext2_bmap(inode, block_index, 0);
		uint8_t *block_buf;
		int error;

		if (chunk > total - done)
			chunk = total - done;

		block_buf = kmalloc(EXT2_BLOCK_SIZE(inode->i_sb));
		if (!block_buf)
			return done ? (int)done : -ENOMEM;

		if (!physical) {
			memset(block_buf, 0, EXT2_BLOCK_SIZE(inode->i_sb));
		} else {
			error = ext2_read_block(inode->i_sb, physical, block_buf);
			if (error) {
				kfree(block_buf);
				return done ? (int)done : error;
			}
		}

		memcpy_tofs(buf + done, block_buf + offset, chunk);
		kfree(block_buf);
		done += chunk;
		file->f_pos += chunk;
	}

	return done;
}

static int ext2_file_write(struct inode *inode, struct file *file, char *buf,
			   int count)
{
	struct super_block *sb = inode->i_sb;
	struct ext2_inode *raw = &EXT2_INODE(inode).raw;
	uint32_t start;
	uint32_t total = 0;
	uint32_t block_size = EXT2_BLOCK_SIZE(sb);
	int error = 0;

	if (!inode || !S_ISREG(inode->i_mode))
		return -EINVAL;

	down(&inode->i_sem);
	start = (file->f_flags & O_APPEND) ? inode->i_size : file->f_pos;

	while (total < (uint32_t)count) {
		uint32_t block_index = start / block_size;
		uint32_t offset = start % block_size;
		uint32_t chunk = block_size - offset;
		uint32_t physical;
		uint8_t *block_buf;

		if (chunk > (uint32_t)count - total)
			chunk = (uint32_t)count - total;

		physical = ext2_bmap(inode, block_index, 1);
		if (!physical) {
			error = -EIO;
			break;
		}

		block_buf = kmalloc(block_size);
		if (!block_buf) {
			error = -ENOMEM;
			break;
		}
		error = ext2_read_block(sb, physical, block_buf);
		if (error) {
			kfree(block_buf);
			break;
		}
		memcpy_fromfs(block_buf + offset, buf + total, chunk);
		error = ext2_write_block(sb, physical, block_buf);
		kfree(block_buf);
		if (error)
			break;

		total += chunk;
		start += chunk;
		file->f_pos = start;
	}

	if (start > inode->i_size) {
		inode->i_size = start;
		ext2_write32((uint8_t *)&raw->i_size, start);
	}
	if (total) {
		inode->i_mtime = inode->i_ctime = current_time();
		ext2_write32((uint8_t *)&raw->i_mtime, inode->i_mtime);
		ext2_write32((uint8_t *)&raw->i_ctime, inode->i_ctime);
		set_bit(I_Dirty, &inode->i_state);
	}

	up(&inode->i_sem);
	return total ? (int)total : error;
}

static int ext2_free_inode_block(struct inode *inode, uint32_t block)
{
	struct ext2_inode *raw = &EXT2_INODE(inode).raw;
	uint32_t sectors = EXT2_BLOCK_SIZE(inode->i_sb) / 512;
	int error;

	error = ext2_free_block(inode->i_sb, block);
	if (error)
		return error;
	if (inode->i_nr_block >= sectors)
		inode->i_nr_block -= sectors;
	else
		inode->i_nr_block = 0;
	ext2_write32((uint8_t *)&raw->i_blocks, inode->i_nr_block);
	return 0;
}

static int ext2_free_branch(struct inode *inode, uint32_t branch, int level,
			    uint32_t logical_start, uint32_t new_blocks,
			    int *empty)
{
	struct super_block *sb = inode->i_sb;
	uint32_t per = EXT2_ADDRS_PER_BLOCK(sb);
	uint32_t chunk = 1;
	uint32_t *ptrs;
	uint32_t i;
	int changed = 0;
	int all_zero = 1;
	int error;

	for (i = 0; i < (uint32_t)level; i++)
		chunk *= per;

	error = ext2_read_block(sb, branch, ptrs = kmalloc(EXT2_BLOCK_SIZE(sb)));
	if (error) {
		if (ptrs)
			kfree(ptrs);
		return error;
	}

	for (i = 0; i < per; i++) {
		uint32_t child = ext2_read32((const uint8_t *)&ptrs[i]);
		uint32_t child_logical = logical_start + i * chunk;

		if (!child)
			continue;

		if (level == 0) {
			if (child_logical >= new_blocks) {
				error = ext2_free_inode_block(inode, child);
				if (error)
					goto out;
				ext2_write32((uint8_t *)&ptrs[i], 0);
				changed = 1;
			} else {
				all_zero = 0;
			}
		} else if (child_logical >= new_blocks) {
			int child_empty = 0;

			error = ext2_free_branch(inode, child, level - 1,
						 child_logical, new_blocks,
						 &child_empty);
			if (error)
				goto out;
			if (!child_empty) {
				error = -EIO;
				goto out;
			}
			error = ext2_free_inode_block(inode, child);
			if (error)
				goto out;
			ext2_write32((uint8_t *)&ptrs[i], 0);
			changed = 1;
		} else if (child_logical + chunk > new_blocks) {
			int child_empty = 0;

			error = ext2_free_branch(inode, child, level - 1,
						 child_logical, new_blocks,
						 &child_empty);
			if (error)
				goto out;
			if (child_empty) {
				error = ext2_free_inode_block(inode, child);
				if (error)
					goto out;
				ext2_write32((uint8_t *)&ptrs[i], 0);
				changed = 1;
			} else {
				all_zero = 0;
			}
		} else {
			all_zero = 0;
		}
	}

	if (changed && !all_zero)
		error = ext2_write_block(sb, branch, ptrs);
	else
		error = 0;
	*empty = all_zero;
out:
	kfree(ptrs);
	return error;
}

int ext2_truncate(struct inode *inode, int size)
{
	struct super_block *sb = inode->i_sb;
	struct ext2_inode *raw = &EXT2_INODE(inode).raw;
	uint32_t block_size = EXT2_BLOCK_SIZE(sb);
	uint32_t per = EXT2_ADDRS_PER_BLOCK(sb);
	uint32_t single_start = EXT2_NDIR_BLOCKS;
	uint32_t double_start = single_start + per;
	uint32_t triple_start = double_start + per * per;
	uint32_t old_size = inode->i_size;
	uint32_t new_size = size;
	uint32_t old_blocks;
	uint32_t new_blocks;
	uint32_t i;
	int error;

	if (!S_ISREG(inode->i_mode) && !S_ISDIR(inode->i_mode) &&
	    !S_ISLNK(inode->i_mode))
		return -EINVAL;
	if (size < 0)
		return -EINVAL;

	old_blocks = (old_size + block_size - 1) / block_size;
	new_blocks = (new_size + block_size - 1) / block_size;

	if (new_blocks >= old_blocks) {
		inode->i_size = new_size;
		ext2_write32((uint8_t *)&raw->i_size, new_size);
		set_bit(I_Dirty, &inode->i_state);
		return 0;
	}

	for (i = new_blocks; i < EXT2_NDIR_BLOCKS && i < old_blocks; i++) {
		uint32_t block =
			ext2_read32((const uint8_t *)&raw->i_block[i]);

		if (!block)
			continue;
		error = ext2_free_inode_block(inode, block);
		if (error)
			return error;
		ext2_write32((uint8_t *)&raw->i_block[i], 0);
	}

	for (i = 0; i < 3; i++) {
		uint32_t slot;
		uint32_t start;
		int level;
		uint32_t branch;
		int empty = 0;

		if (i == 0) {
			slot = EXT2_IND_BLOCK;
			start = single_start;
			level = 0;
		} else if (i == 1) {
			slot = EXT2_DIND_BLOCK;
			start = double_start;
			level = 1;
		} else {
			slot = EXT2_TIND_BLOCK;
			start = triple_start;
			level = 2;
		}

		branch = ext2_read32((const uint8_t *)&raw->i_block[slot]);
		if (!branch)
			continue;
		error = ext2_free_branch(inode, branch, level, start,
					 new_blocks, &empty);
		if (error)
			return error;
		if (empty) {
			error = ext2_free_inode_block(inode, branch);
			if (error)
				return error;
			ext2_write32((uint8_t *)&raw->i_block[slot], 0);
		}
	}

	inode->i_size = new_size;
	inode->i_mtime = inode->i_ctime = current_time();
	ext2_write32((uint8_t *)&raw->i_size, new_size);
	ext2_write32((uint8_t *)&raw->i_mtime, inode->i_mtime);
	ext2_write32((uint8_t *)&raw->i_ctime, inode->i_ctime);
	set_bit(I_Dirty, &inode->i_state);
	return 0;
}

static int ext2_readlink(struct inode *inode, char *buf, int count)
{
	char *target;
	int ret;

	if (!S_ISLNK(inode->i_mode)) {
		iput(inode);
		return -EINVAL;
	}

	target = kmalloc(inode->i_size + 1);
	if (!target) {
		iput(inode);
		return -ENOMEM;
	}
	ret = ext2_read_symlink_target(inode, target, inode->i_size + 1);
	if (ret < 0) {
		kfree(target);
		iput(inode);
		return ret;
	}
	count = MIN(count, ret);
	memcpy_tofs(buf, target, count);
	kfree(target);
	iput(inode);
	return ret;
}

static int ext2_follow_link(struct inode *dir, struct inode *inode, int flags,
			    struct inode **res_inode)
{
	char *target;
	int error;

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

	target = kmalloc(inode->i_size + 1);
	if (!target) {
		iput(dir);
		iput(inode);
		return -ENOMEM;
	}
	error = ext2_read_symlink_target(inode, target, inode->i_size + 1);
	if (error < 0) {
		kfree(target);
		iput(dir);
		iput(inode);
		return error;
	}

	iput(inode);
	current->link_count++;
	error = open_namei(target, flags, res_inode, dir);
	current->link_count--;
	kfree(target);
	return error;
}

struct inode_operations ext2_file_iops = {
	.truncate = ext2_truncate,
};

struct inode_operations ext2_link_iops = {
	.readlink = ext2_readlink,
	.follow_link = ext2_follow_link,
};

struct file_operations ext2_file_fops = {
	.read = ext2_file_read,
	.write = ext2_file_write,
};
