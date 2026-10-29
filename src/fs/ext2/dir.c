#include <fs/fs.h>
#include <fs/ext2/ext2.h>
#include <fs/dirent.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

extern uint64_t current_time(void);

static int ext2_dir_is_indexed(struct inode *dir)
{
	return !!(ext2_read32((const uint8_t *)&EXT2_INODE(dir).raw.i_flags) &
		  EXT2_BTREE_FL);
}

static uint8_t ext2_file_type(umode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:
		return EXT2_FT_REG_FILE;
	case S_IFDIR:
		return EXT2_FT_DIR;
	case S_IFCHR:
		return EXT2_FT_CHRDEV;
	case S_IFBLK:
		return EXT2_FT_BLKDEV;
	case S_IFIFO:
		return EXT2_FT_FIFO;
	case S_IFLNK:
		return EXT2_FT_SYMLINK;
	default:
		return EXT2_FT_UNKNOWN;
	}
}

static void ext2_touch_dir(struct inode *dir)
{
	struct ext2_inode *raw = &EXT2_INODE(dir).raw;

	dir->i_mtime = dir->i_ctime = current_time();
	ext2_write32((uint8_t *)&raw->i_mtime, dir->i_mtime);
	ext2_write32((uint8_t *)&raw->i_ctime, dir->i_ctime);
	set_bit(I_Dirty, &dir->i_state);
}

static int ext2_find_entry(struct inode *dir, const char *name, int len,
			   uint32_t *res_block, uint32_t *res_offset)
{
	struct super_block *sb = dir->i_sb;
	uint32_t block_index;
	int error;

	if (ext2_dir_is_indexed(dir))
		return -ENOTSUP;

	for (block_index = 0;
	     (uint64_t)block_index * sb->s_block_size < (uint64_t)dir->i_size;
	     block_index++) {
		uint32_t block = ext2_bmap(dir, block_index, 0);
		uint32_t block_start = block_index * sb->s_block_size;
		uint32_t limit = sb->s_block_size;
		uint32_t offset;
		uint8_t *buf;

		if (!block)
			continue;
		if ((uint64_t)block_start + limit > (uint64_t)dir->i_size)
			limit = dir->i_size - block_start;

		buf = kmalloc(sb->s_block_size);
		if (!buf)
			return -ENOMEM;
		error = ext2_read_block(sb, block, buf);
		if (error) {
			kfree(buf);
			return error;
		}

		for (offset = 0; offset < limit;) {
			struct ext2_dir_entry_2 *entry =
				(struct ext2_dir_entry_2 *)(buf + offset);
			uint16_t rec_len =
				ext2_read16((const uint8_t *)&entry->rec_len);

			if (rec_len < 8 || offset + rec_len > limit)
				break;
			if (ext2_read32((const uint8_t *)&entry->inode) &&
			    entry->name_len == len &&
			    !memcmp(entry->name, name, len)) {
				*res_block = block;
				*res_offset = offset;
				kfree(buf);
				return 0;
			}
			offset += rec_len;
		}
		kfree(buf);
	}

	return -ENOENT;
}

