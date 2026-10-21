#include <fs/fs.h>
#include <fs/ext2/ext2.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

#define EXT2_ADDRS_PER_BLOCK(sb) (EXT2_BLOCK_SIZE(sb) / sizeof(uint32_t))

static uint32_t ext2_single_start(struct super_block *sb)
{
	return EXT2_NDIR_BLOCKS;
}

static uint32_t ext2_double_start(struct super_block *sb)
{
	return ext2_single_start(sb) + EXT2_ADDRS_PER_BLOCK(sb);
}

static uint32_t ext2_triple_start(struct super_block *sb)
{
	uint32_t per = EXT2_ADDRS_PER_BLOCK(sb);

	return ext2_double_start(sb) + per * per;
}

static int ext2_load_ptrs(struct super_block *sb, uint32_t block, uint32_t **ptrs)
{
	*ptrs = kmalloc(EXT2_BLOCK_SIZE(sb));
	if (!*ptrs)
		return -ENOMEM;
	return ext2_read_block(sb, block, *ptrs);
}

static int ext2_store_ptrs(struct super_block *sb, uint32_t block, uint32_t *ptrs)
{
	return ext2_write_block(sb, block, ptrs);
}

/*
 * Allocate and zero a block, then account for it in the inode's on-disk
 * i_blocks count.  Metadata and data blocks are both accounted in 512-byte
 * sectors, matching ext2's i_blocks convention.
 */
static int ext2_new_block(struct inode *inode, uint32_t *result)
{
	struct super_block *sb = inode->i_sb;
	struct ext2_inode *raw = &EXT2_INODE(inode).raw;
	uint32_t block;
	uint32_t sectors;
	uint8_t *buf;
	int error;

	error = ext2_alloc_block(sb, 0, &block);
	if (error)
		return error;

	buf = kmalloc(EXT2_BLOCK_SIZE(sb));
	if (!buf) {
		ext2_free_block(sb, block);
		return -ENOMEM;
	}
	memset(buf, 0, EXT2_BLOCK_SIZE(sb));
	error = ext2_write_block(sb, block, buf);
	kfree(buf);
	if (error) {
		ext2_free_block(sb, block);
		return error;
	}

	sectors = EXT2_BLOCK_SIZE(sb) / 512;
	inode->i_nr_block += sectors;
	ext2_write32((uint8_t *)&raw->i_blocks,
		     ext2_read32((const uint8_t *)&raw->i_blocks) + sectors);
	set_bit(I_Dirty, &inode->i_state);
	*result = block;
	return 0;
}

static int ext2_ensure_indirect(struct inode *inode, uint8_t *slot,
				uint32_t *result)
{
	uint32_t block = ext2_read32(slot);
	int error;

	if (block) {
		*result = block;
		return 0;
	}
	error = ext2_new_block(inode, &block);
	if (error)
		return error;
	ext2_write32(slot, block);
	*result = block;
	return 0;
}

static int ext2_ensure_data(struct inode *inode, uint32_t *ptrs, uint32_t index,
			    uint32_t *result)
{
	uint32_t block = ext2_read32((const uint8_t *)&ptrs[index]);
	int error;

	if (block) {
		*result = block;
		return 0;
	}
	error = ext2_new_block(inode, &block);
	if (error)
		return error;
	ext2_write32((uint8_t *)&ptrs[index], block);
	*result = block;
	return 0;
}

/*
 * Resolve a logical file-block number to a physical ext2 block.  With
 * `create` clear this is the read-only path and zero denotes a hole.  With
 * `create` set, missing indirect and data blocks are allocated on demand.
 */
