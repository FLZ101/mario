#include <fs/fs.h>
#include <fs/fat/fat.h>
#include <fs/dirent.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

static int fat_dir_next_cluster(struct inode *dir, uint32_t cluster,
				uint32_t *next)
{
	return fat_next_cluster(dir->i_sb, cluster, next);
}

struct fat_lfn {
	char name[256];
	int len;
	uint8_t checksum;
	int have_last;
	int next_seq;
};

static const uint8_t fat_lfn_offsets[] = {
	1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
};

static uint8_t fat_short_name_checksum(const uint8_t *entry)
{
	uint8_t sum = 0;
	int i;

	for (i = 0; i < 11; i++)
		sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + entry[i];
	return sum;
}

static void fat_lfn_reset(struct fat_lfn *lfn)
{
	memset(lfn, 0, sizeof(*lfn));
}

/*
 * Append one LFN directory entry to the pending name.  LFN entries appear in
 * reverse order (the last piece first), so the sequence number is used to
 * place each piece directly at its final position in the name buffer.
 */
static int fat_lfn_add(struct fat_lfn *lfn, const uint8_t *entry)
{
	uint8_t seq = entry[0] & 0x1f;
	uint8_t checksum = entry[13];
	int base;
	int i;
	int count;

	if (entry[11] != FAT_ATTR_LFN || entry[12] != 0 || seq == 0 ||
	    seq > 20)
		return -EINVAL;

	if (!lfn->have_last) {
		if (!(entry[0] & 0x40))
			return -EINVAL;
		lfn->have_last = 1;
		lfn->next_seq = seq - 1;
		lfn->checksum = checksum;
	} else {
		if (seq != lfn->next_seq || checksum != lfn->checksum)
			return -EINVAL;
		lfn->next_seq--;
	}

	base = (seq - 1) * 13;
	if (base >= (int)sizeof(lfn->name) - 1)
		return -EINVAL;
	count = 0;
	for (i = 0; i < 13; i++) {
		uint16_t value = fat_read16(entry + fat_lfn_offsets[i]);
		char ch;

		if (value == 0 || value == 0xffff)
			break;
		if (base + count >= (int)sizeof(lfn->name) - 1)
			break;
		ch = value < 0x80 ? (char)value : '?';
		lfn->name[base + count++] = ch;
	}
	if (base + count > lfn->len)
		lfn->len = base + count;
	return 0;
}

static int fat_lfn_finish(struct fat_lfn *lfn, const uint8_t *short_entry)
{
	if (!lfn->have_last || lfn->next_seq != 0)
		return -EINVAL;
	if (fat_short_name_checksum(short_entry) != lfn->checksum)
		return -EINVAL;
	lfn->name[lfn->len] = '\0';
	return lfn->len > 0 ? 0 : -EINVAL;
}

static int fat_lfn_name_match(const char *name, int len, const char *lfn)
{
	int i;

	if (len != (int)strlen(lfn))
		return 0;
	for (i = 0; i < len; i++) {
		char a = name[i];
		char b = lfn[i];

		if (a >= 'A' && a <= 'Z')
			a = a - 'A' + 'a';
		if (b >= 'A' && b <= 'Z')
			b = b - 'A' + 'a';
		if (a != b)
			return 0;
	}
	return 1;
}

/*
 * Return the mode of a FAT directory entry for use by lookup/readdir.  Only
 * directories and regular files are supported in the read-only milestone.
 */
static umode_t fat_entry_mode(const uint8_t *entry)
{
	if (entry[11] & FAT_ATTR_DIRECTORY)
		return S_IFDIR | 0777;
	return S_IFREG | 0777;
}

static int fat_read_dir_cluster(struct inode *dir, uint32_t cluster_index,
				uint32_t *cluster, uint8_t **buf)
{
	struct super_block *sb = dir->i_sb;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(sb);
	int error;

	if (cluster_index >= FAT_SB(sb).total_clusters)
		return -EIO;
	*cluster = fat_cluster_at(dir, cluster_index);
	if (!*cluster)
		return -ENOENT;
	*buf = kmalloc(cluster_size);
	if (!*buf)
		return -ENOMEM;
	error = fat_read_cluster(sb, *cluster, *buf);
	if (error) {
		kfree(*buf);
		*buf = NULL;
		return error;
	}
	return 0;
}