static int ext2_add_entry(struct inode *dir, const char *name, int len,
			  uint32_t ino, uint8_t file_type)
{
	struct super_block *sb = dir->i_sb;
	struct ext2_inode *raw = &EXT2_INODE(dir).raw;
	uint32_t block_size = sb->s_block_size;
	uint16_t rec_len = EXT2_DIR_REC_LEN(len);
	uint32_t block_index;
	uint32_t physical;
	uint8_t *buf;
	int error;

	if (len > EXT2_NAME_LEN)
		return -ENAMETOOLONG;

	for (block_index = 0;
	     (uint64_t)block_index * block_size < (uint64_t)dir->i_size;
	     block_index++) {
		uint32_t block_start = block_index * block_size;
		uint32_t limit = block_size;
		uint32_t offset;
		uint32_t prev_offset = 0;
		uint16_t prev_rec = 0;

		physical = ext2_bmap(dir, block_index, 0);
		if (!physical)
			continue;
		if ((uint64_t)block_start + limit > (uint64_t)dir->i_size)
			limit = dir->i_size - block_start;

		buf = kmalloc(block_size);
		if (!buf)
			return -ENOMEM;
		error = ext2_read_block(sb, physical, buf);
		if (error) {
			kfree(buf);
			return error;
		}

		for (offset = 0; offset < limit;) {
			struct ext2_dir_entry_2 *entry =
				(struct ext2_dir_entry_2 *)(buf + offset);
			uint16_t entry_rec =
				ext2_read16((const uint8_t *)&entry->rec_len);

			if (entry_rec < 8 || offset + entry_rec > limit)
				break;
			if (!ext2_read32((const uint8_t *)&entry->inode) &&
			    entry_rec >= rec_len) {
				ext2_write32((uint8_t *)&entry->inode, ino);
				ext2_write16((uint8_t *)&entry->rec_len, entry_rec);
				entry->name_len = len;
				entry->file_type = file_type;
				memcpy(entry->name, name, len);
				error = ext2_write_block(sb, physical, buf);
				if (!error)
					ext2_touch_dir(dir);
				kfree(buf);
				return error;
			}
			prev_offset = offset;
			prev_rec = entry_rec;
			offset += entry_rec;
		}

		if (prev_rec) {
			struct ext2_dir_entry_2 *prev =
				(struct ext2_dir_entry_2 *)(buf + prev_offset);
			uint16_t used = EXT2_DIR_REC_LEN(prev->name_len);
			uint32_t new_offset = prev_offset + used;

			if (new_offset + rec_len <= limit) {
				struct ext2_dir_entry_2 *entry;

				ext2_write16((uint8_t *)&prev->rec_len, used);
				entry = (struct ext2_dir_entry_2 *)(buf + new_offset);
				ext2_write32((uint8_t *)&entry->inode, ino);
				ext2_write16((uint8_t *)&entry->rec_len,
					     limit - new_offset);
				entry->name_len = len;
				entry->file_type = file_type;
				memcpy(entry->name, name, len);
				error = ext2_write_block(sb, physical, buf);
				if (!error)
					ext2_touch_dir(dir);
				kfree(buf);
				return error;
			}
		}
		kfree(buf);
	}

	block_index = dir->i_size / block_size;
	physical = ext2_bmap(dir, block_index, 1);
	if (!physical)
		return -ENOSPC;

	buf = kmalloc(block_size);
	if (!buf)
		return -ENOMEM;
	memset(buf, 0, block_size);
	{
		struct ext2_dir_entry_2 *entry = (struct ext2_dir_entry_2 *)buf;

		ext2_write32((uint8_t *)&entry->inode, ino);
		ext2_write16((uint8_t *)&entry->rec_len, block_size);
		entry->name_len = len;
		entry->file_type = file_type;
		memcpy(entry->name, name, len);
	}
	error = ext2_write_block(sb, physical, buf);
	if (!error) {
		dir->i_size += block_size;
		ext2_write32((uint8_t *)&raw->i_size, dir->i_size);
		ext2_touch_dir(dir);
	}
	kfree(buf);
	return error;
}

static int ext2_delete_entry(struct inode *dir, const char *name, int len)
{
	struct super_block *sb = dir->i_sb;
	uint32_t block;
	uint32_t offset;
	uint8_t *buf;
	struct ext2_dir_entry_2 *entry;
	int error;

	error = ext2_find_entry(dir, name, len, &block, &offset);
	if (error)
		return error;

	buf = kmalloc(sb->s_block_size);
	if (!buf)
		return -ENOMEM;
	error = ext2_read_block(sb, block, buf);
	if (error) {
		kfree(buf);
		return error;
	}
	entry = (struct ext2_dir_entry_2 *)(buf + offset);
	ext2_write32((uint8_t *)&entry->inode, 0);
	error = ext2_write_block(sb, block, buf);
	if (!error)
		ext2_touch_dir(dir);
	kfree(buf);
	return error;
}

