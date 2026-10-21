#include <fs/fs.h>
#include <fs/ext2/ext2.h>

#include <mm/kmalloc.h>

#include <lib/bitops.h>
#include <lib/string.h>

static int ext2_inode_location(struct super_block *sb, uint32_t ino,
			       uint32_t *block, uint32_t *offset)
{
	struct ext2_group_desc *desc;
	uint32_t group;
	uint32_t index;
	uint32_t byte_offset;

	if (!sb || ino == 0 || ino > EXT2_SB(sb).inodes_count)
		return -EINVAL;

	group = (ino - 1) / EXT2_INODES_PER_GROUP(sb);
	index = (ino - 1) % EXT2_INODES_PER_GROUP(sb);
	if (group >= EXT2_NR_GROUPS(sb))
		return -EINVAL;

	desc = &EXT2_GROUP_DESC(sb)[group];
	byte_offset = index * EXT2_SB(sb).inode_size;
	*block = ext2_read32((const uint8_t *)&desc->bg_inode_table) +
		 byte_offset / EXT2_BLOCK_SIZE(sb);
	*offset = byte_offset % EXT2_BLOCK_SIZE(sb);
	return 0;
}

static int ext2_zero_inode(struct super_block *sb, uint32_t ino)
{
	uint32_t block;
	uint32_t offset;
	uint32_t left = EXT2_SB(sb).inode_size;
	uint8_t *buf;
	int error;

	error = ext2_inode_location(sb, ino, &block, &offset);
	if (error)
		return error;

	buf = kmalloc(EXT2_BLOCK_SIZE(sb));
	if (!buf)
		return -ENOMEM;
	while (left) {
		uint32_t chunk = MIN(left, EXT2_BLOCK_SIZE(sb) - offset);

		error = ext2_read_block(sb, block, buf);
		if (error)
			goto out;
		memset(buf + offset, 0, chunk);
		error = ext2_write_block(sb, block, buf);
		if (error)
			goto out;
		left -= chunk;
		offset = 0;
		block++;
	}
out:
	kfree(buf);
	return error;
}

static int ext2_set_inode_mode(struct super_block *sb, uint32_t ino,
			       uint16_t mode)
{
	uint32_t block;
	uint32_t offset;
	uint8_t *buf;
	int error;

	error = ext2_inode_location(sb, ino, &block, &offset);
	if (error)
		return error;

	buf = kmalloc(EXT2_BLOCK_SIZE(sb));
	if (!buf)
		return -ENOMEM;
	error = ext2_read_block(sb, block, buf);
	if (!error) {
		ext2_write16(buf + offset, mode);
		error = ext2_write_block(sb, block, buf);
	}
	kfree(buf);
	return error;
}

int ext2_alloc_inode(struct super_block *sb, uint16_t mode, uint32_t *ino)
{
	uint32_t group;
	uint8_t *bitmap;
	int error;

	if (!sb || !ino)
		return -EINVAL;
	if (EXT2_SB(sb).readonly)
		return -EROFS;
	if (!EXT2_SB(sb).free_inodes_count)
		return -ENOSPC;

	bitmap = kmalloc(EXT2_BLOCK_SIZE(sb));
	if (!bitmap)
		return -ENOMEM;

	down(&sb->s_sem);
	for (group = 0; group < EXT2_NR_GROUPS(sb); group++) {
		struct ext2_group_desc *desc = &EXT2_GROUP_DESC(sb)[group];
		uint32_t bitmap_block;
		int bit;

		if (!ext2_read16((const uint8_t *)&desc->bg_free_inodes_count))
			continue;
		bitmap_block =
			ext2_read32((const uint8_t *)&desc->bg_inode_bitmap);
		error = ext2_read_block(sb, bitmap_block, bitmap);
		if (error)
			goto out;
		bit = find_first_zero_bit(bitmap, EXT2_INODES_PER_GROUP(sb));
		if (bit < 0)
			continue;

		set_bit(bit, bitmap);
		error = ext2_write_block(sb, bitmap_block, bitmap);
		if (error) {
			clear_bit(bit, bitmap);
			ext2_write_block(sb, bitmap_block, bitmap);
			goto out;
		}

		*ino = group * EXT2_INODES_PER_GROUP(sb) + bit + 1;
		if (*ino > EXT2_SB(sb).inodes_count) {
			clear_bit(bit, bitmap);
			ext2_write_block(sb, bitmap_block, bitmap);
			error = -ENOSPC;
			goto out;
		}

		error = ext2_zero_inode(sb, *ino);
		if (error) {
			clear_bit(bit, bitmap);
			ext2_write_block(sb, bitmap_block, bitmap);
			goto out;
		}
		error = ext2_set_inode_mode(sb, *ino, mode);
		if (error) {
			clear_bit(bit, bitmap);
			ext2_write_block(sb, bitmap_block, bitmap);
			goto out;
		}

		ext2_write16((uint8_t *)&desc->bg_free_inodes_count,
			     ext2_read16((const uint8_t *)&desc->bg_free_inodes_count) - 1);
		EXT2_SB(sb).free_inodes_count--;
		set_bit(SB_Dirty, &sb->s_state);
		error = ext2_write_group_desc(sb, group);
		if (error)
			goto out;

		up(&sb->s_sem);
		kfree(bitmap);
		return 0;
	}

	error = -ENOSPC;
out:
	up(&sb->s_sem);
	kfree(bitmap);
	return error;
}

int ext2_free_inode(struct super_block *sb, uint32_t ino)
{
	struct ext2_group_desc *desc;
	uint32_t group;
	uint32_t bit;
	uint32_t bitmap_block;
	uint8_t *bitmap;
	int error;

	if (!sb || EXT2_SB(sb).readonly || ino == 0 ||
	    ino > EXT2_SB(sb).inodes_count)
		return -EINVAL;

	group = (ino - 1) / EXT2_INODES_PER_GROUP(sb);
	if (group >= EXT2_NR_GROUPS(sb))
		return -EINVAL;
	bit = (ino - 1) % EXT2_INODES_PER_GROUP(sb);

	bitmap = kmalloc(EXT2_BLOCK_SIZE(sb));
	if (!bitmap)
		return -ENOMEM;

	down(&sb->s_sem);
	desc = &EXT2_GROUP_DESC(sb)[group];
	bitmap_block = ext2_read32((const uint8_t *)&desc->bg_inode_bitmap);
	error = ext2_read_block(sb, bitmap_block, bitmap);
	if (error)
		goto out;

	if (!test_and_clear_bit(bit, bitmap)) {
		error = -EIO;
		goto out;
	}
	error = ext2_write_block(sb, bitmap_block, bitmap);
	if (error) {
		set_bit(bit, bitmap);
		ext2_write_block(sb, bitmap_block, bitmap);
		goto out;
	}

	ext2_write16((uint8_t *)&desc->bg_free_inodes_count,
		     ext2_read16((const uint8_t *)&desc->bg_free_inodes_count) + 1);
	EXT2_SB(sb).free_inodes_count++;
	set_bit(SB_Dirty, &sb->s_state);
	error = ext2_write_group_desc(sb, group);
out:
	up(&sb->s_sem);
	kfree(bitmap);
	return error;
}