static int fat_dir_find_entry(struct inode *dir, const char *name, int len,
			      uint32_t *res_cluster, uint32_t *res_offset)
{
	struct super_block *sb = dir->i_sb;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(sb);
	struct fat_lfn lfn;
	int lfn_pending = 0;
	uint32_t cluster_index = 0;

	fat_lfn_reset(&lfn);
	for (;;) {
		uint32_t cluster;
		uint8_t *buf;
		uint32_t offset;
		int error;

		error = fat_read_dir_cluster(dir, cluster_index, &cluster, &buf);
		if (error)
			return error;

		for (offset = 0; offset < cluster_size; offset += FAT_ENTRY_SIZE) {
			const uint8_t *entry = buf + offset;
			uint8_t attributes = entry[11];
			int lfn_ok;

			if (entry[0] == 0x00) {
				kfree(buf);
				return -ENOENT;
			}
			if (entry[0] == 0xe5) {
				fat_lfn_reset(&lfn);
				lfn_pending = 0;
				continue;
			}
			if (attributes == FAT_ATTR_LFN) {
				if (!fat_lfn_add(&lfn, entry))
					lfn_pending = 1;
				else {
					fat_lfn_reset(&lfn);
					lfn_pending = 0;
				}
				continue;
			}
			if (attributes & FAT_ATTR_VOLUME_ID) {
				fat_lfn_reset(&lfn);
				lfn_pending = 0;
				continue;
			}

			lfn_ok = lfn_pending && !fat_lfn_finish(&lfn, entry);
			if ((lfn_ok && fat_lfn_name_match(name, len, lfn.name)) ||
			    fat_short_name_match(name, len, entry)) {
				*res_cluster = cluster;
				*res_offset = offset;
				kfree(buf);
				return 0;
			}
			fat_lfn_reset(&lfn);
			lfn_pending = 0;
		}

		kfree(buf);
		cluster_index++;
	}
}

static int fat_dir_short_name_exists(struct inode *dir,
				     const uint8_t short_name[11],
				     uint32_t skip_cluster, uint32_t skip_offset)
{
	uint32_t cluster_index = 0;

	for (;;) {
		uint32_t cluster;
		uint8_t *buf;
		uint32_t offset;
		int error;

		error = fat_read_dir_cluster(dir, cluster_index, &cluster, &buf);
		if (error == -ENOENT)
			return 0;
		if (error)
			return error;

		for (offset = 0; offset < FAT_CLUSTER_SIZE(dir->i_sb);
		     offset += FAT_ENTRY_SIZE) {
			const uint8_t *entry = buf + offset;

			if (entry[0] == 0x00) {
				kfree(buf);
				return 0;
			}
			if (entry[0] == 0xe5 || entry[11] == FAT_ATTR_LFN ||
			    (entry[11] & FAT_ATTR_VOLUME_ID))
				continue;
			if (cluster == skip_cluster && offset == skip_offset)
				continue;
			if (fat_short_name_equal(entry, short_name)) {
				kfree(buf);
				return 1;
			}
		}

		kfree(buf);
		cluster_index++;
	}
}