static int ext2_dir_is_empty(struct inode *dir)
{
	struct super_block *sb = dir->i_sb;
	uint32_t block_index;

	if (ext2_dir_is_indexed(dir))
		return -ENOTSUP;

	for (block_index = 0;
	     (uint64_t)block_index * sb->s_block_size < (uint64_t)dir->i_size;
	     block_index++) {
		uint32_t block = ext2_bmap(dir, block_index, 0);
		uint32_t block_start = block_index * sb->s_block_size;
		uint32_t limit = sb->s_block_size;
		uint32_t offset;
		uint8_t *buf;
		int error;

		if (!block)
			continue;
		if ((uint64_t)block_start + limit > (uint64_t)dir->i_size)
			limit = dir->i_size - block_start;

		buf = kmalloc(sb->s_block_size);
		if (!buf)
			return -ENOMEM;
		error = ext2_read_block(sb, block, buf);
		if (error) {
			kfree(buf);
			return error;
		}

		for (offset = 0; offset < limit;) {
			struct ext2_dir_entry_2 *entry =
				(struct ext2_dir_entry_2 *)(buf + offset);
			uint16_t rec_len =
				ext2_read16((const uint8_t *)&entry->rec_len);

			if (rec_len < 8 || offset + rec_len > limit)
				break;
			if (ext2_read32((const uint8_t *)&entry->inode) &&
			    !(entry->name_len == 1 && entry->name[0] == '.') &&
			    !(entry->name_len == 2 && entry->name[0] == '.' &&
			      entry->name[1] == '.')) {
				kfree(buf);
				return 0;
			}
			offset += rec_len;
		}
		kfree(buf);
	}

	return 1;
}

static void ext2_set_links(struct inode *inode, uint16_t links);

static int ext2_read_entry_inode(struct inode *dir, const char *name, int len,
				 uint32_t *ino)
{
	uint32_t block;
	uint32_t offset;
	uint8_t *buf;
	int error;

	error = ext2_find_entry(dir, name, len, &block, &offset);
	if (error)
		return error;

	buf = kmalloc(dir->i_sb->s_block_size);
	if (!buf)
		return -ENOMEM;
	error = ext2_read_block(dir->i_sb, block, buf);
	if (!error) {
		struct ext2_dir_entry_2 *entry =
			(struct ext2_dir_entry_2 *)(buf + offset);
		*ino = ext2_read32((const uint8_t *)&entry->inode);
	}
	kfree(buf);
	return error;
}

static int ext2_update_entry_inode(struct inode *dir, const char *name, int len,
				   uint32_t ino)
{
	uint32_t block;
	uint32_t offset;
	uint8_t *buf;
	int error;

	error = ext2_find_entry(dir, name, len, &block, &offset);
	if (error)
		return error;

	buf = kmalloc(dir->i_sb->s_block_size);
	if (!buf)
		return -ENOMEM;
	error = ext2_read_block(dir->i_sb, block, buf);
	if (!error) {
		struct ext2_dir_entry_2 *entry =
			(struct ext2_dir_entry_2 *)(buf + offset);
		ext2_write32((uint8_t *)&entry->inode, ino);
		error = ext2_write_block(dir->i_sb, block, buf);
	}
	kfree(buf);
	return error;
}

static int ext2_drop_link(struct inode *inode, uint32_t ino)
{
	int error;

	if (inode->i_nlink > 1) {
		ext2_set_links(inode, inode->i_nlink - 1);
		return 0;
	}

	if (!S_ISLNK(inode->i_mode) || inode->i_nr_block) {
		down(&inode->i_sem);
		error = ext2_truncate(inode, 0);
		up(&inode->i_sem);
		if (error)
			return error;
	}

	error = ext2_free_inode(inode->i_sb, ino);
	if (!error)
		clear_bit(I_Dirty, &inode->i_state);
	return error;
}

static int ext2_lookup(struct inode *dir, char *name, int len,
		       struct inode **res)
{
	uint32_t block;
	uint32_t offset;
	uint32_t ino;
	struct super_block *sb;
	int error;

	*res = NULL;
	if (!dir)
		return -ENOENT;
	if (!S_ISDIR(dir->i_mode)) {
		iput(dir);
		return -ENOENT;
	}
	if (len == 1 && name[0] == '.') {
		*res = dir;
		return 0;
	}

	sb = dir->i_sb;
	error = ext2_find_entry(dir, name, len, &block, &offset);
	if (error) {
		iput(dir);
		return error;
	}

	{
		uint8_t *buf = kmalloc(sb->s_block_size);
		struct ext2_dir_entry_2 *entry;

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
		entry = (struct ext2_dir_entry_2 *)(buf + offset);
		ino = ext2_read32((const uint8_t *)&entry->inode);
		kfree(buf);
	}

