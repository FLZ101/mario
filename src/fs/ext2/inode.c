#include <fs/fs.h>
#include <fs/ext2/ext2.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

extern uint64_t current_time(void);
extern struct file_operations blkdev_fops;
extern struct file_operations chrdev_fops;

static void ext2_set_ops(struct inode *inode)
{
	/*
	 * Match the generic inode mode to the operation tables used by the VFS.
	 * Device and FIFO inodes keep the generic fops and have no filesystem
	 * inode operations, just like their MarioFS counterparts.
	 */
	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &ext2_file_iops;
		inode->i_fop = &ext2_file_fops;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &ext2_dir_iops;
		inode->i_fop = &ext2_dir_fops;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &ext2_link_iops;
		inode->i_fop = NULL;
	} else if (S_ISBLK(inode->i_mode)) {
		inode->i_op = NULL;
		inode->i_fop = &blkdev_fops;
	} else if (S_ISCHR(inode->i_mode)) {
		inode->i_op = NULL;
		inode->i_fop = &chrdev_fops;
	} else if (S_ISFIFO(inode->i_mode)) {
		inode->i_op = NULL;
		inode->i_fop = &fifo_fops;
	} else {
		inode->i_op = NULL;
		inode->i_fop = NULL;
	}
}

int ext2_read_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ext2_group_desc *desc;
	struct ext2_inode *raw;
	uint32_t ino = inode->i_ino;
	uint32_t group;
	uint32_t index;
	uint32_t byte_offset;
	uint32_t block;
	uint32_t offset;
	uint32_t copy;
	uint8_t *buf;
	int error;

	if (!sb || ino == 0)
		return -EINVAL;

	/* ext2 inode numbers are 1-based; convert to group and in-group index. */
	group = (ino - 1) / EXT2_INODES_PER_GROUP(sb);
	index = (ino - 1) % EXT2_INODES_PER_GROUP(sb);
	if (group >= EXT2_NR_GROUPS(sb))
		return -EINVAL;

	desc = &EXT2_GROUP_DESC(sb)[group];
	byte_offset = index * EXT2_SB(sb).inode_size;
	block = desc->bg_inode_table + byte_offset / EXT2_BLOCK_SIZE(sb);
	offset = byte_offset % EXT2_BLOCK_SIZE(sb);

	buf = kmalloc(EXT2_BLOCK_SIZE(sb));
	if (!buf)
		return -ENOMEM;
	error = ext2_read_block(sb, block, buf);
	if (error) {
		kfree(buf);
		return error;
	}

	raw = &EXT2_INODE(inode).raw;
	copy = sizeof(*raw);
	/*
	 * The inode table entry starts at @offset in the loaded table block.
	 * Copy only the bytes present in this block and the fixed prefix this
	 * driver interprets.  The first 128 bytes never span a block boundary
	 * for the supported 1024/2048/4096 byte block sizes.
	 */
	if (copy > EXT2_BLOCK_SIZE(sb) - offset)
		copy = EXT2_BLOCK_SIZE(sb) - offset;
	if (copy > EXT2_SB(sb).inode_size)
		copy = EXT2_SB(sb).inode_size;
	/* Preserve only the fixed 128-byte portion that this driver knows
	 * about; later fields are intentionally not interpreted. */
	memset(raw, 0, sizeof(*raw));
	memcpy(raw, buf + offset, copy);
	kfree(buf);

	inode->i_mode = ext2_read16((const uint8_t *)&raw->i_mode);
	inode->i_size = ext2_read32((const uint8_t *)&raw->i_size);
	inode->i_nr_block = ext2_read32((const uint8_t *)&raw->i_blocks);
	inode->i_block_size = EXT2_BLOCK_SIZE(sb);
	inode->i_nlink = ext2_read16((const uint8_t *)&raw->i_links_count);
	inode->i_atime = ext2_read32((const uint8_t *)&raw->i_atime);
	inode->i_mtime = ext2_read32((const uint8_t *)&raw->i_mtime);
	inode->i_ctime = ext2_read32((const uint8_t *)&raw->i_ctime);

	/* Traditional ext2 stores device numbers in i_block[0]. */
	if (S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode))
		inode->i_rdev = ext2_read32((const uint8_t *)&raw->i_block[0]);
	else
		inode->i_rdev = 0;

	ext2_set_ops(inode);
	return 0;
}

int ext2_write_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ext2_group_desc *desc;
	struct ext2_inode *raw = &EXT2_INODE(inode).raw;
	uint32_t ino = inode->i_ino;
	uint32_t group;
	uint32_t index;
	uint32_t byte_offset;
	uint32_t block;
	uint32_t offset;
	uint8_t *buf;
	int error;

	if (EXT2_SB(inode->i_sb).readonly)
		return -EROFS;
	if (!sb || ino == 0)
		return -EINVAL;

	group = (ino - 1) / EXT2_INODES_PER_GROUP(sb);
	index = (ino - 1) % EXT2_INODES_PER_GROUP(sb);
	if (group >= EXT2_NR_GROUPS(sb))
		return -EINVAL;

	ext2_write16((uint8_t *)&raw->i_mode, inode->i_mode);
	ext2_write32((uint8_t *)&raw->i_size, inode->i_size);
	ext2_write32((uint8_t *)&raw->i_atime, inode->i_atime);
	ext2_write32((uint8_t *)&raw->i_ctime, inode->i_ctime);
	ext2_write32((uint8_t *)&raw->i_mtime, inode->i_mtime);
	ext2_write16((uint8_t *)&raw->i_links_count, inode->i_nlink);
	ext2_write32((uint8_t *)&raw->i_blocks, inode->i_nr_block);

	desc = &EXT2_GROUP_DESC(sb)[group];
	byte_offset = index * EXT2_SB(sb).inode_size;
	block = ext2_read32((const uint8_t *)&desc->bg_inode_table) +
		byte_offset / EXT2_BLOCK_SIZE(sb);
	offset = byte_offset % EXT2_BLOCK_SIZE(sb);

	buf = kmalloc(EXT2_BLOCK_SIZE(sb));
	if (!buf)
		return -ENOMEM;
	error = ext2_read_block(sb, block, buf);
	if (error) {
		kfree(buf);
		return error;
	}
	memcpy(buf + offset, raw, sizeof(*raw));
	error = ext2_write_block(sb, block, buf);
	kfree(buf);
	return error;
}

void ext2_clear_inode(struct inode *inode)
{
	/* No separately allocated per-inode state yet. */
}
