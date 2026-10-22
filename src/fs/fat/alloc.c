#include <fs/fs.h>
#include <fs/fat/fat.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

/*
 * Write one data cluster.  All cluster validation is shared with the read
 * path so that a bad inode cannot address metadata outside the data area.
 */
int fat_write_cluster(struct super_block *sb, uint32_t cluster,
		      const void *buf)
{
	uint32_t sector;

	if (!sb || !buf)
		return -EINVAL;
	if (cluster < 2 || cluster - 2 >= FAT_SB(sb).total_clusters)
		return -EINVAL;

	sector = FAT_SB(sb).first_data_sector +
		 (cluster - 2) * FAT_SB(sb).sectors_per_cluster;
	return blkdev_write(sb->s_dev, sector,
			    FAT_SB(sb).sectors_per_cluster, (char *)buf);
}

/*
 * Update one FAT32 entry in every FAT copy.  FAT32 uses only the low 28 bits
 * of each 32-bit entry.
 */
int fat_write_fat_entry(struct super_block *sb, uint32_t cluster,
			uint32_t value)
{
	struct fat_sb_info *fsb = &FAT_SB(sb);
	uint32_t fat_sector;
	uint32_t offset;
	uint8_t *buf;
	int i;
	int error;

	if (!sb || cluster < 2 || cluster - 2 >= fsb->total_clusters)
		return -EINVAL;
	if (value != FAT_BAD_CLUSTER && value != 0 &&
	    !(value >= FAT_EOC_MIN && value <= FAT_EOC_MAX) &&
	    value - 2 >= fsb->total_clusters)
		return -EINVAL;

	fat_sector = (cluster * 4) / fsb->bytes_per_sector;
	offset = (cluster * 4) % fsb->bytes_per_sector;
	buf = kmalloc(fsb->bytes_per_sector);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < fsb->num_fats; i++) {
		uint32_t sector = fsb->reserved_sectors +
				  i * fsb->fat_size32 + fat_sector;

		error = blkdev_read(sb->s_dev, sector, 1, (char *)buf);
		if (error)
			goto out;
		fat_write32(buf + offset, value);
		error = blkdev_write(sb->s_dev, sector, 1, (char *)buf);
		if (error)
			goto out;

		/* Keep the primary FAT sector cache coherent. */
		if (i == 0) {
			memcpy(fsb->fat_cache, buf, fsb->bytes_per_sector);
			fsb->fat_cache_sector = sector;
			fsb->fat_cache_valid = 1;
		}
	}
	error = 0;
out:
	kfree(buf);
	return error;
}

/*
 * Find a free FAT32 cluster, mark it end-of-chain, and link it after
 * @previous when @previous is non-zero.
 */
int fat_alloc_cluster(struct super_block *sb, uint32_t previous,
		      uint32_t *cluster)
{
	struct fat_sb_info *fsb = &FAT_SB(sb);
	uint32_t start;
	uint32_t candidate;
	int error;

	if (!sb || !cluster)
		return -EINVAL;
	if (previous && (previous < 2 || previous - 2 >= fsb->total_clusters))
		return -EINVAL;

	start = fsb->next_free ? fsb->next_free : 2;
	if (start < 2)
		start = 2;
	if (start - 2 >= fsb->total_clusters)
		start = 2;

	/*
	 * Scan from the hint to the end, then wrap and scan the beginning.
	 * The wrap makes sure every free entry is reachable when the hint is
	 * in the middle of the FAT.
	 */
	for (candidate = start; candidate - 2 < fsb->total_clusters; candidate++) {
		uint32_t value;

		error = fat_read_fat_entry(sb, candidate, &value);
		if (error)
			return error;
		if (value == 0) {
			error = fat_write_fat_entry(sb, candidate, FAT_EOC_MAX);
			if (error)
				return error;
			if (previous) {
				error = fat_write_fat_entry(sb, previous, candidate);
				if (error)
					return error;
			}
			fsb->next_free = candidate + 1;
			if (fsb->next_free - 2 >= fsb->total_clusters)
				fsb->next_free = 2;
			*cluster = candidate;
			return 0;
		}
	}

	/* Wrap around and scan the entries before the allocation hint. */
	for (candidate = 2; candidate < start; candidate++) {
		uint32_t value;

		error = fat_read_fat_entry(sb, candidate, &value);
		if (error)
			return error;
		if (value == 0) {
			error = fat_write_fat_entry(sb, candidate, FAT_EOC_MAX);
			if (error)
				return error;
			if (previous) {
				error = fat_write_fat_entry(sb, previous, candidate);
				if (error)
					return error;
			}
			fsb->next_free = candidate + 1;
			if (fsb->next_free - 2 >= fsb->total_clusters)
				fsb->next_free = 2;
			*cluster = candidate;
			return 0;
		}
	}

	return -ENOSPC;
}