	iput(dir);
	*res = iget(sb, ino);
	return 0;
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

static void ext2_set_links(struct inode *inode, uint16_t links)
{
	struct ext2_inode *raw = &EXT2_INODE(inode).raw;

	inode->i_nlink = links;
	ext2_write16((uint8_t *)&raw->i_links_count, links);
	set_bit(I_Dirty, &inode->i_state);
}

static int ext2_create(struct inode *dir, char *name, int len,
		       struct inode **res)
{
	uint32_t block;
	uint32_t offset;
	uint32_t ino;
	struct inode *inode;
	int error;

	*res = NULL;
	if (len > EXT2_NAME_LEN)
		return -ENAMETOOLONG;

	error = ext2_find_entry(dir, name, len, &block, &offset);
	if (!error) {
		iput(dir);
		return -EEXIST;
	}
	if (error != -ENOENT) {
		iput(dir);
		return error;
	}

	error = ext2_alloc_inode(dir->i_sb, MODE_REG, &ino);
	if (error) {
		iput(dir);
		return error;
	}
	inode = iget(dir->i_sb, ino);
	ext2_set_links(inode, 1);
	error = ext2_add_entry(dir, name, len, ino, EXT2_FT_REG_FILE);
	if (error) {
		ext2_free_inode(dir->i_sb, ino);
		clear_bit(I_Dirty, &inode->i_state);
		iput(inode);
		iput(dir);
		return error;
	}

	*res = inode;
	iput(dir);
	return 0;
}

static int ext2_link(struct inode *old_inode, struct inode *dir, char *name,
		     int len)
{
	uint32_t block;
	uint32_t offset;
	int error;

	if (len > EXT2_NAME_LEN || S_ISDIR(old_inode->i_mode) ||
	    old_inode->i_sb != dir->i_sb) {
		iput(dir);
		return len > EXT2_NAME_LEN ? -ENAMETOOLONG : -EPERM;
	}
	if (old_inode->i_nlink >= 0xffff) {
		iput(dir);
		return -EMLINK;
	}

	down(&dir->i_sem);
	error = ext2_find_entry(dir, name, len, &block, &offset);
	if (!error) {
		up(&dir->i_sem);
		iput(dir);
		return -EEXIST;
	}
	if (error != -ENOENT) {
		up(&dir->i_sem);
		iput(dir);
		return error;
	}

	error = ext2_add_entry(dir, name, len, old_inode->i_ino,
			       ext2_file_type(old_inode->i_mode));
	if (!error) {
		ext2_set_links(old_inode, old_inode->i_nlink + 1);
		old_inode->i_ctime = current_time();
		ext2_write32((uint8_t *)&EXT2_INODE(old_inode).raw.i_ctime,
			     old_inode->i_ctime);
	}
	up(&dir->i_sem);
	iput(dir);
	return error;
}

static int ext2_mkdir(struct inode *dir, char *name, int len, int mode)
{
	uint32_t block;
	uint32_t offset;
	uint32_t ino;
	struct inode *inode;
	int error;

	if (len > EXT2_NAME_LEN)
		return -ENAMETOOLONG;
	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.')) {
		iput(dir);
		return -EINVAL;
	}

	down(&dir->i_sem);
	error = ext2_find_entry(dir, name, len, &block, &offset);
	if (!error) {
		up(&dir->i_sem);
		iput(dir);
		return -EEXIST;
	}
	if (error != -ENOENT) {
		up(&dir->i_sem);
		iput(dir);
		return error;
	}

	error = ext2_alloc_inode(dir->i_sb, MODE_DIR | (mode & 0777), &ino);
	if (error) {
		up(&dir->i_sem);
		iput(dir);
		return error;
	}
	inode = iget(dir->i_sb, ino);
	error = ext2_add_entry(inode, ".", 1, ino, EXT2_FT_DIR);
	if (!error)
		error = ext2_add_entry(inode, "..", 2, dir->i_ino, EXT2_FT_DIR);
	if (error) {
		ext2_free_inode(dir->i_sb, ino);
		clear_bit(I_Dirty, &inode->i_state);
		iput(inode);
		up(&dir->i_sem);
		iput(dir);
		return error;
	}
	ext2_set_links(inode, 2);

	error = ext2_add_entry(dir, name, len, ino, EXT2_FT_DIR);
	if (error) {
		ext2_free_inode(dir->i_sb, ino);
		clear_bit(I_Dirty, &inode->i_state);
		iput(inode);
		up(&dir->i_sem);
		iput(dir);
		return error;
	}

