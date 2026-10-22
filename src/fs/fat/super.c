#include <fs/fs.h>
#include <fs/fat/fat.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

static struct super_operations fat_super_operations;

/*
 * Update the FAT32 clean-shutdown bit in the boot sector.  Bit 0 is clear
 * while the volume may contain unflushed writes, and set after a clean
 * unmount.  Read-only mounts never touch the flag.
 */
static int fat_set_clean_shutdown(struct super_block *sb, int clean)
{
	struct fat_sb_info *fsb = &FAT_SB(sb);
	uint8_t *buf;
	int error;

	buf = kmalloc(fsb->bytes_per_sector);
	if (!buf)
		return -ENOMEM;
	error = blkdev_read(sb->s_dev, 0, 1, (char *)buf);
	if (error)
		goto out;
	if (buf[510] != 0x55 || buf[511] != 0xaa) {
		error = -EIO;
		goto out;
	}
	if (clean)
		buf[FAT_BPB_CLEAN_SHUTDOWN] |= 0x01;
	else
		buf[FAT_BPB_CLEAN_SHUTDOWN] &= ~0x01;
	error = blkdev_write(sb->s_dev, 0, 1, (char *)buf);
out:
	kfree(buf);
	return error;
}

static int fat_write_super(struct super_block *sb)
{
	int error;

	if (!sb || FAT_SB(sb).readonly)
		return 0;

	error = fat_set_clean_shutdown(sb, 1);
	if (error)
		return error;
	fat_write_fsinfo(sb);
	return 0;
}

/*
 * Parse and validate the FAT32 BPB from a 512-byte boot sector.  Fields are
 * read directly from the buffer rather than through a packed struct because
 * the BPB layout predates natural C alignment.
 */
static int fat_parse_bpb(struct super_block *sb, const uint8_t *buf)
{
	struct fat_sb_info *fsb = &FAT_SB(sb);
	uint16_t total16;
	uint32_t total32;
	uint32_t fat_start;

	fsb->bytes_per_sector = fat_read16(buf + FAT_BPB_BYTES_PER_SECTOR);
	fsb->sectors_per_cluster = buf[FAT_BPB_SECTORS_PER_CLUSTER];
	fsb->reserved_sectors = fat_read16(buf + FAT_BPB_RESERVED_SECTORS);
	fsb->num_fats = buf[FAT_BPB_NUM_FATS];
	total16 = fat_read16(buf + FAT_BPB_TOTAL_SECTORS16);
	total32 = fat_read32(buf + FAT_BPB_TOTAL_SECTORS32);
	fsb->fat_size32 = fat_read32(buf + FAT_BPB_FAT_SIZE32);
	fsb->root_cluster = fat_read32(buf + FAT_BPB_ROOT_CLUSTER);
	fsb->fsinfo_sector = fat_read16(buf + FAT_BPB_FSINFO);

	if (fsb->bytes_per_sector != 512)
		return -EINVAL;
	if (!fsb->sectors_per_cluster || fsb->sectors_per_cluster > 128 ||
	    (fsb->sectors_per_cluster & (fsb->sectors_per_cluster - 1)))
		return -EINVAL;
	if (!fsb->reserved_sectors || !fsb->fat_size32)
		return -EINVAL;
	if (fsb->num_fats < 1 || fsb->num_fats > 2)
		return -EINVAL;
	if (fsb->root_cluster < 2)
		return -EINVAL;

	/* FAT32 stores the total count in the 32-bit field; use the legacy
	 * 16-bit field only when it is present. */
	fsb->total_sectors = total16 ? total16 : total32;
	if (!fsb->total_sectors)
		return -EINVAL;
	if (fsb->fsinfo_sector &&
	    fsb->fsinfo_sector >= fsb->total_sectors)
		return -EINVAL;

	fat_start = fsb->reserved_sectors;
	fsb->first_data_sector = fat_start +
				 fsb->num_fats * fsb->fat_size32;
	if (fsb->first_data_sector >= fsb->total_sectors)
		return -EINVAL;
	fsb->data_sectors = fsb->total_sectors - fsb->first_data_sector;
	fsb->total_clusters = fsb->data_sectors /
			      fsb->sectors_per_cluster;
	if (fsb->total_clusters < 2)
		return -EINVAL;
	fsb->cluster_size = fsb->bytes_per_sector *
			    fsb->sectors_per_cluster;
	return 0;
}

struct super_block *fat_read_super(struct super_block *sb)
{
	struct fat_sb_info *fsb;
	uint8_t *buf;
	int error;

	/*
	 * mount_root() probes every registered filesystem on the ramdisk root,
	 * so a non-FAT device must fail cleanly here.
	 */
	if (!sb || !sb->s_dev)
		return NULL;

	buf = kmalloc(512);
	if (!buf)
		return NULL;
	error = blkdev_read(sb->s_dev, 0, 1, (char *)buf);
	if (error)
		goto fail;
	if (buf[510] != 0x55 || buf[511] != 0xaa)
		goto fail;

	error = fat_parse_bpb(sb, buf);
	if (error)
		goto fail;

	fsb = &FAT_SB(sb);
	fsb->readonly = (sb->s_flags & MS_RDONLY) ? 1 : 0;
	fsb->next_free = 2;
	{
		int i;

		for (i = 0; i < FAT_FAT_CACHE_SLOTS; i++) {
			fsb->fat_cache[i] = kmalloc(fsb->bytes_per_sector);
			if (!fsb->fat_cache[i])
				goto fail_cache;
			fsb->fat_cache_sector[i] = 0;
			fsb->fat_cache_valid[i] = 0;
			fsb->fat_cache_age[i] = 0;
		}
	}
	fsb->free_clusters = FAT_FSINFO_UNKNOWN;
	fat_load_fsinfo(sb);
	if (fsb->free_clusters == FAT_FSINFO_UNKNOWN) {
		error = fat_count_free_clusters(sb, &fsb->free_clusters);
		if (error)
			goto fail_cache;
	}

	sb->s_block_size = fsb->cluster_size;
	sb->s_magic = FAT_SUPER_MAGIC;
	sb->s_op = &fat_super_operations;
	sb->s_mounted = iget(sb, FAT_ROOT_INO);
	if (!sb->s_mounted)
		goto fail_cache;
	if (!fsb->readonly) {
		error = fat_set_clean_shutdown(sb, 0);
		if (error) {
			iput(sb->s_mounted);
			sb->s_mounted = NULL;
			goto fail_cache;
		}
		set_bit(SB_Dirty, &sb->s_state);
	}

	kfree(buf);
	return sb;

fail_cache:
	{
		int i;

		for (i = 0; i < FAT_FAT_CACHE_SLOTS; i++) {
			kfree(fsb->fat_cache[i]);
			fsb->fat_cache[i] = NULL;
		}
	}
fail:
	kfree(buf);
	return NULL;
}

static struct super_operations fat_super_operations = {
	.read_inode = fat_read_inode,
	.write_inode = fat_write_inode,
	.write_super = fat_write_super,
	.clear_inode = fat_clear_inode,
};

struct file_system_type fat = {
	.read_super = fat_read_super,
	.list = LIST_HEAD_INIT(fat.list),
	.name = "vfat",
};
