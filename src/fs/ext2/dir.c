#include <fs/fs.h>
#include <fs/ext2/ext2.h>
#include <fs/dirent.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

/*
 * Directory traversal for the read-only ext2 driver.
 *
 * Directory blocks are arrays of variable-length `ext2_dir_entry_2` records.
 * `lookup` scans them linearly for a name, while `readdir` uses `file->f_pos`
 * as a byte offset so getdents can resume in the middle of a directory.  A
 * `rec_len` larger than the current entry skips deleted entries.
 *
 * Indexed (hashed) directories are not searched linearly because their
 * records do not preserve plain ext2 name lookup semantics; fail explicitly
 * rather than silently returning wrong results.
 */
static int ext2_dir_is_indexed(struct inode *dir)
{
	return !!(ext2_read32((const uint8_t *)&EXT2_INODE(dir).raw.i_flags) &
		  EXT2_BTREE_FL);
}

static int ext2_lookup(struct inode *dir, char *name, int len,
		       struct inode **res)
{
	struct super_block *sb;
	uint32_t block;
	uint32_t block_index;
	uint32_t block_start;
	uint32_t limit;
	uint32_t offset;
	uint8_t *buf;
	int error;
	int found;

	*res = NULL;
	if (!dir)
		return -ENOENT;
	if (!S_ISDIR(dir->i_mode)) {
		iput(dir);
		return -ENOENT;
	}
	if (ext2_dir_is_indexed(dir)) {
		iput(dir);
		return -ENOTSUP;
	}
	if (len == 1 && name[0] == '.') {
		*res = dir;
		return 0;
	}

	sb = dir->i_sb;
	found = 0;
	block_index = 0;
	while ((uint64_t)block_index * sb->s_block_size < (uint64_t)dir->i_size) {
		block = ext2_bmap(dir, block_index, 0);
		if (!block) {
			block_index++;
			continue;
		}

		buf = kmalloc(sb->s_block_size);
		if (!buf) {
			iput(dir);
			return -ENOMEM;
		}
		error = ext2_read_block(sb, block, buf);
		if (error) {
			kfree(buf);
			iput(dir);
			return error;
		}

		block_start = block_index * sb->s_block_size;
		limit = sb->s_block_size;
		if ((uint64_t)block_start + limit > (uint64_t)dir->i_size)
			limit = dir->i_size - block_start;

		for (offset = 0; offset < limit;) {
			struct ext2_dir_entry_2 *entry;
			uint16_t rec_len;

			entry = (struct ext2_dir_entry_2 *)(buf + offset);
			rec_len = ext2_read16((const uint8_t *)&entry->rec_len);
			if (rec_len < 8 || offset + rec_len > limit)
				break;
		if (ext2_read32((const uint8_t *)&entry->inode) &&
		    entry->name_len == len &&
			    !memcmp(entry->name, name, len)) {
				found = 1;
				break;
			}
			offset += rec_len;
		}

		if (found) {
			struct ext2_dir_entry_2 *entry =
				(struct ext2_dir_entry_2 *)(buf + offset);
			uint32_t ino =
				ext2_read32((const uint8_t *)&entry->inode);
			struct inode *inode = iget(sb, ino);

			kfree(buf);
			iput(dir);
			*res = inode;
			return 0;
		}

		kfree(buf);
		block_index++;
	}

	iput(dir);
	return -ENOENT;
}

static int ext2_readdir(struct inode *dir, struct file *file, void *dirent,
			filldir_t filldir)
{
	struct super_block *sb = dir->i_sb;
	uint32_t block_size = sb->s_block_size;
	uint32_t block_index;
	uint32_t block_start;
	uint32_t limit;
	uint32_t offset;
	uint32_t physical;
	uint8_t *buf;
	int error;

	if (ext2_dir_is_indexed(dir))
		return -ENOTSUP;

	while ((uint64_t)file->f_pos < (uint64_t)dir->i_size) {
		block_index = file->f_pos / block_size;
		block_start = block_index * block_size;
		physical = ext2_bmap(dir, block_index, 0);
		if (!physical)
			return -EIO;

		buf = kmalloc(block_size);
		if (!buf)
			return -ENOMEM;
		error = ext2_read_block(sb, physical, buf);
		if (error) {
			kfree(buf);
			return error;
		}

		limit = block_size;
		if ((uint64_t)block_start + limit > (uint64_t)dir->i_size)
			limit = dir->i_size - block_start;
		offset = file->f_pos - block_start;

		while (offset < limit) {
			struct ext2_dir_entry_2 *entry;
			uint16_t rec_len;
			uint8_t file_type;

			entry = (struct ext2_dir_entry_2 *)(buf + offset);
			rec_len = ext2_read16((const uint8_t *)&entry->rec_len);
			if (rec_len < 8 || offset + rec_len > limit) {
				kfree(buf);
				return -EIO;
			}

			if (ext2_read32((const uint8_t *)&entry->inode)) {
				uint32_t ino =
					ext2_read32((const uint8_t *)&entry->inode);
				file_type = EXT2_SB(sb).incompat_features &
					EXT2_FEATURE_INCOMPAT_FILETYPE ?
					entry->file_type : DT_UNKNOWN;
				if (filldir(dirent, entry->name, entry->name_len,
					    block_start + offset + rec_len,
					    ino, file_type)) {
					kfree(buf);
					return 0;
				}
			}
			file->f_pos = block_start + offset + rec_len;
			offset += rec_len;
		}

		kfree(buf);
	}

	return 0;
}

struct inode_operations ext2_dir_iops = {
	.lookup = ext2_lookup,
};

struct file_operations ext2_dir_fops = {
	.readdir = ext2_readdir,
};