	dir->i_nlink++;
	ext2_write16((uint8_t *)&EXT2_INODE(dir).raw.i_links_count,
		     dir->i_nlink);
	set_bit(I_Dirty, &dir->i_state);
	iput(inode);
	up(&dir->i_sem);
	iput(dir);
	return 0;
}

static int ext2_unlink(struct inode *dir, char *name, int len)
{
	uint32_t block;
	uint32_t offset;
	uint32_t ino;
	struct inode *inode;
	int error;

	down(&dir->i_sem);
	error = ext2_find_entry(dir, name, len, &block, &offset);
	if (error) {
		up(&dir->i_sem);
		iput(dir);
		return error;
	}
	ino = 0;
	{
		uint8_t *buf = kmalloc(dir->i_sb->s_block_size);

		if (!buf) {
			up(&dir->i_sem);
			iput(dir);
			return -ENOMEM;
		}
		error = ext2_read_block(dir->i_sb, block, buf);
		if (!error) {
			struct ext2_dir_entry_2 *entry =
				(struct ext2_dir_entry_2 *)(buf + offset);
			ino = ext2_read32((const uint8_t *)&entry->inode);
		}
		kfree(buf);
		if (error) {
			up(&dir->i_sem);
			iput(dir);
			return error;
		}
	}

	inode = iget(dir->i_sb, ino);
	if (S_ISDIR(inode->i_mode)) {
		iput(inode);
		up(&dir->i_sem);
		iput(dir);
		return -EISDIR;
	}

	error = ext2_delete_entry(dir, name, len);
	if (error) {
		iput(inode);
		up(&dir->i_sem);
		iput(dir);
		return error;
	}

	if (inode->i_nlink > 1) {
		ext2_set_links(inode, inode->i_nlink - 1);
	} else {
		if (!S_ISLNK(inode->i_mode) || inode->i_nr_block) {
			down(&inode->i_sem);
			error = ext2_truncate(inode, 0);
			up(&inode->i_sem);
		}
		if (!error)
			error = ext2_free_inode(dir->i_sb, ino);
		clear_bit(I_Dirty, &inode->i_state);
	}

	iput(inode);
	up(&dir->i_sem);
	iput(dir);
	return error;
}

static int ext2_rmdir(struct inode *dir, char *name, int len)
{
	uint32_t block;
	uint32_t offset;
	uint32_t ino;
	struct inode *inode;
	int error;

	down(&dir->i_sem);
	error = ext2_find_entry(dir, name, len, &block, &offset);
	if (error) {
		up(&dir->i_sem);
		iput(dir);
		return error;
	}
	{
		uint8_t *buf = kmalloc(dir->i_sb->s_block_size);

		if (!buf) {
			up(&dir->i_sem);
			iput(dir);
			return -ENOMEM;
		}
		error = ext2_read_block(dir->i_sb, block, buf);
		if (!error) {
			struct ext2_dir_entry_2 *entry =
				(struct ext2_dir_entry_2 *)(buf + offset);
			ino = ext2_read32((const uint8_t *)&entry->inode);
		}
		kfree(buf);
		if (error) {
			up(&dir->i_sem);
			iput(dir);
			return error;
		}
	}

	inode = iget(dir->i_sb, ino);
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		up(&dir->i_sem);
		iput(dir);
		return -ENOTDIR;
	}
	if (inode == dir->i_sb->s_mounted) {
		iput(inode);
		up(&dir->i_sem);
		iput(dir);
		return -EBUSY;
	}
	error = ext2_dir_is_empty(inode);
	if (error <= 0) {
		iput(inode);
		up(&dir->i_sem);
		iput(dir);
		return error ? error : -ENOTEMPTY;
	}

	error = ext2_delete_entry(dir, name, len);
	if (!error) {
		dir->i_nlink--;
		ext2_write16((uint8_t *)&EXT2_INODE(dir).raw.i_links_count,
			     dir->i_nlink);
		set_bit(I_Dirty, &dir->i_state);

		down(&inode->i_sem);
		error = ext2_truncate(inode, 0);
		up(&inode->i_sem);
		if (!error)
			error = ext2_free_inode(dir->i_sb, ino);
		clear_bit(I_Dirty, &inode->i_state);
	}

	iput(inode);
	up(&dir->i_sem);
	iput(dir);
	return error;
}

