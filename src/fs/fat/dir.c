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

static int fat_lookup(struct inode *dir, char *name, int len,
		      struct inode **res)
{
	struct super_block *sb;
	uint32_t cluster;
	uint32_t offset;
	uint32_t cluster_size;
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
				kfree(buf);
				iput(dir);
				*res = iget(sb, ino);
				return 0;
			}
			fat_lfn_reset(&lfn);
			lfn_pending = 0;
		}

		kfree(buf);
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

struct inode_operations fat_dir_iops = {
	.lookup = fat_lookup,
};

struct file_operations fat_dir_fops = {
	.readdir = fat_readdir,
};
