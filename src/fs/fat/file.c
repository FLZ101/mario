#include <fs/fs.h>
#include <fs/fat/fat.h>

#include <mm/kmalloc.h>

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

struct inode_operations fat_file_iops = {
	/* Read-only milestone: truncate is not available. */
};

struct file_operations fat_file_fops = {
	.read = fat_file_read,
};
