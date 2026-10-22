#include <fs/fs.h>
#include <fs/fat/fat.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

static int fat_file_read(struct inode *inode, struct file *file, char *buf,
			 int count)
{
	struct super_block *sb;
	uint32_t cluster_size;
	uint64_t total;
	uint32_t done;

	if (!inode || !S_ISREG(inode->i_mode))
		return -EINVAL;
	if (file->f_pos >= inode->i_size)
		return 0;

	sb = inode->i_sb;
	cluster_size = FAT_CLUSTER_SIZE(sb);
	total = MIN((uint64_t)count, (uint64_t)(inode->i_size - file->f_pos));
	done = 0;

	while (done < total) {
		uint32_t cluster_index = file->f_pos / cluster_size;
		uint32_t offset = file->f_pos % cluster_size;
		uint32_t chunk = cluster_size - offset;
		uint32_t cluster;
		uint8_t *cluster_buf;
		int error;

		if (chunk > total - done)
			chunk = total - done;

		cluster = fat_cluster_at(inode, cluster_index);
		if (!cluster)
			return done ? (int)done : -EIO;

		cluster_buf = kmalloc(cluster_size);
		if (!cluster_buf)
			return done ? (int)done : -ENOMEM;
		error = fat_read_cluster(sb, cluster, cluster_buf);
		if (error) {
			kfree(cluster_buf);
			return done ? (int)done : error;
		}

		memcpy_tofs(buf + done, cluster_buf + offset, chunk);
		kfree(cluster_buf);
		done += chunk;
		file->f_pos += chunk;
	}

	return done;
}

static int fat_zero_range(struct inode *inode, uint64_t start,
			      uint64_t end)
{
	struct super_block *sb = inode->i_sb;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(sb);
	int error;

	if (end <= start)
		return 0;

	while (start < end) {
		uint32_t cluster_index = start / cluster_size;
		uint32_t offset = start % cluster_size;
		uint32_t chunk = cluster_size - offset;
		uint32_t cluster;
		uint8_t *buf;

		if (chunk > end - start)
			chunk = end - start;

		cluster = fat_cluster_at_alloc(inode, cluster_index, &error);
		if (!cluster) {
			error = error ? error : -EIO;
			return error;
		}

		buf = kmalloc(cluster_size);
		if (!buf)
			return -ENOMEM;
		if (offset == 0 && chunk == cluster_size) {
			memset(buf, 0, cluster_size);
		} else {
			error = fat_read_cluster(sb, cluster, buf);
			if (error) {
				kfree(buf);
				return error;
			}
			memset(buf + offset, 0, chunk);
		}
		error = fat_write_cluster(sb, cluster, buf);
		kfree(buf);
		if (error)
			return error;

		start += chunk;
	}

	return 0;
}

static int fat_file_truncate(struct inode *inode, int size)
{
	struct super_block *sb;
	uint32_t cluster_size;
	uint32_t needed;
	uint32_t have;
	uint64_t old_size;
	int error;

	if (!inode || !S_ISREG(inode->i_mode))
		return -EINVAL;
	if (size < 0)
		return -EINVAL;
	if (FAT_SB(inode->i_sb).readonly)
		return -EROFS;

	sb = inode->i_sb;
	cluster_size = FAT_CLUSTER_SIZE(sb);
	old_size = inode->i_size;
	needed = (uint32_t)((uint64_t)size + cluster_size - 1) /
		 cluster_size;
	have = fat_cluster_count(inode);

	if (needed < have) {
		if (!needed) {
			error = fat_free_cluster_chain(sb,
						      FAT_INODE(inode).start_cluster);
			if (error)
				return error;
			FAT_INODE(inode).start_cluster = 0;
		} else {
			uint32_t keep;
			uint32_t tail;

			error = 0;
			keep = fat_cluster_at_alloc(inode, needed - 1, &error);
			if (error)
				return error;
			error = fat_next_cluster(sb, keep, &tail);
			if (error == -ENOENT)
				tail = 0;
			else if (error)
				return error;
			error = fat_write_fat_entry(sb, keep, FAT_EOC_MAX);
			if (error)
				return error;
			if (tail) {
				error = fat_free_cluster_chain(sb, tail);
				if (error)
					return error;
			}
		}
	} else if (needed > have) {
		error = 0;
		fat_cluster_at_alloc(inode, needed - 1, &error);
		if (error)
			return error;
	}

	if ((uint64_t)size > old_size) {
		error = fat_zero_range(inode, old_size, size);
		if (error)
			return error;
	}

	inode->i_size = size;
	inode->i_nr_block = ((uint64_t)size + 511) / 512;
	FAT_INODE(inode).attributes |= FAT_ATTR_ARCHIVE;
	set_bit(I_Dirty, &inode->i_state);
	return fat_write_inode(inode);
}

