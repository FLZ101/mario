#include <fs/fs.h>
#include <fs/ext2/ext2.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

/*
 * ext2 logical block I/O.  The kernel buffer cache is fixed at 512 bytes, so
 * ext2 uses the raw block-device interface directly with a sector multiplier.
 */
int ext2_read_block(struct super_block *sb, uint32_t block, void *buf)
{
	uint32_t sec_per_block;

	if (!sb || !buf || !EXT2_BLOCK_SIZE(sb))
		return -EINVAL;
	if (block >= EXT2_SB(sb).blocks_count)
		return -EINVAL;

	/* ext2 block numbers are physical device blocks, so 1 KiB block 0 maps
	 * to sector 0 and block 1 maps to the sector containing offset 1024. */
	sec_per_block = EXT2_BLOCK_SIZE(sb) / 512;
	return blkdev_read(sb->s_dev, block * sec_per_block,
			   sec_per_block, buf);
}

int ext2_write_block(struct super_block *sb, uint32_t block, const void *buf)
{
	uint32_t sec_per_block;
	int error;

	if (!sb || !buf || !EXT2_BLOCK_SIZE(sb))
		return -EINVAL;
	if (block >= EXT2_SB(sb).blocks_count)
		return -EINVAL;

	sec_per_block = EXT2_BLOCK_SIZE(sb) / 512;
	error = blkdev_write(sb->s_dev, block * sec_per_block,
			     sec_per_block, (char *)buf);
	if (!error)
		set_bit(SB_Dirty, &sb->s_state);
	return error;
}