/*
 * Free all clusters in a FAT chain.  The loop is bounded by the filesystem
 * cluster count so a corrupt chain cannot make this walk forever.
 */
int fat_free_cluster_chain(struct super_block *sb, uint32_t cluster)
{
	struct fat_sb_info *fsb = &FAT_SB(sb);
	uint32_t seen;
	int error;

	if (!sb || !cluster)
		return 0;

	seen = 0;
	while (cluster && seen++ < fsb->total_clusters) {
		uint32_t next;

		error = fat_next_cluster(sb, cluster, &next);
		if (error == -ENOENT) {
			error = fat_write_fat_entry(sb, cluster, 0);
			return error;
		}
		if (error)
			return error;
		error = fat_write_fat_entry(sb, cluster, 0);
		if (error)
			return error;
		cluster = next;
	}

	return cluster ? -EIO : 0;
}

/*
 * Count the clusters currently present in an inode's FAT chain.  This is
 * used by truncate and file extension.
 */
uint32_t fat_cluster_count(struct inode *inode)
{
	uint32_t cluster = FAT_INODE(inode).start_cluster;
	uint32_t count = 0;

	while (cluster) {
		uint32_t next;

		count++;
		if (fat_next_cluster(inode->i_sb, cluster, &next))
			break;
		cluster = next;
	}
	return count;
}

/*
 * Return the physical cluster for a logical cluster index, optionally
 * extending the chain with newly allocated clusters.  When the inode had no
 * cluster yet and a new cluster is allocated, the in-memory start cluster is
 * updated immediately; fat_write_inode() later persists it to the directory
 * entry.
 */
uint32_t fat_cluster_at_alloc(struct inode *inode, uint32_t cluster_index,
			      int *error)
{
	uint32_t cluster = FAT_INODE(inode).start_cluster;
	uint32_t current_index = 0;
	uint32_t previous = 0;

	*error = 0;
	if (cluster && cluster_index == 0)
		return cluster;

	/*
	 * Walk an existing chain up to cluster_index.  When the chain ends
	 * early, current_index is advanced to the first missing logical slot
	 * and previous is the cluster that slot must be linked after.
	 */
	while (cluster && current_index < cluster_index) {
		uint32_t next;

		previous = cluster;
		*error = fat_next_cluster(inode->i_sb, cluster, &next);
		if (*error) {
			if (*error != -ENOENT)
				return 0;
			*error = 0;
			cluster = 0;
			current_index++;
			break;
		}
		cluster = next;
		current_index++;
	}

	if (cluster)
		return cluster;

	/* Allocate every missing logical cluster from current_index up to target. */
	while (current_index <= cluster_index) {
		uint32_t next;

		*error = fat_alloc_cluster(inode->i_sb, previous, &next);
		if (*error)
			return 0;
		if (!FAT_INODE(inode).start_cluster)
			FAT_INODE(inode).start_cluster = next;
		previous = next;
		cluster = next;
		current_index++;
	}

	return cluster;
}
