#include <fs/fs.h>
#include <fs/ext2/ext2.h>

#include <mm/kmalloc.h>

#include <lib/bitops.h>
#include <lib/string.h>

static uint32_t ext2_group_start(struct super_block *sb, uint32_t group)
{
	return EXT2_SB(sb).first_data_block +
	       group * EXT2_BLOCKS_PER_GROUP(sb);
}

static uint32_t ext2_group_blocks(struct super_block *sb, uint32_t group)
{
	uint32_t start = ext2_group_start(sb, group);
	uint32_t remaining;

	if (start >= EXT2_SB(sb).blocks_count)
		return 0;
	remaining = EXT2_SB(sb).blocks_count - start;
	return MIN(remaining, EXT2_BLOCKS_PER_GROUP(sb));
}

int ext2_write_group_desc(struct super_block *sb, uint32_t group)
{
	struct ext2_group_desc *desc;
	uint32_t desc_block;
	uint32_t index;
	uint8_t *buf;
	int error;

	if (!sb || group >= EXT2_NR_GROUPS(sb))
		return -EINVAL;

	index = group % EXT2_SB(sb).desc_per_block;
	desc_block = EXT2_SB(sb).first_data_block + 1 +
		     group / EXT2_SB(sb).desc_per_block;
	buf = kmalloc(EXT2_BLOCK_SIZE(sb));
	if (!buf)
		return -ENOMEM;
	error = ext2_read_block(sb, desc_block, buf);
	if (error) {
		kfree(buf);
		return error;
	}

	desc = (struct ext2_group_desc *)(buf +
					  index * sizeof(*desc));
	memcpy(desc, &EXT2_GROUP_DESC(sb)[group], sizeof(*desc));
	error = ext2_write_block(sb, desc_block, buf);
	kfree(buf);
	return error;
}

int ext2_alloc_block(struct super_block *sb, uint32_t goal, uint32_t *block)
{
	uint32_t first_group = 0;
	uint32_t i;
	uint8_t *bitmap;
	int error;

	if (!sb || !block)
		return -EINVAL;
	if (EXT2_SB(sb).readonly)
		return -EROFS;
	if (!EXT2_SB(sb).free_blocks_count)
		return -ENOSPC;

	/*
	 * The goal is advisory.  Prefer the group containing it, then fall
	 * back to the remaining groups in order.  Small test filesystems have
	 * only a few groups, so the scan remains short.
	 */
	if (goal >= EXT2_SB(sb).first_data_block)
		first_group = (goal - EXT2_SB(sb).first_data_block) /
			      EXT2_BLOCKS_PER_GROUP(sb);
	if (first_group >= EXT2_NR_GROUPS(sb))
		first_group = 0;

	bitmap = kmalloc(EXT2_BLOCK_SIZE(sb));
	if (!bitmap)
		return -ENOMEM;

	down(&sb->s_sem);
	for (i = 0; i < EXT2_NR_GROUPS(sb); i++) {
		struct ext2_group_desc *desc;
		uint32_t group = (first_group + i) % EXT2_NR_GROUPS(sb);
		uint32_t start = ext2_group_start(sb, group);
		uint32_t count = ext2_group_blocks(sb, group);
		uint32_t bitmap_block;
		int bit;

		if (!count)
			continue;
		desc = &EXT2_GROUP_DESC(sb)[group];
		if (!ext2_read16((const uint8_t *)&desc->bg_free_blocks_count))
			continue;

		bitmap_block =
			ext2_read32((const uint8_t *)&desc->bg_block_bitmap);
		error = ext2_read_block(sb, bitmap_block, bitmap);
		if (error)
			goto out;
		bit = find_first_zero_bit(bitmap, count);
		if (bit < 0)
			continue;

		set_bit(bit, bitmap);
		error = ext2_write_block(sb, bitmap_block, bitmap);
		if (error) {
			clear_bit(bit, bitmap);
			ext2_write_block(sb, bitmap_block, bitmap);
			goto out;
		}

		ext2_write16((uint8_t *)&desc->bg_free_blocks_count,
			     ext2_read16((const uint8_t *)&desc->bg_free_blocks_count) - 1);
		EXT2_SB(sb).free_blocks_count--;
		set_bit(SB_Dirty, &sb->s_state);
		error = ext2_write_group_desc(sb, group);
		if (error)
			goto out;

		*block = start + bit;
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

int ext2_free_block(struct super_block *sb, uint32_t block)
{
	struct ext2_group_desc *desc;
	uint32_t group;
	uint32_t start;
	uint32_t bit;
	uint32_t bitmap_block;
	uint8_t *bitmap;
	int error;

	if (!sb || EXT2_SB(sb).readonly)
		return -EROFS;
	if (block < EXT2_SB(sb).first_data_block ||
	    block >= EXT2_SB(sb).blocks_count)
		return -EINVAL;

	group = (block - EXT2_SB(sb).first_data_block) /
		EXT2_BLOCKS_PER_GROUP(sb);
	if (group >= EXT2_NR_GROUPS(sb))
		return -EINVAL;
	start = ext2_group_start(sb, group);
	bit = block - start;

	bitmap = kmalloc(EXT2_BLOCK_SIZE(sb));
	if (!bitmap)
		return -ENOMEM;

	down(&sb->s_sem);
	desc = &EXT2_GROUP_DESC(sb)[group];
	bitmap_block = ext2_read32((const uint8_t *)&desc->bg_block_bitmap);
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

	ext2_write16((uint8_t *)&desc->bg_free_blocks_count,
		     ext2_read16((const uint8_t *)&desc->bg_free_blocks_count) + 1);
	EXT2_SB(sb).free_blocks_count++;
	set_bit(SB_Dirty, &sb->s_state);
	error = ext2_write_group_desc(sb, group);
out:
	up(&sb->s_sem);
	kfree(bitmap);
	return error;
}
