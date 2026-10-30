#include <fs/fs.h>
#include <fs/ext2/ext2.h>
#include <fs/dirent.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

extern uint64_t current_time(void);

static void ext2_touch_dir(struct inode *dir);
static int ext2_add_entry_linear(struct inode *dir, const char *name, int len,
				 uint32_t ino, uint8_t file_type);
static int ext2_delete_entry_linear(struct inode *dir, const char *name,
				    int len);

static int ext2_dir_is_indexed(struct inode *dir)
{
	return !!(ext2_read32((const uint8_t *)&EXT2_INODE(dir).raw.i_flags) &
		  EXT2_BTREE_FL);
}

static void ext2_dir_deindex(struct inode *dir)
{
	uint32_t flags = ext2_read32((const uint8_t *)&EXT2_INODE(dir).raw.i_flags);

	ext2_write32((uint8_t *)&EXT2_INODE(dir).raw.i_flags,
		     flags & ~EXT2_BTREE_FL);
	ext2_touch_dir(dir);
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

#define EXT2_DX_ROOT_INFO_OFFSET		24
#define EXT2_DX_ROOT_ENTRIES_OFFSET		32
#define EXT2_DX_NODE_ENTRIES_OFFSET		8

struct ext2_dx_rebuild_entry {
	uint32_t ino;
	uint32_t hash;
	uint8_t file_type;
	uint8_t name_len;
	char name[EXT2_NAME_LEN];
};

static int ext2_dx_rebuild_entry_before(const struct ext2_dx_rebuild_entry *a,
					const struct ext2_dx_rebuild_entry *b)
{
	int min_len;
	int cmp;

	if (a->hash != b->hash)
		return a->hash < b->hash;

	min_len = a->name_len < b->name_len ? a->name_len : b->name_len;
	cmp = memcmp(a->name, b->name, min_len);
	if (cmp)
		return cmp < 0;
	return a->name_len < b->name_len;
}

static void ext2_dx_sort_entries(struct ext2_dx_rebuild_entry *entries,
				 uint32_t count)
{
	uint32_t i;

	for (i = 1; i < count; i++) {
		struct ext2_dx_rebuild_entry key = entries[i];
		uint32_t j = i;

		while (j > 0 && ext2_dx_rebuild_entry_before(&key, &entries[j - 1])) {
			entries[j] = entries[j - 1];
			j--;
		}
		entries[j] = key;
	}
}

static uint32_t ext2_dx_count(const uint8_t *entries)
{
	return ext2_read16(entries + 2);
}

static uint32_t ext2_dx_limit(const uint8_t *entries)
{
	return ext2_read16(entries);
}

static uint32_t ext2_dx_entry_hash(const uint8_t *entries, uint32_t index)
{
	return ext2_read32(entries + index * sizeof(struct ext2_dx_entry));
}

static uint32_t ext2_dx_entry_block(const uint8_t *entries, uint32_t index)
{
	return ext2_read32(entries + index * sizeof(struct ext2_dx_entry) + 4);
}

/*
 * Pick the index entry whose hash range contains @hash.  Entry zero is the
 * dummy entry that owns the lowest range, so it is handled as the lower
 * bound of the binary search instead of using its countlimit word as a hash.
 */
static int ext2_dx_probe(const uint8_t *entries, uint32_t available,
			 uint32_t hash, uint32_t *logical, uint32_t *index)
{
	uint32_t count = ext2_dx_count(entries);
	uint32_t limit = ext2_dx_limit(entries);
	uint32_t first = 1;
	uint32_t last;

	if (!limit || limit > available / sizeof(struct ext2_dx_entry) ||
	    count < 1 || count > limit)
		return -EIO;

	if (count == 1) {
		*logical = ext2_dx_entry_block(entries, 0);
		*index = 0;
		return 0;
	}

	last = count - 1;
	while (first <= last) {
		uint32_t mid = first + (last - first) / 2;

		if (ext2_dx_entry_hash(entries, mid) > hash)
			last = mid - 1;
		else
			first = mid + 1;
	}

	*index = first - 1;
	*logical = ext2_dx_entry_block(entries, *index);
	return 0;
}

/*
 * Locate the leaf block for @hash in a level-zero htree.  The root buffer is
 * returned to the caller so an insertion can update the index boundaries in
 * the same in-memory copy before writing it back.
 */
static int ext2_dx_probe_leaf(struct inode *dir, uint32_t hash,
			      uint32_t *leaf_logical, uint32_t *physical,
			      uint32_t *root_physical, uint8_t **root_buf,
			      uint8_t **entries_out, uint32_t *entry_index)
{
	struct super_block *sb = dir->i_sb;
	uint32_t block_size = sb->s_block_size;
	struct ext2_dx_root_info *info;
	uint8_t *buf;
	uint8_t *entries;
	uint32_t index;
	uint32_t target;
	int error;

	*root_buf = NULL;
	*entries_out = NULL;
	buf = kmalloc(block_size);
	if (!buf)
		return -ENOMEM;

	*physical = ext2_bmap(dir, 0, 0);
	if (!*physical) {
		error = -EIO;
		goto out;
	}
	*root_physical = *physical;
	error = ext2_read_block(sb, *physical, buf);
	if (error)
		goto out;

	info = (struct ext2_dx_root_info *)(buf + EXT2_DX_ROOT_INFO_OFFSET);
	if (info->hash_version != EXT2_HASH_HALF_MD4 ||
	    info->info_length != 8 || info->indirect_levels != 0) {
		error = -ENOTSUP;
		goto out;
	}

	entries = buf + EXT2_DX_ROOT_ENTRIES_OFFSET;
	error = ext2_dx_probe(entries, block_size - EXT2_DX_ROOT_ENTRIES_OFFSET,
			      hash, &target, &index);
	if (error)
		goto out;
	if (!target ||
	    (uint64_t)target * block_size >= (uint64_t)dir->i_size) {
		error = -EIO;
		goto out;
	}

	*leaf_logical = target;
	*entry_index = index;
	*entries_out = entries;
	*root_buf = buf;
	*physical = ext2_bmap(dir, target, 0);
	if (!*physical) {
		error = -EIO;
		goto out;
	}
	return 0;
out:
	kfree(buf);
	return error;
}

/*
 * Resolve a name in an indexed directory.  The root block is logical block
 * zero; interior nodes have the same dx_entry layout at offset 8 instead of
 * 32.  The index is walked without recursion, bounded by the on-disk
 * indirect_levels field, and the selected leaf is scanned like a normal ext2
 * directory block.
 */
static int ext2_dx_find_entry(struct inode *dir, const char *name, int len,
			      uint32_t *res_block, uint32_t *res_offset)
{
	struct super_block *sb = dir->i_sb;
	uint32_t block_size = sb->s_block_size;
	uint32_t root_levels;
	uint32_t depth = 0;
	uint32_t seen[3] = { 0, 0, 0 };
	uint32_t hash;
	uint32_t physical;
	uint32_t target;
	uint32_t offset;
	uint32_t limit;
	uint8_t *buf;
	const uint8_t *entries;
	int error;

	error = ext2_dirhash(sb, name, len, &hash);
	if (error)
		return error;

	physical = ext2_bmap(dir, 0, 0);
	if (!physical)
		return -EIO;
	buf = kmalloc(block_size);
	if (!buf)
		return -ENOMEM;
	error = ext2_read_block(sb, physical, buf);
	if (error)
		goto out;

	{
		struct ext2_dx_root_info *info =
			(struct ext2_dx_root_info *)(buf + EXT2_DX_ROOT_INFO_OFFSET);

		/*
		 * Only half-MD4 indexed directories are interpreted.  The
		 * root info is fixed size in ext2/3 and htree depth is capped
		 * at two, keeping the iterative walk below bounded.
		 */
		if (info->hash_version != EXT2_HASH_HALF_MD4 ||
		    info->info_length != 8 || info->indirect_levels > 2) {
			error = -ENOTSUP;
			goto out;
		}
		root_levels = info->indirect_levels;
	}

	entries = buf + EXT2_DX_ROOT_ENTRIES_OFFSET;
	for (;;) {
		uint32_t i;
		uint32_t entry_index;

		error = ext2_dx_probe(entries, block_size -
				      (uint32_t)(entries - buf), hash, &target,
				      &entry_index);
		if (error)
			goto out;
		if (!target ||
		    (uint64_t)target * block_size >= (uint64_t)dir->i_size) {
			error = -EIO;
			goto out;
		}
		for (i = 0; i < depth; i++) {
			if (seen[i] == target) {
				error = -EIO;
				goto out;
			}
		}
		seen[depth] = target;

		if (depth == root_levels)
			break;

		kfree(buf);
		buf = NULL;
		physical = ext2_bmap(dir, target, 0);
		if (!physical) {
			error = -EIO;
			goto out;
		}
		buf = kmalloc(block_size);
		if (!buf) {
			error = -ENOMEM;
			goto out;
		}
		error = ext2_read_block(sb, physical, buf);
		if (error)
			goto out;
		entries = buf + EXT2_DX_NODE_ENTRIES_OFFSET;
		depth++;
	}

	physical = ext2_bmap(dir, target, 0);
	if (!physical) {
		error = -EIO;
		goto out;
	}

	kfree(buf);
	buf = kmalloc(block_size);
	if (!buf) {
		error = -ENOMEM;
		goto out;
	}
	error = ext2_read_block(sb, physical, buf);
	if (error)
		goto out;

	limit = block_size;
	if ((uint64_t)target * block_size + limit > (uint64_t)dir->i_size)
		limit = dir->i_size - target * block_size;

	for (offset = 0; offset < limit;) {
		struct ext2_dir_entry_2 *entry =
			(struct ext2_dir_entry_2 *)(buf + offset);
		uint16_t rec_len =
			ext2_read16((const uint8_t *)&entry->rec_len);

		if (rec_len < 8 || offset + rec_len > limit) {
			error = -EIO;
			goto out;
		}
		if (ext2_read32((const uint8_t *)&entry->inode) &&
		    entry->name_len == len &&
		    !memcmp(entry->name, name, len)) {
			*res_block = physical;
			*res_offset = offset;
			error = 0;
			goto out;
		}
		offset += rec_len;
	}

	error = -ENOENT;
out:
	if (buf)
		kfree(buf);
	return error;
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

	if (ext2_dir_is_indexed(dir)) {
		error = ext2_dx_find_entry(dir, name, len, res_block,
					   res_offset);
		if (error != -ENOTSUP)
			return error;
	}

	/*
	 * Fallback for indexed directories whose hash version or tree shape is
	 * not supported by ext2_dx_find_entry().  Their leaf blocks still use
	 * the normal directory-entry format, and index blocks start with fake
	 * entries whose rec_len covers the index payload, so this linear scan
	 * skips those blocks and reaches every leaf entry.
	 */
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

static int ext2_dx_leaf_insert(struct inode *dir, uint32_t physical,
			       uint8_t *buf, const char *name, int len,
			       uint32_t ino, uint8_t file_type)
{
	struct super_block *sb = dir->i_sb;
	uint32_t block_size = sb->s_block_size;
	uint16_t rec_len = EXT2_DIR_REC_LEN(len);
	uint32_t offset;
	uint32_t prev_offset = 0;
	uint16_t prev_rec = 0;

	for (offset = 0; offset < block_size;) {
		struct ext2_dir_entry_2 *entry =
			(struct ext2_dir_entry_2 *)(buf + offset);
		uint16_t entry_rec =
			ext2_read16((const uint8_t *)&entry->rec_len);

		if (entry_rec < 8 || offset + entry_rec > block_size)
			break;
		if (!ext2_read32((const uint8_t *)&entry->inode) &&
		    entry_rec >= rec_len) {
			ext2_write32((uint8_t *)&entry->inode, ino);
			ext2_write16((uint8_t *)&entry->rec_len, entry_rec);
			entry->name_len = len;
			entry->file_type = file_type;
			memcpy(entry->name, name, len);
			return ext2_write_block(sb, physical, buf);
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

		if (new_offset + rec_len <= block_size) {
			struct ext2_dir_entry_2 *entry;

			ext2_write16((uint8_t *)&prev->rec_len, used);
			entry = (struct ext2_dir_entry_2 *)(buf + new_offset);
			ext2_write32((uint8_t *)&entry->inode, ino);
			ext2_write16((uint8_t *)&entry->rec_len,
				     block_size - new_offset);
			entry->name_len = len;
			entry->file_type = file_type;
			memcpy(entry->name, name, len);
			return ext2_write_block(sb, physical, buf);
		}
	}

	return -ENOSPC;
}

static int ext2_dx_leaf_min_hash(struct inode *dir, const uint8_t *buf,
				 uint32_t *min_hash)
{
	struct super_block *sb = dir->i_sb;
	uint32_t block_size = sb->s_block_size;
	uint32_t offset;
	int found = 0;

	*min_hash = 0;
	for (offset = 0; offset < block_size;) {
		struct ext2_dir_entry_2 *entry =
			(struct ext2_dir_entry_2 *)(buf + offset);
		uint16_t rec_len =
			ext2_read16((const uint8_t *)&entry->rec_len);
		uint32_t hash;
		int error;

		if (rec_len < 8 || offset + rec_len > block_size)
			break;
		if (!ext2_read32((const uint8_t *)&entry->inode)) {
			offset += rec_len;
			continue;
		}
		error = ext2_dirhash(sb, entry->name, entry->name_len, &hash);
		if (error)
			return error;
		if (!found || hash < *min_hash) {
			*min_hash = hash;
			found = 1;
		}
		offset += rec_len;
	}
	return 0;
}

static void ext2_dx_pack_block(uint8_t *buf, uint32_t block_size,
			       const struct ext2_dx_rebuild_entry *entries,
			       uint32_t start, uint32_t end)
{
	uint32_t offset = 0;
	uint32_t last_offset = 0;
	uint32_t i;

	memset(buf, 0, block_size);
	if (end == start) {
		struct ext2_dir_entry_2 *entry = (struct ext2_dir_entry_2 *)buf;

		ext2_write16((uint8_t *)&entry->rec_len, block_size);
		return;
	}

	for (i = start; i < end; i++) {
		struct ext2_dir_entry_2 *entry =
			(struct ext2_dir_entry_2 *)(buf + offset);
		uint16_t rec_len = EXT2_DIR_REC_LEN(entries[i].name_len);

		ext2_write32((uint8_t *)&entry->inode, entries[i].ino);
		ext2_write16((uint8_t *)&entry->rec_len, rec_len);
		entry->name_len = entries[i].name_len;
		entry->file_type = entries[i].file_type;
		memcpy(entry->name, entries[i].name, entries[i].name_len);
		last_offset = offset;
		offset += rec_len;
	}
	if (last_offset + EXT2_DIR_REC_LEN(entries[end - 1].name_len) <
	    block_size) {
		struct ext2_dir_entry_2 *last =
			(struct ext2_dir_entry_2 *)(buf + last_offset);

		ext2_write16((uint8_t *)&last->rec_len,
			     block_size - last_offset);
	}
}

static int ext2_dx_split_leaf(struct inode *dir, uint32_t leaf_physical,
			      uint32_t root_physical,
			      uint32_t entry_index, uint32_t new_hash,
			      uint8_t *root_buf, uint8_t *root_entries,
			      const char *name, int len, uint32_t ino,
			      uint8_t file_type)
{
	struct super_block *sb = dir->i_sb;
	struct ext2_inode *raw = &EXT2_INODE(dir).raw;
	uint32_t block_size = sb->s_block_size;
	uint32_t old_count;
	uint32_t count = ext2_dx_count(root_entries);
	uint32_t limit = ext2_dx_limit(root_entries);
	struct ext2_dx_rebuild_entry *items;
	uint8_t *leaf_buf;
	uint8_t *first_buf;
	uint8_t *second_buf;
	uint32_t total;
	uint32_t old_leaf_entries = 0;
	uint32_t pos;
	uint32_t first_count;
	uint32_t first_size;
	uint32_t new_logical;
	uint32_t new_physical;
	uint32_t offset;
	uint32_t i;
	int error;

	if (count >= limit)
		return -ENOTSUP;

	leaf_buf = kmalloc(block_size);
	if (!leaf_buf)
		return -ENOMEM;
	error = ext2_read_block(sb, leaf_physical, leaf_buf);
	if (error) {
		kfree(leaf_buf);
		return error;
	}

	for (offset = 0; offset < block_size;) {
		struct ext2_dir_entry_2 *entry =
			(struct ext2_dir_entry_2 *)(leaf_buf + offset);
		uint16_t rec_len =
			ext2_read16((const uint8_t *)&entry->rec_len);

		if (rec_len < 8 || offset + rec_len > block_size)
			break;
		if (ext2_read32((const uint8_t *)&entry->inode))
			old_leaf_entries++;
		offset += rec_len;
	}

	total = old_leaf_entries + 1;
	items = kmalloc(total * sizeof(*items));
	if (!items) {
		kfree(leaf_buf);
		return -ENOMEM;
	}

	old_count = 0;
	for (offset = 0; offset < block_size;) {
		struct ext2_dir_entry_2 *entry =
			(struct ext2_dir_entry_2 *)(leaf_buf + offset);
		uint16_t rec_len =
			ext2_read16((const uint8_t *)&entry->rec_len);

		if (rec_len < 8 || offset + rec_len > block_size)
			break;
		if (ext2_read32((const uint8_t *)&entry->inode)) {
			struct ext2_dx_rebuild_entry *item = &items[old_count++];

			item->ino = ext2_read32((const uint8_t *)&entry->inode);
			item->name_len = entry->name_len;
			item->file_type = entry->file_type;
			memcpy(item->name, entry->name, entry->name_len);
			error = ext2_dirhash(sb, entry->name, entry->name_len,
					     &item->hash);
			if (error) {
				kfree(items);
				kfree(leaf_buf);
				return error;
			}
		}
		offset += rec_len;
	}
	kfree(leaf_buf);

	items[old_count].ino = ino;
	items[old_count].hash = new_hash;
	items[old_count].name_len = len;
	items[old_count].file_type = file_type;
	memcpy(items[old_count].name, name, len);
	ext2_dx_sort_entries(items, total);

	pos = 0;
	first_count = 0;
	first_size = 0;
	while (pos < total) {
		uint32_t group_start = pos;
		uint32_t group_hash = items[pos].hash;
		uint32_t group_size = 0;

		while (pos < total && items[pos].hash == group_hash) {
			group_size += EXT2_DIR_REC_LEN(items[pos].name_len);
			pos++;
		}
		if (group_size > block_size) {
			kfree(items);
			return -ENOTSUP;
		}
		if (first_size + group_size > block_size) {
			pos = group_start;
			break;
		}
		first_size += group_size;
		first_count = pos;
		if (pos == total)
			break;
	}
	if (first_count == 0 || first_count == total) {
		kfree(items);
		return -ENOTSUP;
	}

	new_logical = dir->i_size / block_size;
	new_physical = ext2_bmap(dir, new_logical, 1);
	if (!new_physical) {
		kfree(items);
		return -ENOSPC;
	}
	first_buf = kmalloc(block_size);
	second_buf = kmalloc(block_size);
	if (!first_buf || !second_buf) {
		if (first_buf)
			kfree(first_buf);
		if (second_buf)
			kfree(second_buf);
		kfree(items);
		return -ENOMEM;
	}

	ext2_dx_pack_block(first_buf, block_size, items, 0, first_count);
	ext2_dx_pack_block(second_buf, block_size, items, first_count, total);
	error = ext2_write_block(sb, leaf_physical, first_buf);
	if (!error)
		error = ext2_write_block(sb, new_physical, second_buf);
	kfree(first_buf);
	kfree(second_buf);
	if (error) {
		kfree(items);
		return error;
	}

	dir->i_size += block_size;
	ext2_write32((uint8_t *)&raw->i_size, dir->i_size);
	ext2_touch_dir(dir);

	for (i = count; i > entry_index + 1; i--) {
		ext2_write32(root_entries + i * sizeof(struct ext2_dx_entry),
			     ext2_dx_entry_hash(root_entries, i - 1));
		ext2_write32(root_entries + i * sizeof(struct ext2_dx_entry) + 4,
			     ext2_dx_entry_block(root_entries, i - 1));
	}
	ext2_write32(root_entries + (entry_index + 1) *
		     sizeof(struct ext2_dx_entry),
		     items[first_count].hash);
	ext2_write32(root_entries + (entry_index + 1) *
		     sizeof(struct ext2_dx_entry) + 4, new_logical);
	if (entry_index > 0) {
		ext2_write32(root_entries + entry_index *
			     sizeof(struct ext2_dx_entry), items[0].hash);
	}
	ext2_write16(root_entries + 2, count + 1);
	error = ext2_write_block(sb, root_physical, root_buf);
	kfree(items);
	if (error) {
		ext2_dir_deindex(dir);
		error = 0;
	}
	return error;
}

static int ext2_add_entry_indexed(struct inode *dir, const char *name, int len,
				  uint32_t ino, uint8_t file_type)
{
	struct super_block *sb = dir->i_sb;
	uint32_t block_size = sb->s_block_size;
	uint32_t hash;
	uint32_t leaf_logical;
	uint32_t leaf_physical;
	uint32_t root_physical;
	uint32_t entry_index;
	uint32_t min_hash;
	uint8_t *root_buf;
	uint8_t *root_entries;
	uint8_t *leaf_buf;
	int error;

	if (len > EXT2_NAME_LEN)
		return -ENAMETOOLONG;

	error = ext2_dirhash(sb, name, len, &hash);
	if (error)
		goto deindex;
	error = ext2_dx_probe_leaf(dir, hash, &leaf_logical, &leaf_physical,
				   &root_physical, &root_buf, &root_entries,
				   &entry_index);
	if (error)
		goto deindex;

	leaf_buf = kmalloc(block_size);
	if (!leaf_buf) {
		kfree(root_buf);
		goto deindex;
	}
	error = ext2_read_block(sb, leaf_physical, leaf_buf);
	if (error) {
		kfree(leaf_buf);
		kfree(root_buf);
		goto deindex;
	}
	error = ext2_dx_leaf_insert(dir, leaf_physical, leaf_buf, name, len,
				    ino, file_type);
	if (error == -ENOSPC) {
		kfree(leaf_buf);
		error = ext2_dx_split_leaf(dir, leaf_physical,
					   root_physical, entry_index, hash,
					   root_buf, root_entries, name, len,
					   ino, file_type);
		kfree(root_buf);
		if (error == -ENOTSUP)
			goto deindex;
		return error;
	}
	if (error) {
		kfree(leaf_buf);
		kfree(root_buf);
		goto deindex;
	}

	error = ext2_dx_leaf_min_hash(dir, leaf_buf, &min_hash);
	kfree(leaf_buf);
	if (error) {
		kfree(root_buf);
		ext2_dir_deindex(dir);
		return 0;
	}
	if (!error && entry_index > 0) {
		ext2_write32(root_entries + entry_index *
			     sizeof(struct ext2_dx_entry), min_hash);
		error = ext2_write_block(sb, root_physical, root_buf);
		if (error) {
			ext2_dir_deindex(dir);
			error = 0;
		}
	}
	kfree(root_buf);
	if (!error)
		ext2_touch_dir(dir);
	return error;

deindex:
	ext2_dir_deindex(dir);
	return ext2_add_entry_linear(dir, name, len, ino, file_type);
}

static int ext2_add_entry_linear(struct inode *dir, const char *name, int len,
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

static int ext2_add_entry(struct inode *dir, const char *name, int len,
			  uint32_t ino, uint8_t file_type)
{
	if (ext2_dir_is_indexed(dir))
		return ext2_add_entry_indexed(dir, name, len, ino, file_type);
	return ext2_add_entry_linear(dir, name, len, ino, file_type);
}

static int ext2_delete_entry_indexed(struct inode *dir, const char *name,
				     int len)
{
	struct super_block *sb = dir->i_sb;
	uint32_t block;
	uint32_t offset;
	uint8_t *buf;
	struct ext2_dir_entry_2 *entry;
	int error;

	error = ext2_dx_find_entry(dir, name, len, &block, &offset);
	if (error == -ENOTSUP) {
		ext2_dir_deindex(dir);
		return ext2_delete_entry_linear(dir, name, len);
	}
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
	kfree(buf);
	if (!error)
		ext2_touch_dir(dir);
	return error;
}

static int ext2_delete_entry_linear(struct inode *dir, const char *name,
				    int len)
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

static int ext2_delete_entry(struct inode *dir, const char *name, int len)
{
	if (ext2_dir_is_indexed(dir))
		return ext2_delete_entry_indexed(dir, name, len);
	return ext2_delete_entry_linear(dir, name, len);
}

static int ext2_dir_is_empty(struct inode *dir)
{
	struct super_block *sb = dir->i_sb;
	uint32_t block_index;

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

	/*
	 * Indexed directories use the same leaf-block format as linear ones, so
	 * readdir can list them by walking the logical blocks directly.
	 */
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
