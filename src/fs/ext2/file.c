#include <fs/fs.h>
#include <fs/ext2/ext2.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

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
};

struct inode_operations ext2_link_iops = {
	.readlink = ext2_readlink,
	.follow_link = ext2_follow_link,
};

struct file_operations ext2_file_fops = {
	.read = ext2_file_read,
};