uint32_t ext2_bmap(struct inode *inode, uint32_t block, int create)
{
	struct super_block *sb = inode->i_sb;
	struct ext2_inode *raw = &EXT2_INODE(inode).raw;
	uint32_t per = EXT2_ADDRS_PER_BLOCK(sb);
	uint32_t single_start = ext2_single_start(sb);
	uint32_t double_start = ext2_double_start(sb);
	uint32_t triple_start = ext2_triple_start(sb);
	uint32_t indirect;
	uint32_t index;
	uint32_t *ptrs;
	uint32_t result;
	int error;

	if (block < EXT2_NDIR_BLOCKS) {
		result = ext2_read32((const uint8_t *)&raw->i_block[block]);
		if (result)
			return result;
		if (!create)
			return 0;
		error = ext2_new_block(inode, &result);
		if (error)
			return 0;
		ext2_write32((uint8_t *)&raw->i_block[block], result);
		return result;
	}

	if (block < double_start) {
		index = block - single_start;
		error = ext2_ensure_indirect(
			inode, (uint8_t *)&raw->i_block[EXT2_IND_BLOCK], &indirect);
		if (error)
			return 0;
		error = ext2_load_ptrs(sb, indirect, &ptrs);
		if (error)
			return 0;
		result = ext2_read32((const uint8_t *)&ptrs[index]);
		if (!result && create) {
			error = ext2_ensure_data(inode, ptrs, index, &result);
			if (!error)
				error = ext2_store_ptrs(sb, indirect, ptrs);
		}
		kfree(ptrs);
		return error ? 0 : result;
	}

	if (block < triple_start) {
		uint32_t dind;
		uint32_t ind_block;
		uint32_t first;
		uint32_t second;

		index = block - double_start;
		first = index / per;
		second = index % per;

		error = ext2_ensure_indirect(
			inode, (uint8_t *)&raw->i_block[EXT2_DIND_BLOCK], &dind);
		if (error)
			return 0;
		error = ext2_load_ptrs(sb, dind, &ptrs);
		if (error)
			return 0;
		error = ext2_ensure_indirect(inode, (uint8_t *)&ptrs[first], &ind_block);
		if (!error) {
			error = ext2_store_ptrs(sb, dind, ptrs);
			kfree(ptrs);
			if (error)
				return 0;
			error = ext2_load_ptrs(sb, ind_block, &ptrs);
			if (error)
				return 0;
			result = ext2_read32((const uint8_t *)&ptrs[second]);
			if (!result && create) {
				error = ext2_ensure_data(inode, ptrs, second, &result);
				if (!error)
					error = ext2_store_ptrs(sb, ind_block, ptrs);
			}
			kfree(ptrs);
			return error ? 0 : result;
		}
		kfree(ptrs);
		return 0;
	}

	{
		uint32_t tind;
		uint32_t dind_block;
		uint32_t ind_block;
		uint32_t first;
		uint32_t second;
		uint32_t third;

		index = block - triple_start;
		first = index / (per * per);
		second = (index / per) % per;
		third = index % per;

		error = ext2_ensure_indirect(
			inode, (uint8_t *)&raw->i_block[EXT2_TIND_BLOCK], &tind);
		if (error)
			return 0;
		error = ext2_load_ptrs(sb, tind, &ptrs);
		if (error)
			return 0;
		error = ext2_ensure_indirect(inode, (uint8_t *)&ptrs[first], &dind_block);
		if (error) {
			kfree(ptrs);
			return 0;
		}
		error = ext2_store_ptrs(sb, tind, ptrs);
		kfree(ptrs);
		if (error)
			return 0;

		error = ext2_load_ptrs(sb, dind_block, &ptrs);
		if (error)
			return 0;
		error = ext2_ensure_indirect(inode, (uint8_t *)&ptrs[second], &ind_block);
		if (error) {
			kfree(ptrs);
			return 0;
		}
		error = ext2_store_ptrs(sb, dind_block, ptrs);
		kfree(ptrs);
		if (error)
			return 0;

		error = ext2_load_ptrs(sb, ind_block, &ptrs);
		if (error)
			return 0;
		result = ext2_read32((const uint8_t *)&ptrs[third]);
		if (!result && create) {
			error = ext2_ensure_data(inode, ptrs, third, &result);
			if (!error)
				error = ext2_store_ptrs(sb, ind_block, ptrs);
		}
		kfree(ptrs);
		return error ? 0 : result;
	}
}