static int ext2_rename(struct inode *old_dir, char *old_name, int old_len,
		       struct inode *new_dir, char *new_name, int new_len)
{
	static struct semaphore sem = INIT_MUTEX(sem);
	uint32_t old_ino = 0;
	uint32_t new_ino = 0;
	uint32_t block;
	uint32_t offset;
	struct inode *old_inode = NULL;
	struct inode *new_inode = NULL;
	int same_dir = old_dir == new_dir;
	int new_exists = 0;
	int error;

	down(&sem);
	down(&old_dir->i_sem);
	if (!same_dir)
		down(&new_dir->i_sem);

	error = ext2_read_entry_inode(old_dir, old_name, old_len, &old_ino);
	if (error)
		goto out;
	old_inode = iget(old_dir->i_sb, old_ino);

	error = ext2_find_entry(new_dir, new_name, new_len, &block, &offset);
	if (error && error != -ENOENT)
		goto out;
	if (!error) {
		error = ext2_read_entry_inode(new_dir, new_name, new_len,
					      &new_ino);
		if (error)
			goto out;
		/* Renaming a link over itself is a no-op. */
		if (old_ino == new_ino) {
			error = 0;
			goto out;
		}
		new_exists = 1;
		new_inode = iget(new_dir->i_sb, new_ino);
		if (S_ISDIR(old_inode->i_mode) &&
		    !S_ISDIR(new_inode->i_mode)) {
			error = -EISDIR;
			goto out;
		}
		if (!S_ISDIR(old_inode->i_mode) &&
		    S_ISDIR(new_inode->i_mode)) {
			error = -ENOTDIR;
			goto out;
		}
		if (S_ISDIR(new_inode->i_mode)) {
			error = ext2_dir_is_empty(new_inode);
			if (error <= 0) {
				error = error ? error : -ENOTEMPTY;
				goto out;
			}
		}
	} else {
		error = 0;
	}

	error = ext2_delete_entry(old_dir, old_name, old_len);
	if (error)
		goto out;
	if (new_exists) {
		error = ext2_delete_entry(new_dir, new_name, new_len);
		if (error) {
			ext2_add_entry(old_dir, old_name, old_len, old_ino,
				       ext2_file_type(old_inode->i_mode));
			goto out;
		}
	}

	error = ext2_add_entry(new_dir, new_name, new_len, old_ino,
			       ext2_file_type(old_inode->i_mode));
	if (error) {
		ext2_add_entry(old_dir, old_name, old_len, old_ino,
			       ext2_file_type(old_inode->i_mode));
		if (new_exists) {
			ext2_add_entry(new_dir, new_name, new_len, new_ino,
				       ext2_file_type(new_inode->i_mode));
		}
		goto out;
	}

	if (S_ISDIR(old_inode->i_mode)) {
		old_dir->i_nlink--;
		ext2_write16((uint8_t *)&EXT2_INODE(old_dir).raw.i_links_count,
			     old_dir->i_nlink);
		set_bit(I_Dirty, &old_dir->i_state);

		new_dir->i_nlink++;
		ext2_write16((uint8_t *)&EXT2_INODE(new_dir).raw.i_links_count,
			     new_dir->i_nlink);
		set_bit(I_Dirty, &new_dir->i_state);

		if (new_exists) {
			new_dir->i_nlink--;
			ext2_write16((uint8_t *)&EXT2_INODE(new_dir).raw.i_links_count,
				     new_dir->i_nlink);
			set_bit(I_Dirty, &new_dir->i_state);
		}

		if (!same_dir) {
			error = ext2_update_entry_inode(old_inode, "..", 2,
							new_dir->i_ino);
			if (error)
				goto out;
		}
	}

	if (new_exists) {
		error = ext2_drop_link(new_inode, new_ino);
		if (error)
			goto out;
	}

	old_inode->i_ctime = current_time();
	ext2_write32((uint8_t *)&EXT2_INODE(old_inode).raw.i_ctime,
		     old_inode->i_ctime);
	set_bit(I_Dirty, &old_inode->i_state);
	error = 0;
out:
	if (new_inode)
		iput(new_inode);
	if (old_inode)
		iput(old_inode);
	if (!same_dir)
		up(&new_dir->i_sem);
	up(&old_dir->i_sem);
	up(&sem);
	iput(old_dir);
	iput(new_dir);
	return error;
}