static int fat_dir_find_free(struct inode *dir, uint32_t *res_cluster,
			     uint32_t *res_offset, int *end_marker)
{
	struct super_block *sb = dir->i_sb;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(sb);
	uint32_t cluster_index = 0;
	uint32_t previous = 0;

	for (;;) {
		uint32_t cluster;
		uint8_t *buf;
		uint32_t offset;
		int error;

		error = fat_read_dir_cluster(dir, cluster_index, &cluster, &buf);
		if (error == -ENOENT)
			goto expand;
		if (error)
			return error;

		for (offset = 0; offset < cluster_size; offset += FAT_ENTRY_SIZE) {
			uint8_t *entry = buf + offset;

			if (entry[0] == 0x00) {
				*res_cluster = cluster;
				*res_offset = offset;
				*end_marker = 1;
				kfree(buf);
				return 0;
			}
			if (entry[0] == 0xe5) {
				*res_cluster = cluster;
				*res_offset = offset;
				*end_marker = 0;
				kfree(buf);
				return 0;
			}
		}

		previous = cluster;
		kfree(buf);
		cluster_index++;
	}

expand:
	{
		uint32_t cluster;
		uint8_t *buf;
		int error;

		error = fat_alloc_cluster(sb, previous, &cluster);
		if (error)
			return error;
		buf = kmalloc(cluster_size);
		if (!buf)
			return -ENOMEM;
		memset(buf, 0, cluster_size);
		error = fat_write_cluster(sb, cluster, buf);
		kfree(buf);
		if (error) {
			fat_free_cluster_chain(sb, cluster);
			return error;
		}
		*res_cluster = cluster;
		*res_offset = 0;
		*end_marker = 1;
		return 0;
	}
}

static int fat_write_entry_at(struct inode *dir, uint32_t cluster,
			      uint32_t offset, const uint8_t *entry,
			      int end_marker)
{
	struct super_block *sb = dir->i_sb;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(sb);
	uint8_t *buf;
	int error;

	buf = kmalloc(cluster_size);
	if (!buf)
		return -ENOMEM;
	error = fat_read_cluster(sb, cluster, buf);
	if (error)
		goto out;
	memcpy(buf + offset, entry, FAT_ENTRY_SIZE);
	if (end_marker && offset + FAT_ENTRY_SIZE < cluster_size)
		buf[offset + FAT_ENTRY_SIZE] = 0x00;
	error = fat_write_cluster(sb, cluster, buf);
out:
	kfree(buf);
	return error;
}

static int fat_add_entry(struct inode *dir, const uint8_t short_name[11],
			 uint8_t attributes, uint32_t start_cluster,
			 uint32_t size, unsigned long *ino_out)
{
	uint32_t cluster;
	uint32_t offset;
	uint8_t entry[FAT_ENTRY_SIZE];
	int end_marker;
	int error;

	error = fat_dir_short_name_exists(dir, short_name, 0, 0);
	if (error)
		return error == 1 ? -EEXIST : error;

	error = fat_dir_find_free(dir, &cluster, &offset, &end_marker);
	if (error)
		return error;

	fat_make_short_entry(entry, short_name, attributes, start_cluster, size);
	error = fat_write_entry_at(dir, cluster, offset, entry, end_marker);
	if (error)
		return error;

	*ino_out = fat_entry_ino(dir->i_sb, cluster, offset);
	inode_invalidate(dir->i_sb, *ino_out);
	return 0;
}

static int fat_delete_entry_at(struct inode *dir, uint32_t cluster,
			       uint32_t offset)
{
	struct super_block *sb = dir->i_sb;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(sb);
	uint8_t *buf;
	int error;

	buf = kmalloc(cluster_size);
	if (!buf)
		return -ENOMEM;
	error = fat_read_cluster(sb, cluster, buf);
	if (error)
		goto out;

	while (offset >= FAT_ENTRY_SIZE && buf[offset - FAT_ENTRY_SIZE] != 0xe5 &&
	       buf[offset - FAT_ENTRY_SIZE + 11] == FAT_ATTR_LFN)
		offset -= FAT_ENTRY_SIZE;

	while (offset < cluster_size &&
	       buf[offset + 11] == FAT_ATTR_LFN) {
		buf[offset] = 0xe5;
		offset += FAT_ENTRY_SIZE;
	}
	buf[offset] = 0xe5;
	error = fat_write_cluster(sb, cluster, buf);
out:
	kfree(buf);
	return error;
}

static int fat_read_entry_at(struct inode *dir, uint32_t cluster,
			     uint32_t offset, uint8_t entry[FAT_ENTRY_SIZE])
{
	struct super_block *sb = dir->i_sb;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(sb);
	uint8_t *buf;
	int error;

	buf = kmalloc(cluster_size);
	if (!buf)
		return -ENOMEM;
	error = fat_read_cluster(sb, cluster, buf);
	if (!error)
		memcpy(entry, buf + offset, FAT_ENTRY_SIZE);
	kfree(buf);
	return error;
}

