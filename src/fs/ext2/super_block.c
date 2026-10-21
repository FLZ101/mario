#include <fs/fs.h>
#include <fs/ext2/ext2.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

extern uint64_t current_time(void);

/* The read-only first implementation registers these operations immediately
 * after probing the superblock. */
static struct super_operations ext2_sops = {
	.read_inode = ext2_read_inode,
	.write_inode = ext2_write_inode,
	.write_super = ext2_write_super,
	.clear_inode = ext2_clear_inode,
};

static int ext2_load_group_descriptors(struct super_block *sb,
				       const struct ext2_super_block *esb)
{
	uint32_t block;
	uint32_t i;
	uint32_t descs_per_block;
	uint32_t nr_groups;
	uint32_t desc_table_block;
	uint32_t table_bytes;
	uint8_t *buf;
	struct ext2_group_desc *table;
	int error;

	/*
	 * Derive the filesystem block size first.  The on-disk `s_log_block_size`
	 * is the base-2 log of (block_size / 1024), so supported values produce
	 * 1024, 2048, or 4096 byte blocks.  first_data_block is 1 only for the
	 * smallest block size, where the boot block occupies block 0.
	 */
	EXT2_SB(sb).block_size = 1024U << esb->s_log_block_size;
	EXT2_SB(sb).first_data_block = EXT2_SB(sb).block_size == 1024 ? 1 : 0;
	EXT2_SB(sb).blocks_per_group = esb->s_blocks_per_group;
	EXT2_SB(sb).inodes_per_group = esb->s_inodes_per_group;
	if (!EXT2_SB(sb).blocks_per_group || !EXT2_SB(sb).inodes_per_group)
		return -EINVAL;

	/* Group descriptors follow the superblock contiguously. */
	nr_groups = (esb->s_blocks_count - EXT2_SB(sb).first_data_block +
		     EXT2_SB(sb).blocks_per_group - 1) /
		    EXT2_SB(sb).blocks_per_group;
	if (!nr_groups || nr_groups > 1024 * 1024)
		return -EINVAL;

	table_bytes = nr_groups * sizeof(struct ext2_group_desc);
	table = kmalloc(table_bytes);
	if (!table)
		return -ENOMEM;

	descs_per_block = EXT2_SB(sb).block_size / sizeof(*table);
	/* Descriptors start in the block immediately after the superblock. */
	desc_table_block = EXT2_SB(sb).first_data_block + 1;
	buf = kmalloc(EXT2_SB(sb).block_size);
	if (!buf) {
		kfree(table);
		return -ENOMEM;
	}

	for (i = 0; i < nr_groups; i += descs_per_block) {
		uint32_t copy = descs_per_block;

		if (copy > nr_groups - i)
			copy = nr_groups - i;
		block = desc_table_block + i / descs_per_block;
		error = ext2_read_block(sb, block, buf);
		if (error) {
			kfree(buf);
			kfree(table);
			return error;
		}
		memcpy(table + i, buf, copy * sizeof(*table));
	}

	kfree(buf);
	EXT2_SB(sb).nr_groups = nr_groups;
	EXT2_SB(sb).desc_per_block = descs_per_block;
	EXT2_SB(sb).group_desc = table;
	return 0;
}

static int ext2_check_features(struct ext2_super_block *esb)
{
	uint32_t incompat;
	uint32_t ro_compat;

	/* Reject ext3/ext4 images before interpreting their different inode or
	 * descriptor layouts. */
	incompat = esb->s_feature_incompat;
	if (incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE)
		return -EINVAL;

	ro_compat = esb->s_feature_ro_compat;
	if (ro_compat & ~(EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER |
			  EXT2_FEATURE_RO_COMPAT_LARGE_FILE))
		return -EINVAL;

	return 0;
}

struct super_block *ext2_read_super(struct super_block *sb)
{
	struct ext2_super_block *esb;
	uint8_t *buf;
	uint32_t block_size;
	int error;

	/*
	 * Keep probe failures non-fatal.  mount_root() tries every registered
	 * filesystem on ROOT_DEV, so an ext2 driver must return NULL for
	 * ramdisks (and vice versa) instead of hanging the boot.
	 */
	if (!sb || !sb->s_dev)
		return NULL;

	/*
	 * Read the fixed 1024-byte superblock slot.  On every supported block
	 * size it occupies bytes 1024..2047 of the device, which are sectors
	 * 2 and 3 when the sector size is 512.
	 */
	buf = kmalloc(1024);
	if (!buf)
		return NULL;
	if (blkdev_read(sb->s_dev, 2, 2, (char *)buf)) {
		kfree(buf);
		return NULL;
	}

	esb = (struct ext2_super_block *)buf;
	if (esb->s_magic != EXT2_SUPER_MAGIC ||
	    esb->s_log_block_size > 2 ||
	    esb->s_inode_size < EXT2_GOOD_OLD_INODE_SIZE ||
	    ext2_check_features(esb)) {
		kfree(buf);
		return NULL;
	}

	block_size = 1024U << esb->s_log_block_size;
	EXT2_SB(sb).block_size = block_size;
	EXT2_SB(sb).blocks_count = esb->s_blocks_count;
	EXT2_SB(sb).inodes_count = esb->s_inodes_count;
	EXT2_SB(sb).free_blocks_count = esb->s_free_blocks_count;
	EXT2_SB(sb).free_inodes_count = esb->s_free_inodes_count;
	EXT2_SB(sb).inode_size = esb->s_inode_size;
	EXT2_SB(sb).incompat_features = esb->s_feature_incompat;
	EXT2_SB(sb).ro_compat_features = esb->s_feature_ro_compat;
	EXT2_SB(sb).readonly = 0;

	error = ext2_load_group_descriptors(sb, esb);
	if (error) {
		kfree(buf);
		return NULL;
	}

	sb->s_block_size = block_size;
	sb->s_magic = EXT2_SUPER_MAGIC;
	sb->s_op = &ext2_sops;
	sb->s_mounted = iget(sb, EXT2_ROOT_INO);
	if (!sb->s_mounted) {
		kfree(EXT2_SB(sb).group_desc);
		EXT2_SB(sb).group_desc = NULL;
		kfree(buf);
		return NULL;
	}

	kfree(buf);
	return sb;
}

int ext2_write_super(struct super_block *sb)
{
	struct ext2_super_block *esb;
	uint8_t *buf;
	int error;

	if (!sb || EXT2_SB(sb).readonly)
		return -EROFS;

	buf = kmalloc(1024);
	if (!buf)
		return -ENOMEM;
	error = blkdev_read(sb->s_dev, 2, 2, (char *)buf);
	if (error) {
		kfree(buf);
		return error;
	}

	esb = (struct ext2_super_block *)buf;
	ext2_write32((uint8_t *)&esb->s_free_blocks_count,
		     EXT2_SB(sb).free_blocks_count);
	ext2_write32((uint8_t *)&esb->s_free_inodes_count,
		     EXT2_SB(sb).free_inodes_count);
	ext2_write32((uint8_t *)&esb->s_wtime, current_time());
	ext2_write16((uint8_t *)&esb->s_state, 1);
	error = blkdev_write(sb->s_dev, 2, 2, (char *)buf);
	kfree(buf);
	return error;
}

struct file_system_type ext2 = {
	.read_super = ext2_read_super,
	.list = LIST_HEAD_INIT(ext2.list),
	.name = "ext2",
};
