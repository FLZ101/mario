#ifndef _FS_FAT_FAT_H
#define _FS_FAT_FAT_H

#include <types.h>

/*
 * Read-only FAT32 filesystem support.
 *
 * FAT has no inode table.  The kernel uses the physical location of each
 * short-name directory entry as a synthetic inode number.  This lets the VFS
 * cache inodes and lets fat_read_inode() load metadata without first walking
 * the parent directory.
 */

struct super_block;
struct inode;
struct file_system_type;
struct inode_operations;
struct file_operations;

#define FAT_ROOT_INO		1
#define FAT_ENTRY_SIZE		32
#define FAT_SUPER_MAGIC		0x4d44

#define FAT_ATTR_READ_ONLY	0x01
#define FAT_ATTR_HIDDEN		0x02
#define FAT_ATTR_SYSTEM		0x04
#define FAT_ATTR_VOLUME_ID	0x08
#define FAT_ATTR_DIRECTORY	0x10
#define FAT_ATTR_ARCHIVE	0x20
#define FAT_ATTR_LFN		0x0f

#define FAT_EOC_MIN		0x0ffffff8UL
#define FAT_EOC_MAX		0x0fffffffUL
#define FAT_BAD_CLUSTER		0x0ffffff7UL

/* BPB field offsets in the first sector of the mounted partition. */
#define FAT_BPB_BYTES_PER_SECTOR	11
#define FAT_BPB_SECTORS_PER_CLUSTER	13
#define FAT_BPB_RESERVED_SECTORS	14
#define FAT_BPB_NUM_FATS		16
#define FAT_BPB_TOTAL_SECTORS16		19
#define FAT_BPB_TOTAL_SECTORS32		32
#define FAT_BPB_FAT_SIZE32		36
#define FAT_BPB_ROOT_CLUSTER		44
#define FAT_BPB_FSINFO			48

/*
 * Kernel-side mount state for one FAT32 filesystem.  The BPB is read manually
 * from a byte buffer because its fields are unaligned and must not be
 * interpreted through a packed C structure.
 */
struct fat_sb_info {
	uint16_t bytes_per_sector;
	uint8_t sectors_per_cluster;
	uint8_t num_fats;
	uint32_t reserved_sectors;
	uint32_t fat_size32;
	uint32_t total_sectors;
	uint32_t root_cluster;
	uint32_t first_data_sector;
	uint32_t data_sectors;
	uint32_t total_clusters;
	uint32_t cluster_size;
	unsigned long readonly;

	uint32_t fat_cache_sector;
	int fat_cache_valid;
	uint8_t *fat_cache;
};

struct fat_inode_info {
	uint32_t start_cluster;
	uint32_t dir_cluster;
	uint32_t dir_offset;
	uint8_t attributes;
};

#define FAT_SB(sb)			((sb)->u.fat_sb)
#define FAT_INODE(inode)		((inode)->u.fat_i)
#define FAT_CLUSTER_SIZE(sb)		FAT_SB(sb).cluster_size

/* FAT structures are little-endian on disk. */
static inline uint16_t fat_read16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t fat_read32(const uint8_t *p)
{
	return (uint32_t)p[0] |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

struct super_block *fat_read_super(struct super_block *sb);

int fat_read_cluster(struct super_block *sb, uint32_t cluster, void *buf);
int fat_read_fat_entry(struct super_block *sb, uint32_t cluster,
		       uint32_t *value);
int fat_next_cluster(struct super_block *sb, uint32_t cluster,
		     uint32_t *next);
uint32_t fat_cluster_at(struct inode *inode, uint32_t cluster_index);

uint32_t fat_entry_first_cluster(const uint8_t *entry);
uint32_t fat_entry_size(const uint8_t *entry);
unsigned long fat_entry_ino(struct super_block *sb, uint32_t cluster,
			    uint32_t offset);
int fat_ino_location(struct super_block *sb, unsigned long ino,
		     uint32_t *cluster, uint32_t *offset);

void fat_short_name_from_entry(const uint8_t *entry, char *name);
int fat_short_name_match(const char *name, int len, const uint8_t *entry);

int fat_read_inode(struct inode *inode);
void fat_clear_inode(struct inode *inode);

extern struct file_system_type fat;
extern struct inode_operations fat_file_iops;
extern struct inode_operations fat_dir_iops;
extern struct file_operations fat_file_fops;
extern struct file_operations fat_dir_fops;

#endif /* _FS_FAT_FAT_H */