static int fat_dir_is_empty(struct inode *dir)
{
	uint32_t cluster_index = 0;

	for (;;) {
		uint32_t cluster;
		uint8_t *buf;
		uint32_t offset;
		int error;

		error = fat_read_dir_cluster(dir, cluster_index, &cluster, &buf);
		if (error == -ENOENT)
			return 1;
		if (error)
			return error;

		for (offset = 0; offset < FAT_CLUSTER_SIZE(dir->i_sb);
		     offset += FAT_ENTRY_SIZE) {
			const uint8_t *entry = buf + offset;

			if (entry[0] == 0x00) {
				kfree(buf);
				return 1;
			}
			if (entry[0] == 0xe5 || entry[11] == FAT_ATTR_LFN ||
			    (entry[11] & FAT_ATTR_VOLUME_ID))
				continue;
			if ((entry[0] == '.' && (entry[1] == ' ' ||
			    (entry[1] == '.' && entry[2] == ' '))))
				continue;
			kfree(buf);
			return 0;
		}

		kfree(buf);
		cluster_index++;
	}
}

static int fat_lookup(struct inode *dir, char *name, int len,
		      struct inode **res)
{
	struct super_block *sb;
	uint32_t cluster;
	uint32_t offset;
	uint32_t cluster_size;
	uint32_t cluster_index = 0;
	uint8_t *buf;
	struct fat_lfn lfn;
	int lfn_pending;
	int error;

	*res = NULL;
	if (!dir)
		return -ENOENT;
	if (!S_ISDIR(dir->i_mode)) {
		iput(dir);
		return -ENOTDIR;
	}
	if (len == 1 && name[0] == '.') {
		*res = dir;
		return 0;
	}
	if (len == 2 && name[0] == '.' && name[1] == '.') {
		struct super_block *sb = dir->i_sb;
		uint32_t parent_ino = FAT_INODE(dir).parent_ino;

		if (!parent_ino) {
			iput(dir);
			return -ENOENT;
		}
		iput(dir);
		*res = iget(sb, parent_ino);
		return *res ? 0 : -ENOMEM;
	}

	sb = dir->i_sb;
	cluster_size = FAT_CLUSTER_SIZE(sb);
	cluster = FAT_INODE(dir).start_cluster;
	lfn_pending = 0;

	for (;;) {
		uint32_t next;

		buf = kmalloc(cluster_size);
		if (!buf) {
			iput(dir);
			return -ENOMEM;
		}
		error = fat_read_cluster(sb, cluster, buf);
		if (error) {
			kfree(buf);
			iput(dir);
			return error;
		}

		for (offset = 0; offset < cluster_size; offset += FAT_ENTRY_SIZE) {
			const uint8_t *entry = buf + offset;
			uint8_t attributes = entry[11];
			int lfn_ok;

			if (entry[0] == 0x00) {
				kfree(buf);
				iput(dir);
				return -ENOENT;
			}
			if (entry[0] == 0xe5) {
				fat_lfn_reset(&lfn);
				lfn_pending = 0;
				continue;
			}
			if (attributes == FAT_ATTR_LFN) {
				if (!fat_lfn_add(&lfn, entry))
					lfn_pending = 1;
				else {
					fat_lfn_reset(&lfn);
					lfn_pending = 0;
				}
				continue;
			}
			if (attributes & FAT_ATTR_VOLUME_ID) {
				fat_lfn_reset(&lfn);
				lfn_pending = 0;
				continue;
			}

			lfn_ok = lfn_pending && !fat_lfn_finish(&lfn, entry);
			if ((lfn_ok && fat_lfn_name_match(name, len, lfn.name)) ||
			    fat_short_name_match(name, len, entry)) {
				unsigned long ino;

				ino = fat_entry_ino(sb, cluster, offset);
				*res = iget(sb, ino);
				if (*res && S_ISDIR((*res)->i_mode))
					FAT_INODE(*res).parent_ino = dir->i_ino;
				kfree(buf);
				iput(dir);
				return *res ? 0 : -ENOMEM;
			}
			fat_lfn_reset(&lfn);
			lfn_pending = 0;
		}

		kfree(buf);
		if (cluster_index >= FAT_SB(sb).total_clusters) {
			iput(dir);
			return -EIO;
		}
		cluster_index++;
		error = fat_dir_next_cluster(dir, cluster, &next);
		if (error == -ENOENT) {
			iput(dir);
			return -ENOENT;
		}
		if (error) {
			iput(dir);
			return error;
		}
		cluster = next;
	}
}

