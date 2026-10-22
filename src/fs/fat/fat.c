#include <fs/fs.h>
#include <fs/fat/fat.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

/*
 * Read one data cluster from a FAT32 filesystem.  Cluster numbers start at 2;
 * clusters 0 and 1 are reserved and are never used for data.
 */
int fat_read_cluster(struct super_block *sb, uint32_t cluster, void *buf)
{
	uint32_t sector;

	if (!sb || !buf)
		return -EINVAL;
	if (cluster < 2 || cluster - 2 >= FAT_SB(sb).total_clusters)
		return -EINVAL;

	sector = FAT_SB(sb).first_data_sector +
		 (cluster - 2) * FAT_SB(sb).sectors_per_cluster;
	return blkdev_read(sb->s_dev, sector,
			   FAT_SB(sb).sectors_per_cluster, buf);
}

/*
 * Return the next cluster in a FAT chain.  This is the common validation
 * point used by file reads and directory scans.
 */
int fat_read_fat_entry(struct super_block *sb, uint32_t cluster,
		       uint32_t *value)
{
	struct fat_sb_info *fsb = &FAT_SB(sb);
	uint32_t sector;
	uint32_t offset;
	int error;

	if (!sb || !value || cluster < 2 ||
	    cluster - 2 >= fsb->total_clusters)
		return -EINVAL;

	sector = fsb->reserved_sectors + (cluster * 4) /
		 fsb->bytes_per_sector;
	offset = (cluster * 4) % fsb->bytes_per_sector;

	if (!fsb->fat_cache_valid || fsb->fat_cache_sector != sector) {
		error = blkdev_read(sb->s_dev, sector, 1,
				    (char *)fsb->fat_cache);
		if (error)
			return error;
		fsb->fat_cache_sector = sector;
		fsb->fat_cache_valid = 1;
	}

	*value = fat_read32(fsb->fat_cache + offset) & 0x0fffffff;
	return 0;
}

int fat_next_cluster(struct super_block *sb, uint32_t cluster,
		     uint32_t *next)
{
	uint32_t value;
	int error;

	error = fat_read_fat_entry(sb, cluster, &value);
	if (error)
		return error;
	if (value >= FAT_EOC_MIN && value <= FAT_EOC_MAX) {
		*next = 0;
		return -ENOENT;
	}
	if (value == 0 || value == 1 || value == FAT_BAD_CLUSTER ||
	    value - 2 >= FAT_SB(sb).total_clusters)
		return -EIO;

	*next = value;
	return 0;
}

/*
 * Count the free data clusters in a FAT32 volume.  This is used by statfs
 * rather than being maintained incrementally, so it remains correct even if
 * an allocation rollback or an external tool changes the FAT directly.
 */
int fat_count_free_clusters(struct super_block *sb, uint32_t *free_count)
{
	uint32_t cluster;
	uint32_t free;
	int error;

	if (!sb || !free_count)
		return -EINVAL;

	free = 0;
	for (cluster = 2;
	     cluster - 2 < FAT_SB(sb).total_clusters;
	     cluster++) {
		uint32_t value;

		error = fat_read_fat_entry(sb, cluster, &value);
		if (error)
			return error;
		if (value == 0)
			free++;
	}
	*free_count = free;
	return 0;
}

/*
 * Walk a FAT chain forward and return the physical cluster corresponding to
 * cluster_index logical clusters from the inode's start cluster.  Zero means
 * the chain ended, which is treated as end-of-file for a regular file or end
 * of directory for a directory.
 */
uint32_t fat_cluster_at(struct inode *inode, uint32_t cluster_index)
{
	uint32_t cluster = FAT_INODE(inode).start_cluster;

	while (cluster_index--) {
		uint32_t next;

		if (fat_next_cluster(inode->i_sb, cluster, &next))
			return 0;
		cluster = next;
	}
	return cluster;
}

uint32_t fat_entry_first_cluster(const uint8_t *entry)
{
	return ((uint32_t)fat_read16(entry + 20) << 16) |
	       fat_read16(entry + 26);
}

uint32_t fat_entry_size(const uint8_t *entry)
{
	return fat_read32(entry + 28);
}

/*
 * Synthetic inode numbers are derived from the byte offset of the short-name
 * entry inside its directory cluster.  Root is FAT_ROOT_INO; all other inodes
 * are FAT_ROOT_INO + 1 plus that byte offset.
 */
unsigned long fat_entry_ino(struct super_block *sb, uint32_t cluster,
			    uint32_t offset)
{
	return FAT_ROOT_INO + 1UL +
	       (unsigned long)(cluster - 2) * FAT_CLUSTER_SIZE(sb) + offset;
}

int fat_ino_location(struct super_block *sb, unsigned long ino,
		     uint32_t *cluster, uint32_t *offset)
{
	uint32_t physical;

	if (!sb || !cluster || !offset || ino <= FAT_ROOT_INO)
		return -EINVAL;

	physical = ino - FAT_ROOT_INO - 1;
	if (physical % FAT_ENTRY_SIZE)
		return -EINVAL;
	*cluster = physical / FAT_CLUSTER_SIZE(sb) + 2;
	*offset = physical % FAT_CLUSTER_SIZE(sb);
	if (*cluster - 2 >= FAT_SB(sb).total_clusters ||
	    *offset + FAT_ENTRY_SIZE > FAT_CLUSTER_SIZE(sb))
		return -EINVAL;
	return 0;
}