static int fat_file_write(struct inode *inode, struct file *file, char *buf,
			  int count)
{
	struct super_block *sb;
	uint32_t cluster_size;
	uint64_t old_size;
	uint64_t start;
	uint64_t total;
	uint32_t old_clusters;
	int error;

	if (!inode || !S_ISREG(inode->i_mode))
		return -EINVAL;
	if (count <= 0)
		return 0;
	if (FAT_SB(inode->i_sb).readonly)
		return -EROFS;

	down(&inode->i_sem);
	sb = inode->i_sb;
	cluster_size = FAT_CLUSTER_SIZE(sb);
	old_size = inode->i_size;
	old_clusters = (uint32_t)((old_size + cluster_size - 1) /
				  cluster_size);
	start = (file->f_flags & O_APPEND) ? old_size : file->f_pos;
	total = 0;
	error = 0;

	/*
	 * A seek beyond EOF creates a hole.  Zero the gap before writing so
	 * that existing-cluster tails and newly allocated clusters contain no
	 * stale bytes.
	 */
	if (start > old_size) {
		error = fat_zero_range(inode, old_size, start);
		if (error) {
			up(&inode->i_sem);
			return error;
		}
	}

	while ((uint64_t)total < (uint64_t)count) {
		uint32_t cluster_index = start / cluster_size;
		uint32_t offset = start % cluster_size;
		uint32_t chunk = cluster_size - offset;
		uint32_t cluster;
		uint8_t *cluster_buf;

		if ((uint64_t)chunk > (uint64_t)count - total)
			chunk = (uint32_t)((uint64_t)count - total);

		cluster = fat_cluster_at_alloc(inode, cluster_index, &error);
		if (!cluster) {
			error = error ? error : -EIO;
			break;
		}

		cluster_buf = kmalloc(cluster_size);
		if (!cluster_buf) {
			error = -ENOMEM;
			break;
		}
		if (cluster_index < old_clusters) {
			error = fat_read_cluster(sb, cluster, cluster_buf);
			if (error) {
				kfree(cluster_buf);
				break;
			}
		} else {
			memset(cluster_buf, 0, cluster_size);
		}

		memcpy_fromfs(cluster_buf + offset, buf + total, chunk);
		error = fat_write_cluster(sb, cluster, cluster_buf);
		kfree(cluster_buf);
		if (error)
			break;

		total += chunk;
		start += chunk;
		file->f_pos = start;
	}

	if (start > inode->i_size) {
		inode->i_size = start;
		inode->i_nr_block = ((uint64_t)start + 511) / 512;
	}
	if (total) {
		FAT_INODE(inode).attributes |= FAT_ATTR_ARCHIVE;
		set_bit(I_Dirty, &inode->i_state);
		error = fat_write_inode(inode);
	}

	up(&inode->i_sem);
	return total ? (int)total : error;
}

struct inode_operations fat_file_iops = {
	.truncate = fat_file_truncate,
};

struct file_operations fat_file_fops = {
	.read = fat_file_read,
	.write = fat_file_write,
};