static int fat_readdir(struct inode *dir, struct file *file, void *dirent,
		       filldir_t filldir)
{
	struct super_block *sb = dir->i_sb;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(sb);
	uint64_t logical = file->f_pos;
	struct fat_lfn lfn;
	int lfn_pending = 0;

	fat_lfn_reset(&lfn);

	for (;;) {
		uint32_t cluster_index = logical / cluster_size;
		uint32_t offset = logical % cluster_size;
		uint32_t cluster;
		uint32_t next;
		uint8_t *buf;
		int error;

		cluster = fat_cluster_at(dir, cluster_index);
		if (!cluster)
			return 0;
		if (cluster_index >= FAT_SB(sb).total_clusters)
			return -EIO;

		buf = kmalloc(cluster_size);
		if (!buf)
			return -ENOMEM;
		error = fat_read_cluster(sb, cluster, buf);
		if (error) {
			kfree(buf);
			return error;
		}

		for (; offset < cluster_size; offset += FAT_ENTRY_SIZE) {
			uint64_t entry_logical =
				(uint64_t)cluster_index * cluster_size + offset;
			const uint8_t *entry = buf + offset;
			uint8_t attributes = entry[11];
			char short_name[13];
			const char *emit_name;
			int lfn_ok;
			int name_len;
			umode_t mode;
			unsigned long ino;

			if (entry[0] == 0x00) {
				file->f_pos = entry_logical;
				kfree(buf);
				return 0;
			}
			if (entry[0] == 0xe5) {
				file->f_pos = entry_logical + FAT_ENTRY_SIZE;
				fat_lfn_reset(&lfn);
				lfn_pending = 0;
				continue;
			}
			if (attributes == FAT_ATTR_LFN) {
				file->f_pos = entry_logical + FAT_ENTRY_SIZE;
				if (!fat_lfn_add(&lfn, entry))
					lfn_pending = 1;
				else {
					fat_lfn_reset(&lfn);
					lfn_pending = 0;
				}
				continue;
			}
			if (attributes & FAT_ATTR_VOLUME_ID) {
				file->f_pos = entry_logical + FAT_ENTRY_SIZE;
				fat_lfn_reset(&lfn);
				lfn_pending = 0;
				continue;
			}

			lfn_ok = lfn_pending && !fat_lfn_finish(&lfn, entry);
			if (lfn_ok) {
				emit_name = lfn.name;
				name_len = lfn.len;
			} else {
				fat_short_name_from_entry(entry, short_name);
				emit_name = short_name;
				name_len = strlen(short_name);
			}
			mode = fat_entry_mode(entry);
			ino = fat_entry_ino(sb, cluster, offset);
			if (filldir(dirent, (char *)emit_name, name_len,
				    entry_logical + FAT_ENTRY_SIZE,
				    ino, IFTODT(mode))) {
				file->f_pos = entry_logical + FAT_ENTRY_SIZE;
				kfree(buf);
				return 0;
			}
			file->f_pos = entry_logical + FAT_ENTRY_SIZE;
			fat_lfn_reset(&lfn);
			lfn_pending = 0;
		}

		kfree(buf);
		error = fat_dir_next_cluster(dir, cluster, &next);
		if (error)
			return error == -ENOENT ? 0 : error;
		cluster_index++;
		logical = (uint64_t)cluster_index * cluster_size;
	}
}

static int fat_create(struct inode *dir, char *name, int len,
		      struct inode **res)
{
	uint8_t short_name[11];
	unsigned long ino;
	struct inode *inode;
	int error;

