#include <fs/fs.h>
#include <fs/fat/fat.h>
#include <fs/dirent.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

extern uint64_t current_time(void);

static int fat_dir_next_cluster(struct inode *dir, uint32_t cluster,
				uint32_t *next)
{
	return fat_next_cluster(dir->i_sb, cluster, next);
}

static uint32_t fat_dir_entries_per_cluster(struct super_block *sb)
{
	return FAT_CLUSTER_SIZE(sb) / FAT_ENTRY_SIZE;
}

static int fat_dir_last_cluster(struct inode *dir, uint32_t *last,
				uint32_t *previous)
{
	uint32_t cluster = FAT_INODE(dir).start_cluster;
	uint32_t prev = 0;
	uint32_t seen = 0;

	if (!cluster)
		return -ENOENT;
	while (cluster && seen++ < FAT_SB(dir->i_sb).total_clusters) {
		uint32_t next;
		int error;

		prev = cluster;
		error = fat_dir_next_cluster(dir, cluster, &next);
		if (error == -ENOENT)
			break;
		if (error)
			return error;
		cluster = next;
	}
	if (!prev)
		return -ENOENT;
	*last = prev;
	if (previous)
		*previous = prev;
	return 0;
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

/*
 * Find a run of @slots free directory entries.  Runs that fit in one existing
 * cluster are preferred; otherwise the directory is extended with zeroed
 * clusters and the run is placed at the start of the newly allocated region.
 */
static int fat_dir_find_free_run(struct inode *dir, uint32_t slots,
				 uint32_t *res_cluster, uint32_t *res_offset,
				 int *end_marker)
{
	struct super_block *sb = dir->i_sb;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(sb);
	uint32_t entries_per_cluster = fat_dir_entries_per_cluster(sb);
	uint32_t cluster_index = 0;

	*end_marker = 0;
	if (slots <= entries_per_cluster) {
		for (;;) {
			uint32_t cluster;
			uint8_t *buf;
			uint32_t offset;
			uint32_t run = 0;
			uint32_t run_start = 0;
			int run_zero = 0;
			int error;

			error = fat_read_dir_cluster(dir, cluster_index,
						     &cluster, &buf);
			if (error == -ENOENT)
				break;
			if (error)
				return error;

			for (offset = 0; offset < cluster_size;
			     offset += FAT_ENTRY_SIZE) {
				uint8_t first = buf[offset];

				if (first == 0xe5 || first == 0x00) {
					if (!run)
						run_start = offset;
					run++;
					run_zero = first == 0x00;
					if (run == slots) {
						*res_cluster = cluster;
						*res_offset = run_start;
						*end_marker = run_zero;
						kfree(buf);
						return 0;
					}
					continue;
				}
				run = 0;
				run_zero = 0;
			}

			kfree(buf);
			cluster_index++;
		}
	}

	/* Extend the directory with enough zeroed clusters for the run. */
	{
		uint32_t last;
		uint32_t previous = 0;
		uint32_t need = (slots + entries_per_cluster - 1) /
				entries_per_cluster;
		uint32_t first_new = 0;
		uint32_t i;
		uint8_t *buf;
		int error;

		error = fat_dir_last_cluster(dir, &last, &previous);
		if (error && error != -ENOENT)
			return error;
		if (!error)
			previous = last;

		buf = kmalloc(cluster_size);
		if (!buf)
			return -ENOMEM;
		memset(buf, 0, cluster_size);

		for (i = 0; i < need; i++) {
			uint32_t cluster;

			error = fat_alloc_cluster(sb, previous, &cluster);
			if (error)
				goto out;
			if (!first_new)
				first_new = cluster;
			error = fat_write_cluster(sb, cluster, buf);
			if (error) {
				fat_free_cluster_chain(sb, first_new);
				goto out;
			}
			previous = cluster;
		}
		kfree(buf);

		*res_cluster = first_new;
		*res_offset = 0;
		*end_marker = slots * FAT_ENTRY_SIZE < need * cluster_size;
		return 0;
out:
		kfree(buf);
		return error;
	}
}

/*
 * Clear every end-of-directory marker (a zero first byte) that precedes
 * @target_cluster/@target_offset.  A multi-slot LFN run can be placed after
 * what used to be the directory's final slot, so without this the directory
 * scan would still stop at the old marker and never reach the new entries.
 */
static int fat_dir_clear_end_markers_before(struct inode *dir,
					    uint32_t target_cluster,
					    uint32_t target_offset)
{
	struct super_block *sb = dir->i_sb;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(sb);
	uint32_t cur_cluster = FAT_INODE(dir).start_cluster;
	uint32_t cur_offset = 0;

	while (cur_cluster) {
		uint8_t *buf;
		uint32_t next = 0;
		int changed = 0;
		int error;

		buf = kmalloc(cluster_size);
		if (!buf)
			return -ENOMEM;
		error = fat_read_cluster(sb, cur_cluster, buf);
		if (error) {
			kfree(buf);
			return error;
		}

		while (cur_offset < cluster_size) {
			if (cur_cluster == target_cluster &&
			    cur_offset == target_offset) {
				kfree(buf);
				return 0;
			}
			if (buf[cur_offset] == 0x00) {
				buf[cur_offset] = 0xe5;
				changed = 1;
			}
			cur_offset += FAT_ENTRY_SIZE;
		}

		if (changed) {
			error = fat_write_cluster(sb, cur_cluster, buf);
			if (error) {
				kfree(buf);
				return error;
			}
		}
		kfree(buf);

		error = fat_dir_next_cluster(dir, cur_cluster, &next);
		if (error)
			return error;
		cur_cluster = next;
		cur_offset = 0;
	}

	return -ENOENT;
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

static int fat_char_for_short(char c)
{
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 'A';
	if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
	    c == '_' || c == '-' || c == '!' || c == '#' || c == '$' ||
	    c == '%' || c == '&' || c == '\'' || c == '(' || c == ')' ||
	    c == '+' || c == ',' || c == ';' || c == '=' || c == '@' ||
	    c == '[' || c == ']' || c == '^' || c == '`' || c == '{' ||
	    c == '}' || c == '~')
		return c;
	return '_';
}

static int fat_short_alias(struct inode *dir, const char *name, int len,
			   uint8_t short_name[11])
{
	uint8_t base[16];
	uint8_t ext[4];
	int last_dot = -1;
	int base_len = 0;
	int ext_len = 0;
	int i;
	int n;

	for (i = 0; i < len; i++)
		if (name[i] == '.')
			last_dot = i;

	if (last_dot > 0 && last_dot < len - 1) {
		base_len = last_dot;
		ext_len = len - last_dot - 1;
	} else {
		base_len = len;
	}

	for (i = 0; i < base_len && i < 6; i++) {
		base[i] = fat_char_for_short(name[i]);
	}
	for (; i < (int)sizeof(base); i++)
		base[i] = ' ';
	if (base[0] == ' ')
		base[0] = '_';

	for (i = 0; i < ext_len && i < 3; i++)
		ext[i] = fat_char_for_short(name[last_dot + 1 + i]);
	for (; i < 4; i++)
		ext[i] = ' ';

	for (n = 1; n < 10000; n++) {
		char digits[5];
		int value = n;
		int digits_len = 0;
		int prefix = 7 - digits_len;

		do {
			digits[digits_len++] = '0' + value % 10;
			value /= 10;
		} while (value);
		/* Reverse the decimal string. */
		for (i = 0; i < digits_len / 2; i++) {
			char tmp = digits[i];

			digits[i] = digits[digits_len - 1 - i];
			digits[digits_len - 1 - i] = tmp;
		}
		prefix = 7 - digits_len;
		if (prefix > 6)
			prefix = 6;
		if (prefix < 1)
			prefix = 1;

		memset(short_name, ' ', 11);
		memcpy(short_name, base, prefix);
		short_name[prefix] = '~';
		memcpy(short_name + prefix + 1, digits, digits_len);
		memcpy(short_name + 8, ext, 3);

		i = fat_dir_short_name_exists(dir, short_name, 0, 0);
		if (!i)
			return 0;
		if (i < 0)
			return i;
	}
	return -EEXIST;
}

static void fat_fill_lfn_entry(uint8_t *entry, uint8_t seq,
			       uint8_t checksum, const char *name, int len,
			       int base)
{
	int i;

	memset(entry, 0xff, FAT_ENTRY_SIZE);
	entry[0] = seq;
	entry[11] = FAT_ATTR_LFN;
	entry[12] = 0;
	entry[13] = checksum;
	entry[26] = 0;

	for (i = 0; i < 13; i++) {
		uint16_t ch;

		if (base + i < len)
			ch = (uint8_t)name[base + i];
		else
			ch = 0xffff;
		fat_write16(entry + fat_lfn_offsets[i], ch);
	}
}

static int fat_add_entry_name(struct inode *dir, const char *name, int len,
			      uint8_t attributes, uint32_t start_cluster,
			      uint32_t size, unsigned long *ino_out)
{
	struct super_block *sb = dir->i_sb;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(sb);
	uint8_t short_name[11];
	uint8_t *entries;
	uint32_t slots;
	uint32_t lfn_slots;
	uint32_t cluster;
	uint32_t offset;
	uint32_t short_cluster;
	uint32_t short_offset;
	uint8_t checksum;
	uint32_t cur_cluster;
	uint32_t cur_offset;
	int end_marker;
	int i;
	int error;

	if (len < 1 || len > 255)
		return -ENAMETOOLONG;
	for (i = 0; i < len; i++) {
		uint8_t c = name[i];

		if (c < 0x20 || c == '/' || c == '\\' || c == ':' ||
		    c == '*' || c == '?' || c == '"' || c == '<' ||
		    c == '>' || c == '|')
			return -EINVAL;
	}

	error = fat_dir_find_entry(dir, name, len, &cluster, &offset);
	if (!error)
		return -EEXIST;
	if (error != -ENOENT)
		return error;

	if (!fat_name_to_short(name, len, short_name))
		return fat_add_entry(dir, short_name, attributes, start_cluster,
				     size, ino_out);

	error = fat_short_alias(dir, name, len, short_name);
	if (error)
		return error;

	lfn_slots = (len + 12) / 13;
	slots = lfn_slots + 1;
	entries = kmalloc(slots * FAT_ENTRY_SIZE);
	if (!entries)
		return -ENOMEM;

	checksum = fat_short_name_checksum(short_name);
	/* The actual base offset is (seq - 1) * 13. */
	for (i = 0; i < (int)lfn_slots; i++) {
		uint8_t seq = lfn_slots - i;
		int base;

		if (i == 0)
			seq |= 0x40;
		base = ((seq & 0x1f) - 1) * 13;
		fat_fill_lfn_entry(entries + i * FAT_ENTRY_SIZE,
				   seq, checksum, name, len, base);
	}

	fat_make_short_entry(entries + lfn_slots * FAT_ENTRY_SIZE, short_name,
			     attributes, start_cluster, size);

	error = fat_dir_find_free_run(dir, slots, &cluster, &offset,
				      &end_marker);
	if (error)
		goto out;

	error = fat_dir_clear_end_markers_before(dir, cluster, offset);
	if (error)
		goto out;

	cur_cluster = cluster;
	cur_offset = offset;
	short_cluster = 0;
	short_offset = 0;
	for (i = 0; i < (int)slots; i++) {
		int last = i == (int)slots - 1;

		if (last) {
			short_cluster = cur_cluster;
			short_offset = cur_offset;
		}
		error = fat_write_entry_at(dir, cur_cluster, cur_offset,
					   entries + i * FAT_ENTRY_SIZE,
					   last ? end_marker : 0);
		if (error)
			goto out;
		cur_offset += FAT_ENTRY_SIZE;
		if (cur_offset >= cluster_size) {
			uint32_t next;

			error = fat_dir_next_cluster(dir, cur_cluster, &next);
			if (error)
				goto out;
			cur_cluster = next;
			cur_offset = 0;
		}
	}

	*ino_out = fat_entry_ino(sb, short_cluster, short_offset);
	inode_invalidate(sb, *ino_out);
	error = 0;
out:
	kfree(entries);
	return error;
}

static int fat_dir_cluster_index(struct inode *dir, uint32_t cluster,
				 uint32_t *index_out)
{
	uint32_t cur = FAT_INODE(dir).start_cluster;
	uint32_t index = 0;
	uint32_t seen = 0;

	while (cur && seen++ < FAT_SB(dir->i_sb).total_clusters) {
		uint32_t next;
		int error;

		if (cur == cluster) {
			*index_out = index;
			return 0;
		}
		error = fat_dir_next_cluster(dir, cur, &next);
		if (error == -ENOENT)
			break;
		if (error)
			return error;
		cur = next;
		index++;
	}
	return -ENOENT;
}

static int fat_dir_prev_cluster(struct inode *dir, uint32_t cluster,
				uint32_t *prev_out)
{
	uint32_t cur = FAT_INODE(dir).start_cluster;
	uint32_t prev = 0;
	uint32_t seen = 0;

	while (cur && seen++ < FAT_SB(dir->i_sb).total_clusters) {
		uint32_t next;
		int error;

		if (cur == cluster) {
			if (!prev)
				return -ENOENT;
			*prev_out = prev;
			return 0;
		}
		prev = cur;
		error = fat_dir_next_cluster(dir, cur, &next);
		if (error == -ENOENT)
			break;
		if (error)
			return error;
		cur = next;
	}
	return -ENOENT;
}

/*
 * Delete a short-name entry together with any preceding long-name entries.
 * LFN slots are written directly before the short entry and can cross a
 * directory-cluster boundary, so the backward walk follows the directory
 * chain rather than assuming all slots live in one cluster.
 */
static int fat_delete_entry_at(struct inode *dir, uint32_t cluster,
			       uint32_t offset)
{
	struct super_block *sb = dir->i_sb;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(sb);
	uint32_t first_cluster = cluster;
	uint32_t first_offset = offset;
	uint32_t cur_cluster;
	uint32_t cur_offset;
	uint32_t index;
	uint8_t *buf;
	int error;

	error = fat_dir_cluster_index(dir, cluster, &index);
	if (error)
		return error;

	cur_cluster = cluster;
	cur_offset = offset;
	for (;;) {
		uint32_t prev;
		uint32_t check;

		buf = kmalloc(cluster_size);
		if (!buf)
			return -ENOMEM;
		error = fat_read_cluster(sb, cur_cluster, buf);
		if (error) {
			kfree(buf);
			return error;
		}

		if (cur_offset == 0) {
			if (index == 0) {
				kfree(buf);
				break;
			}
			kfree(buf);
			error = fat_dir_prev_cluster(dir, cur_cluster, &prev);
			if (error)
				return error;
			buf = kmalloc(cluster_size);
			if (!buf)
				return -ENOMEM;
			error = fat_read_cluster(sb, prev, buf);
			if (error) {
				kfree(buf);
				return error;
			}
			check = cluster_size - FAT_ENTRY_SIZE;
			if (buf[check] != 0xe5 &&
			    buf[check + 11] == FAT_ATTR_LFN) {
				cur_cluster = prev;
				cur_offset = check;
				index--;
				kfree(buf);
				continue;
			}
			kfree(buf);
			break;
		}

		check = cur_offset - FAT_ENTRY_SIZE;
		if (buf[check] != 0xe5 && buf[check + 11] == FAT_ATTR_LFN) {
			cur_offset = check;
			kfree(buf);
			continue;
		}
		kfree(buf);
		break;
	}

	first_cluster = cur_cluster;
	first_offset = cur_offset;
	cur_cluster = first_cluster;
	cur_offset = first_offset;
	for (;;) {
		int changed = 0;

		buf = kmalloc(cluster_size);
		if (!buf)
			return -ENOMEM;
		error = fat_read_cluster(sb, cur_cluster, buf);
		if (error) {
			kfree(buf);
			return error;
		}

		while (cur_offset < cluster_size) {
			if (cur_cluster == cluster && cur_offset == offset) {
				buf[cur_offset] = 0xe5;
				changed = 1;
				error = changed ? fat_write_cluster(sb, cur_cluster,
								   buf) : 0;
				kfree(buf);
				return error;
			}
			if (buf[cur_offset] != 0xe5) {
				buf[cur_offset] = 0xe5;
				changed = 1;
			}
			cur_offset += FAT_ENTRY_SIZE;
		}

		if (changed) {
			error = fat_write_cluster(sb, cur_cluster, buf);
			if (error) {
				kfree(buf);
				return error;
			}
		}
		kfree(buf);

		error = fat_dir_next_cluster(dir, cur_cluster, &cur_cluster);
		if (error)
			return error;
		cur_offset = 0;
	}
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

static int fat_update_dotdot(struct inode *dir, uint32_t parent_cluster)
{
	struct super_block *sb = dir->i_sb;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(sb);
	uint8_t *buf;
	uint8_t *entry;
	int error;

	if (!FAT_INODE(dir).start_cluster)
		return -EINVAL;
	buf = kmalloc(cluster_size);
	if (!buf)
		return -ENOMEM;
	error = fat_read_cluster(sb, FAT_INODE(dir).start_cluster, buf);
	if (error)
		goto out;

	entry = buf + FAT_ENTRY_SIZE;
	if (entry[0] != '.' || entry[1] != '.' ||
	    !(entry[11] & FAT_ATTR_DIRECTORY)) {
		error = -EIO;
		goto out;
	}
	fat_write16(entry + 20, parent_cluster >> 16);
	fat_write16(entry + 26, parent_cluster);
	error = fat_write_cluster(sb, FAT_INODE(dir).start_cluster, buf);
out:
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

/*
 * Walk every directory in the filesystem and count used short-name entries
 * and free directory slots.  LFN and volume-label entries consume directory
 * capacity but are not files, so they are excluded from the used count.
 */
int fat_count_directory_entries(struct super_block *sb, uint32_t *used,
				uint32_t *free)
{
	struct fat_sb_info *fsb = &FAT_SB(sb);
	uint32_t total = fsb->total_clusters;
	uint32_t *stack;
	uint8_t *seen;
	uint32_t top = 0;
	uint32_t used_count = 0;
	uint32_t free_count = 0;
	int error = 0;

	if (!sb || !used || !free)
		return -EINVAL;

	seen = kmalloc(total);
	if (!seen)
		return -ENOMEM;
	memset(seen, 0, total);
	stack = kmalloc(total * sizeof(*stack));
	if (!stack) {
		kfree(seen);
		return -ENOMEM;
	}

	stack[top++] = fsb->root_cluster;
	if (fsb->root_cluster >= 2 &&
	    fsb->root_cluster - 2 < total)
		seen[fsb->root_cluster - 2] = 1;

	while (top) {
		uint32_t dir_cluster = stack[--top];
		uint32_t cluster = dir_cluster;
		uint32_t chain_seen = 0;

		while (cluster && chain_seen++ < total) {
			uint32_t next = 0;
			uint32_t offset;
			uint8_t *buf;

			buf = kmalloc(fsb->cluster_size);
			if (!buf) {
				error = -ENOMEM;
				goto out;
			}
			error = fat_read_cluster(sb, cluster, buf);
			if (error) {
				kfree(buf);
				goto out;
			}

			for (offset = 0; offset < fsb->cluster_size;
			     offset += FAT_ENTRY_SIZE) {
				const uint8_t *entry = buf + offset;
				uint8_t first = entry[0];
				uint8_t attributes = entry[11];
				uint32_t start_cluster;

				if (first == 0x00 || first == 0xe5) {
					free_count++;
					continue;
				}
				if (attributes == FAT_ATTR_LFN ||
				    (attributes & FAT_ATTR_VOLUME_ID))
					continue;

				used_count++;
				if (!(attributes & FAT_ATTR_DIRECTORY))
					continue;

				start_cluster = fat_entry_first_cluster(entry);
				if (start_cluster >= 2 &&
				    start_cluster - 2 < total &&
				    !seen[start_cluster - 2]) {
					seen[start_cluster - 2] = 1;
					stack[top++] = start_cluster;
				}
			}

			kfree(buf);
			error = fat_next_cluster(sb, cluster, &next);
			if (error == -ENOENT)
				break;
			if (error)
				goto out;
			cluster = next;
		}

		if (cluster) {
			error = -EIO;
			goto out;
		}
	}

	*used = used_count;
	*free = free_count;
	error = 0;
out:
	kfree(stack);
	kfree(seen);
	return error;
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
	unsigned long ino;
	struct inode *inode;
	int error;

	*res = NULL;
	if (FAT_SB(dir->i_sb).readonly) {
		iput(dir);
		return -EROFS;
	}

	error = fat_add_entry_name(dir, name, len, FAT_ATTR_ARCHIVE, 0, 0,
				   &ino);
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

static int fat_mkdir(struct inode *dir, char *name, int len, int mode)
{
	uint8_t dot_name[11];
	uint8_t dotdot_name[11];
	uint8_t entry[FAT_ENTRY_SIZE];
	uint8_t *buf;
	uint32_t cluster;
	uint32_t cluster_size = FAT_CLUSTER_SIZE(dir->i_sb);
	unsigned long ino;
	struct inode *inode;
	int error = 0;

	(void)mode;

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

	error = fat_add_entry_name(dir, name, len, FAT_ATTR_DIRECTORY,
				   cluster, 0, &ino);
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
	if (error) {
		unsigned long rollback_ino;

		fat_add_entry_name(dir, name, len, entry[11], start_cluster,
				   fat_entry_size(entry), &rollback_ino);
	}
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
	if (error) {
		unsigned long rollback_ino;

		fat_add_entry_name(dir, name, len, entry[11], start_cluster, 0,
				   &rollback_ino);
	} else {
		inode_invalidate(dir->i_sb, ino);
	}
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
	static struct semaphore fat_rename_sem = INIT_MUTEX(fat_rename_sem);
	uint8_t old_entry[FAT_ENTRY_SIZE];
	uint8_t new_entry[FAT_ENTRY_SIZE];
	uint32_t old_cluster;
	uint32_t old_offset;
	uint32_t new_cluster;
	uint32_t new_offset;
	uint32_t added_cluster;
	uint32_t added_offset;
	unsigned long old_ino;
	unsigned long new_ino = 0;
	unsigned long target_ino = 0;
	struct inode *old_inode = NULL;
	struct inode *new_inode = NULL;
	int same_dir = old_dir == new_dir;
	int new_exists = 0;
	int error = 0;

	if (FAT_SB(old_dir->i_sb).readonly) {
		iput(old_dir);
		iput(new_dir);
		return -EROFS;
	}

	down(&fat_rename_sem);
	down(&old_dir->i_sem);
	if (!same_dir)
		down(&new_dir->i_sem);

	error = fat_dir_find_entry(old_dir, old_name, old_len,
				   &old_cluster, &old_offset);
	if (error)
		goto out;
	old_ino = fat_entry_ino(old_dir->i_sb, old_cluster, old_offset);
	old_inode = iget(old_dir->i_sb, old_ino);
	if (!old_inode) {
		error = -ENOMEM;
		goto out;
	}
	error = fat_read_entry_at(old_dir, old_cluster, old_offset, old_entry);
	if (error)
		goto out;

	error = fat_dir_find_entry(new_dir, new_name, new_len,
				   &new_cluster, &new_offset);
	if (!error) {
		/* Renaming an entry over itself is a no-op. */
		if (same_dir && old_cluster == new_cluster &&
		    old_offset == new_offset)
			goto out;

		new_exists = 1;
		target_ino = fat_entry_ino(new_dir->i_sb, new_cluster,
					   new_offset);
		new_inode = iget(new_dir->i_sb, target_ino);
		if (!new_inode) {
			error = -ENOMEM;
			goto out;
		}
		error = fat_read_entry_at(new_dir, new_cluster, new_offset,
					  new_entry);
		if (error)
			goto out;

		if (S_ISDIR(old_inode->i_mode) &&
		    !(new_entry[11] & FAT_ATTR_DIRECTORY)) {
			error = -ENOTDIR;
			goto out;
		}
		if (!S_ISDIR(old_inode->i_mode) &&
		    (new_entry[11] & FAT_ATTR_DIRECTORY)) {
			error = -EISDIR;
			goto out;
		}
		if (new_entry[11] & FAT_ATTR_DIRECTORY) {
			error = fat_dir_is_empty(new_inode);
			if (error <= 0) {
				error = error ? error : -ENOTEMPTY;
				goto out;
			}
		}
	} else if (error != -ENOENT) {
		goto out;
	} else {
		error = 0;
	}

	error = fat_delete_entry_at(old_dir, old_cluster, old_offset);
	if (error) {
		goto out;
	}
	inode_invalidate(old_dir->i_sb, old_ino);

	if (new_exists) {
		error = fat_delete_entry_at(new_dir, new_cluster, new_offset);
		if (error) {
			unsigned long rollback_ino;

			fat_add_entry_name(old_dir, old_name, old_len,
					   old_entry[11],
					   fat_entry_first_cluster(old_entry),
					   fat_entry_size(old_entry),
					   &rollback_ino);
			goto out;
		}
		inode_invalidate(new_dir->i_sb, target_ino);
	}

	error = fat_add_entry_name(new_dir, new_name, new_len, old_entry[11],
				   fat_entry_first_cluster(old_entry),
				   fat_entry_size(old_entry), &new_ino);
	if (error) {
		unsigned long rollback_ino;

		fat_add_entry_name(old_dir, old_name, old_len, old_entry[11],
				   fat_entry_first_cluster(old_entry),
				   fat_entry_size(old_entry), &rollback_ino);
		if (new_exists)
			fat_add_entry_name(new_dir, new_name, new_len,
					   new_entry[11],
					   fat_entry_first_cluster(new_entry),
					   fat_entry_size(new_entry),
					   &rollback_ino);
		goto out;
	}

	error = fat_ino_location(new_dir->i_sb, new_ino,
				 &added_cluster, &added_offset);
	if (error)
		goto out;

	FAT_INODE(old_inode).dir_cluster = added_cluster;
	FAT_INODE(old_inode).dir_offset = added_offset;
	FAT_INODE(old_inode).attributes = old_entry[11];
	if (S_ISDIR(old_inode->i_mode)) {
		if (!same_dir) {
			error = fat_update_dotdot(old_inode,
						  FAT_INODE(new_dir).start_cluster);
			if (error)
				goto out;
		}
		FAT_INODE(old_inode).parent_ino = new_dir->i_ino;
	}

	old_inode->i_mtime = old_inode->i_ctime = current_time();
	error = fat_write_inode(old_inode);
	if (error)
		goto out;

	if (new_exists) {
		error = fat_free_cluster_chain(new_dir->i_sb,
					      fat_entry_first_cluster(new_entry));
		if (error)
			goto out;
	}
out:
	if (new_inode)
		iput(new_inode);
	if (old_inode)
		iput(old_inode);
	if (!same_dir)
		up(&new_dir->i_sem);
	up(&old_dir->i_sem);
	up(&fat_rename_sem);
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
