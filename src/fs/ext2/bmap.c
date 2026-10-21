#include <fs/fs.h>
#include <fs/ext2/ext2.h>

#include <mm/kmalloc.h>

/*
 * Resolve a logical file-block number to its physical ext2 block.  A zero
 * result means a hole for reads.  Allocation (`create != 0`) is not supported
 * while ext2 is mounted read-only.
 *
 * ext2 has three levels of indirect blocks.  The thresholds below are the
 * logical block numbers where each level starts: direct blocks occupy
 * EXT2_NDIR_BLOCKS slots in the inode, one singly-indirect block adds one
 * block worth of addresses, one doubly-indirect block adds that many singly
 * indirect blocks, and so on.  Each level is resolved one buffer at a time so
 * the caller never has to hold an entire indirect tree in memory.
 */
uint32_t ext2_bmap(struct inode *inode, uint32_t block, int create)
{
	struct super_block *sb = inode->i_sb;
	struct ext2_inode *raw = &EXT2_INODE(inode).raw;
	uint32_t addr_per_block;
	uint32_t single_start;
	uint32_t double_start;
	uint32_t triple_start;
	uint32_t indirect;
	uint32_t index;
	uint32_t *addr;
	int error;

	if (create)
		return 0;

	addr_per_block = EXT2_BLOCK_SIZE(sb) / sizeof(uint32_t);
	single_start = EXT2_NDIR_BLOCKS;
	double_start = single_start + addr_per_block;
	triple_start = double_start + addr_per_block * addr_per_block;

	if (block < EXT2_NDIR_BLOCKS)
		return ext2_read32((const uint8_t *)&raw->i_block[block]);

	/* First indirect level: one address block pointed to by i_block[12]. */
	if (block < double_start) {
		indirect = ext2_read32((const uint8_t *)&raw->i_block[EXT2_IND_BLOCK]);
		if (!indirect)
			return 0;
		addr = kmalloc(EXT2_BLOCK_SIZE(sb));
		if (!addr)
			return 0;
		error = ext2_read_block(sb, indirect, addr);
		if (error) {
			kfree(addr);
			return 0;
		}
		index = block - single_start;
		indirect = ext2_read32((const uint8_t *)&addr[index]);
		kfree(addr);
		return indirect;
	}

	/* Second indirect level: pointer block -> pointer block -> data block. */
	if (block < triple_start) {
		uint32_t first;
		uint32_t second;

		indirect = ext2_read32((const uint8_t *)&raw->i_block[EXT2_DIND_BLOCK]);
		if (!indirect)
			return 0;
		addr = kmalloc(EXT2_BLOCK_SIZE(sb));
		if (!addr)
			return 0;
		error = ext2_read_block(sb, indirect, addr);
		if (error) {
			kfree(addr);
			return 0;
		}
		index = block - double_start;
		first = index / addr_per_block;
		second = index % addr_per_block;
		indirect = ext2_read32((const uint8_t *)&addr[first]);
		kfree(addr);
		if (!indirect)
			return 0;

		addr = kmalloc(EXT2_BLOCK_SIZE(sb));
		if (!addr)
			return 0;
		error = ext2_read_block(sb, indirect, addr);
		if (error) {
			kfree(addr);
			return 0;
		}
		indirect = ext2_read32((const uint8_t *)&addr[second]);
		kfree(addr);
		return indirect;
	}

	/* Third indirect level: pointer block -> pointer block -> pointer block
	 * -> data block.  These are rarely used by the small read-only test
	 * images, but they are cheap to support and keep regular-file reads
	 * complete. */
	{
		uint32_t first;
		uint32_t second;
		uint32_t third;

		index = block - triple_start;
		indirect = ext2_read32((const uint8_t *)&raw->i_block[EXT2_TIND_BLOCK]);
		if (!indirect)
			return 0;
		addr = kmalloc(EXT2_BLOCK_SIZE(sb));
		if (!addr)
			return 0;
		error = ext2_read_block(sb, indirect, addr);
		if (error) {
			kfree(addr);
			return 0;
		}
		first = index / (addr_per_block * addr_per_block);
		second = (index / addr_per_block) % addr_per_block;
		third = index % addr_per_block;
		indirect = ext2_read32((const uint8_t *)&addr[first]);
		kfree(addr);
		if (!indirect)
			return 0;

		addr = kmalloc(EXT2_BLOCK_SIZE(sb));
		if (!addr)
			return 0;
		error = ext2_read_block(sb, indirect, addr);
		if (error) {
			kfree(addr);
			return 0;
		}
		indirect = ext2_read32((const uint8_t *)&addr[second]);
		kfree(addr);
		if (!indirect)
			return 0;

		addr = kmalloc(EXT2_BLOCK_SIZE(sb));
		if (!addr)
			return 0;
		error = ext2_read_block(sb, indirect, addr);
		if (error) {
			kfree(addr);
			return 0;
		}
		indirect = ext2_read32((const uint8_t *)&addr[third]);
		kfree(addr);
		return indirect;
	}
}