	*res = NULL;
	if (FAT_SB(dir->i_sb).readonly) {
		iput(dir);
		return -EROFS;
	}

	error = fat_name_to_short(name, len, short_name);
	if (error) {
		iput(dir);
		return error;
	}
	error = fat_add_entry(dir, short_name, FAT_ATTR_ARCHIVE, 0, 0, &ino);
	if (error) {
		iput(dir);
		return error;
	}

	inode = iget(dir->i_sb, ino);
	iput(dir);
	if (!inode)
		return -ENOMEM;
	*res = inode;
	return 0;
}

static int fat_mkdir(struct inode *dir, char *name, int len)
{
	uint8_t short_name[11];
	uint8_t dot_name[11];
	uint8_t dotdot_name[11];
	uint8_t entry[FAT_ENTRY_SIZE];
	uint8_t *buf;
	uint32_t cluster;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(dir->i_sb);
	unsigned long ino;
	struct inode *inode;
	int error = 0;

	if (FAT_SB(dir->i_sb).readonly) {
		iput(dir);
		return -EROFS;
	}
	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.')) {
		iput(dir);
		return -EINVAL;
	}

	error = fat_name_to_short(name, len, short_name);
	if (error) {
		iput(dir);
		return error;
	}

	down(&dir->i_sem);
	error = fat_alloc_cluster(dir->i_sb, 0, &cluster);
	if (error)
		goto out;

	memset(dot_name, ' ', sizeof(dot_name));
	dot_name[0] = '.';
	memset(dotdot_name, ' ', sizeof(dotdot_name));
	dotdot_name[0] = '.';
	dotdot_name[1] = '.';

	buf = kmalloc(cluster_size);
	if (!buf) {
		error = -ENOMEM;
		fat_free_cluster_chain(dir->i_sb, cluster);
		goto out;
	}
	memset(buf, 0, cluster_size);
	fat_make_short_entry(entry, dot_name, FAT_ATTR_DIRECTORY, cluster, 0);
	memcpy(buf, entry, FAT_ENTRY_SIZE);
	fat_make_short_entry(entry, dotdot_name, FAT_ATTR_DIRECTORY,
			     FAT_INODE(dir).start_cluster, 0);
	memcpy(buf + FAT_ENTRY_SIZE, entry, FAT_ENTRY_SIZE);
	error = fat_write_cluster(dir->i_sb, cluster, buf);
	kfree(buf);
	if (error) {
		fat_free_cluster_chain(dir->i_sb, cluster);
		goto out;
	}

	error = fat_add_entry(dir, short_name, FAT_ATTR_DIRECTORY, cluster, 0,
			      &ino);
	if (error) {
		fat_free_cluster_chain(dir->i_sb, cluster);
		goto out;
	}

	inode = iget(dir->i_sb, ino);
	if (inode) {
		FAT_INODE(inode).parent_ino = dir->i_ino;
		iput(inode);
	}
out:
	up(&dir->i_sem);
	iput(dir);
	return error;
}

static int fat_unlink(struct inode *dir, char *name, int len)
{
	uint32_t cluster;
	uint32_t offset;
	uint8_t entry[FAT_ENTRY_SIZE];
	uint32_t start_cluster;
	int error;

	if (FAT_SB(dir->i_sb).readonly) {
		iput(dir);
		return -EROFS;
	}

	down(&dir->i_sem);
	error = fat_dir_find_entry(dir, name, len, &cluster, &offset);
	if (error)
		goto out;
	error = fat_read_entry_at(dir, cluster, offset, entry);
	if (error)
		goto out;
	if (entry[11] & FAT_ATTR_DIRECTORY) {
		error = -EISDIR;
		goto out;
	}
	start_cluster = fat_entry_first_cluster(entry);
	error = fat_delete_entry_at(dir, cluster, offset);
	if (error)
		goto out;
	inode_invalidate(dir->i_sb,
			 fat_entry_ino(dir->i_sb, cluster, offset));
	error = fat_free_cluster_chain(dir->i_sb, start_cluster);
out:
	up(&dir->i_sem);
	iput(dir);
	return error;
}

