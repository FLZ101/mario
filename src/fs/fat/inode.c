#include <fs/fs.h>
#include <fs/fat/fat.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

static void fat_set_ops(struct inode *inode)
{
	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &fat_file_iops;
		inode->i_fop = &fat_file_fops;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &fat_dir_iops;
		inode->i_fop = &fat_dir_fops;
	} else {
		inode->i_op = NULL;
		inode->i_fop = NULL;
	}
}

static int fat_populate_from_entry(struct inode *inode, uint32_t cluster,
				   uint32_t offset, const uint8_t *entry)
{
	struct fat_inode_info *info = &FAT_INODE(inode);
	uint8_t attributes = entry[11];

	info->start_cluster = fat_entry_first_cluster(entry);
	info->dir_cluster = cluster;
	info->dir_offset = offset;
	info->attributes = attributes;

	inode->i_size = fat_entry_size(entry);
	inode->i_block_size = FAT_CLUSTER_SIZE(inode->i_sb);
	inode->i_nr_block = (inode->i_size + 511) / 512;
	inode->i_rdev = 0;
	inode->i_atime = inode->i_mtime = inode->i_ctime = 0;

	if (attributes & FAT_ATTR_DIRECTORY) {
		inode->i_mode = S_IFDIR | 0777;
		inode->i_nlink = 2;
	} else {
		inode->i_mode = S_IFREG | 0777;
		inode->i_nlink = 1;
	}

	fat_set_ops(inode);
	return 0;
}

int fat_read_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct fat_inode_info *info = &FAT_INODE(inode);
	uint32_t cluster;
	uint32_t offset;
	uint8_t *buf;
	const uint8_t *entry;
	int error;

	if (!sb || !inode->i_ino)
		return -EINVAL;

	/* The root directory is not represented by a directory entry. */
	if (inode->i_ino == FAT_ROOT_INO) {
		info->start_cluster = FAT_SB(sb).root_cluster;
		info->dir_cluster = 0;
		info->dir_offset = 0;
		info->parent_ino = 0;
		info->attributes = FAT_ATTR_DIRECTORY;
		inode->i_mode = S_IFDIR | 0777;
		inode->i_size = 0;
		inode->i_block_size = FAT_CLUSTER_SIZE(sb);
		inode->i_nr_block = 0;
		inode->i_nlink = 2;
		inode->i_rdev = 0;
		inode->i_atime = inode->i_mtime = inode->i_ctime = 0;
		fat_set_ops(inode);
		return 0;
	}

	error = fat_ino_location(sb, inode->i_ino, &cluster, &offset);
	if (error)
		return error;

	buf = kmalloc(FAT_CLUSTER_SIZE(sb));
	if (!buf)
		return -ENOMEM;
	error = fat_read_cluster(sb, cluster, buf);
	if (error) {
		kfree(buf);
		return error;
	}

	entry = buf + offset;
	if (entry[0] == 0x00 || entry[0] == 0xe5 ||
	    (entry[11] == FAT_ATTR_LFN) ||
	    (entry[11] & FAT_ATTR_VOLUME_ID)) {
		kfree(buf);
		return -ENOENT;
	}

	error = fat_populate_from_entry(inode, cluster, offset, entry);
	kfree(buf);
	return error;
}

int fat_write_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct fat_inode_info *info = &FAT_INODE(inode);
	uint32_t cluster;
	uint32_t offset;
	uint8_t *buf;
	uint8_t *entry;
	int error;

	if (!sb || !inode->i_ino)
		return -EINVAL;

	/* The root directory has no on-disk directory entry. */
	if (inode->i_ino == FAT_ROOT_INO)
		return 0;

	error = fat_ino_location(sb, inode->i_ino, &cluster, &offset);
	if (error)
		return error;

	buf = kmalloc(FAT_CLUSTER_SIZE(sb));
	if (!buf)
		return -ENOMEM;
	error = fat_read_cluster(sb, cluster, buf);
	if (error)
		goto out;

	entry = buf + offset;
	if (entry[0] == 0x00 || entry[0] == 0xe5 ||
	    entry[11] == FAT_ATTR_LFN ||
	    (entry[11] & FAT_ATTR_VOLUME_ID)) {
		error = -ENOENT;
		goto out;
	}

	entry[11] = info->attributes;
	fat_write16(entry + 20, info->start_cluster >> 16);
	fat_write16(entry + 26, info->start_cluster);
	fat_write32(entry + 28, inode->i_size);
	error = fat_write_cluster(sb, cluster, buf);
out:
	kfree(buf);
	return error;
}

void fat_clear_inode(struct inode *inode)
{
	/* All FAT inode state is embedded in struct inode. */
}