static int ext2_mknod(struct inode *dir, char *name, int len, int mode, int rdev)
{
	uint32_t block;
	uint32_t offset;
	uint32_t ino;
	struct inode *inode;
	int error;

	if (len > EXT2_NAME_LEN)
		return -ENAMETOOLONG;

	down(&dir->i_sem);
	error = ext2_find_entry(dir, name, len, &block, &offset);
	if (!error) {
		up(&dir->i_sem);
		iput(dir);
		return -EEXIST;
	}
	if (error != -ENOENT) {
		up(&dir->i_sem);
		iput(dir);
		return error;
	}

	error = ext2_alloc_inode(dir->i_sb, mode, &ino);
	if (error) {
		up(&dir->i_sem);
		iput(dir);
		return error;
	}
	inode = iget(dir->i_sb, ino);
	if (S_ISCHR(mode) || S_ISBLK(mode)) {
		inode->i_rdev = rdev;
		ext2_write32((uint8_t *)&EXT2_INODE(inode).raw.i_block[0], rdev);
	}
	ext2_set_links(inode, 1);
	error = ext2_add_entry(dir, name, len, ino, ext2_file_type(mode));
	if (error) {
		ext2_free_inode(dir->i_sb, ino);
		clear_bit(I_Dirty, &inode->i_state);
		iput(inode);
		up(&dir->i_sem);
		iput(dir);
		return error;
	}

	iput(inode);
	up(&dir->i_sem);
	iput(dir);
	return 0;
}

static int ext2_symlink(struct inode *dir, char *name, int len, char *target)
{
	uint32_t block;
	uint32_t offset;
	uint32_t ino;
	struct inode *inode;
	struct ext2_inode *raw;
	size_t target_len;
	int error;

	if (len > EXT2_NAME_LEN)
		return -ENAMETOOLONG;

	target_len = strlen(target);
	down(&dir->i_sem);
	error = ext2_find_entry(dir, name, len, &block, &offset);
	if (!error) {
		up(&dir->i_sem);
		iput(dir);
		return -EEXIST;
	}
	if (error != -ENOENT) {
		up(&dir->i_sem);
		iput(dir);
		return error;
	}

	error = ext2_alloc_inode(dir->i_sb, MODE_LNK, &ino);
	if (error) {
		up(&dir->i_sem);
		iput(dir);
		return error;
	}
	inode = iget(dir->i_sb, ino);
	raw = &EXT2_INODE(inode).raw;
	inode->i_size = target_len;
	ext2_write32((uint8_t *)&raw->i_size, target_len);

	if (target_len < sizeof(raw->i_block)) {
		memcpy(raw->i_block, target, target_len);
	} else {
		uint32_t physical = ext2_bmap(inode, 0, 1);
		uint8_t *buf;

		if (!physical) {
			error = -ENOSPC;
			goto fail;
		}
		buf = kmalloc(EXT2_BLOCK_SIZE(dir->i_sb));
		if (!buf) {
			error = -ENOMEM;
			goto fail;
		}
		memset(buf, 0, EXT2_BLOCK_SIZE(dir->i_sb));
		memcpy(buf, target, target_len);
		error = ext2_write_block(dir->i_sb, physical, buf);
		kfree(buf);
		if (error)
			goto fail;
	}

	ext2_set_links(inode, 1);
	error = ext2_add_entry(dir, name, len, ino, EXT2_FT_SYMLINK);
	if (error)
		goto fail;
	iput(inode);
	up(&dir->i_sem);
	iput(dir);
	return 0;

fail:
	ext2_free_inode(dir->i_sb, ino);
	clear_bit(I_Dirty, &inode->i_state);
	iput(inode);
	up(&dir->i_sem);
	iput(dir);
	return error;
}

struct inode_operations ext2_dir_iops = {
	.lookup = ext2_lookup,
	.create = ext2_create,
	.rmdir = ext2_rmdir,
	.mkdir = ext2_mkdir,
	.link = ext2_link,
	.unlink = ext2_unlink,
	.rename = ext2_rename,
	.mknod = ext2_mknod,
	.symlink = ext2_symlink,
};

struct file_operations ext2_dir_fops = {
	.readdir = ext2_readdir,
};