static int fat_rmdir(struct inode *dir, char *name, int len)
{
	uint32_t cluster;
	uint32_t offset;
	uint8_t entry[FAT_ENTRY_SIZE];
	uint32_t start_cluster;
	unsigned long ino;
	struct inode *inode = NULL;
	int error;

	if (FAT_SB(dir->i_sb).readonly) {
		iput(dir);
		return -EROFS;
	}
	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.')) {
		iput(dir);
		return -EINVAL;
	}

	down(&dir->i_sem);
	error = fat_dir_find_entry(dir, name, len, &cluster, &offset);
	if (error)
		goto out;
	error = fat_read_entry_at(dir, cluster, offset, entry);
	if (error)
		goto out;
	if (!(entry[11] & FAT_ATTR_DIRECTORY)) {
		error = -ENOTDIR;
		goto out;
	}

	ino = fat_entry_ino(dir->i_sb, cluster, offset);
	if (ino == FAT_ROOT_INO) {
		error = -EBUSY;
		goto out;
	}
	inode = iget(dir->i_sb, ino);
	if (!inode) {
		error = -ENOMEM;
		goto out;
	}
	error = fat_dir_is_empty(inode);
	if (error <= 0) {
		error = error ? error : -ENOTEMPTY;
		goto out;
	}

	start_cluster = fat_entry_first_cluster(entry);
	error = fat_delete_entry_at(dir, cluster, offset);
	if (!error)
		error = fat_free_cluster_chain(dir->i_sb, start_cluster);
	if (!error)
		inode_invalidate(dir->i_sb, ino);
out:
	if (inode)
		iput(inode);
	up(&dir->i_sem);
	iput(dir);
	return error;
}

static int fat_rename(struct inode *old_dir, char *old_name, int old_len,
		      struct inode *new_dir, char *new_name, int new_len)
{
	uint8_t short_name[11];
	uint32_t cluster;
	uint32_t offset;
	uint32_t new_cluster;
	uint32_t new_offset;
	uint8_t *buf;
	uint32_t cluster_size;
	int error;

	if (old_dir != new_dir) {
		iput(old_dir);
		iput(new_dir);
		return -EXDEV;
	}
	if (FAT_SB(old_dir->i_sb).readonly) {
		iput(old_dir);
		iput(new_dir);
		return -EROFS;
	}
	error = fat_name_to_short(new_name, new_len, short_name);
	if (error) {
		iput(old_dir);
		iput(new_dir);
		return error;
	}

	down(&old_dir->i_sem);
	error = fat_dir_find_entry(old_dir, old_name, old_len,
				   &cluster, &offset);
	if (error)
		goto out;
	error = fat_dir_find_entry(new_dir, new_name, new_len,
				   &new_cluster, &new_offset);
	if (!error && !(cluster == new_cluster && offset == new_offset)) {
		error = -EEXIST;
		goto out;
	}
	if (error && error != -ENOENT)
		goto out;
	error = fat_dir_short_name_exists(old_dir, short_name,
					  cluster, offset);
	if (error) {
		error = error == 1 ? -EEXIST : error;
		goto out;
	}

	cluster_size = FAT_CLUSTER_SIZE(old_dir->i_sb);
	buf = kmalloc(cluster_size);
	if (!buf) {
		error = -ENOMEM;
		goto out;
	}
	error = fat_read_cluster(old_dir->i_sb, cluster, buf);
	if (!error) {
		memcpy(buf + offset, short_name, 11);
		error = fat_write_cluster(old_dir->i_sb, cluster, buf);
	}
	kfree(buf);
out:
	up(&old_dir->i_sem);
	iput(old_dir);
	iput(new_dir);
	return error;
}

struct inode_operations fat_dir_iops = {
	.lookup = fat_lookup,
	.create = fat_create,
	.mkdir = fat_mkdir,
	.unlink = fat_unlink,
	.rmdir = fat_rmdir,
	.rename = fat_rename,
};

struct file_operations fat_dir_fops = {
	.readdir = fat_readdir,
};
