#ifndef _FS_FAT_FAT_H
#define _FS_FAT_FAT_H

#include <types.h>

/*
 * FAT32 filesystem support.
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
#define FAT_BPB_CLEAN_SHUTDOWN		0x41

/* FAT32 FSInfo sector layout. */
#define FAT_FSINFO_LEAD_SIG_OFFSET	0
#define FAT_FSINFO_STRUCT_SIG_OFFSET	484
#define FAT_FSINFO_FREE_COUNT_OFFSET	488
#define FAT_FSINFO_NEXT_FREE_OFFSET	492
#define FAT_FSINFO_TRAIL_SIG_OFFSET	508
#define FAT_FSINFO_LEAD_SIG		0x41615252UL
#define FAT_FSINFO_STRUCT_SIG		0x61417272UL
#define FAT_FSINFO_TRAIL_SIG		0xaa550000UL
#define FAT_FSINFO_UNKNOWN		0xffffffffUL

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
	uint32_t fsinfo_sector;
	uint32_t first_data_sector;
	uint32_t data_sectors;
	uint32_t total_clusters;
	uint32_t cluster_size;
	unsigned long readonly;
	uint32_t next_free;
	uint32_t free_clusters;

	uint32_t fat_cache_sector;
	int fat_cache_valid;
	uint8_t *fat_cache;
};

struct fat_inode_info {
	uint32_t start_cluster;
	uint32_t dir_cluster;
	uint32_t dir_offset;
	uint32_t parent_ino;
	uint8_t attributes;
	uint32_t chain_cache_start;
	uint32_t chain_cache_index;
	uint32_t chain_cache_cluster;
	int chain_cache_valid;
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

static inline void fat_write16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static inline void fat_write32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

struct super_block *fat_read_super(struct super_block *sb);

int fat_read_cluster(struct super_block *sb, uint32_t cluster, void *buf);
int fat_write_cluster(struct super_block *sb, uint32_t cluster,
		      const void *buf);
int fat_read_fat_entry(struct super_block *sb, uint32_t cluster,
		       uint32_t *value);
int fat_write_fat_entry(struct super_block *sb, uint32_t cluster,
			uint32_t value);
int fat_load_fsinfo(struct super_block *sb);
int fat_write_fsinfo(struct super_block *sb);
int fat_next_cluster(struct super_block *sb, uint32_t cluster,
		     uint32_t *next);
int fat_count_free_clusters(struct super_block *sb, uint32_t *free_count);
int fat_count_directory_entries(struct super_block *sb, uint32_t *used,
				uint32_t *free);
uint32_t fat_cluster_at(struct inode *inode, uint32_t cluster_index);
uint32_t fat_cluster_at_alloc(struct inode *inode, uint32_t cluster_index,
			      int *error);
uint32_t fat_cluster_count(struct inode *inode);
int fat_alloc_cluster(struct super_block *sb, uint32_t previous,
		      uint32_t *cluster);
int fat_free_cluster_chain(struct super_block *sb, uint32_t cluster);

uint32_t fat_entry_first_cluster(const uint8_t *entry);
uint32_t fat_entry_size(const uint8_t *entry);
unsigned long fat_entry_ino(struct super_block *sb, uint32_t cluster,
			    uint32_t offset);
int fat_ino_location(struct super_block *sb, unsigned long ino,
		     uint32_t *cluster, uint32_t *offset);
int fat_make_short_entry(uint8_t *entry, const uint8_t short_name[11],
			 uint8_t attributes, uint32_t start_cluster,
			 uint32_t size);
void fat_entry_set_create_times(uint8_t *entry, uint64_t time);
void fat_entry_set_modify_times(uint8_t *entry, uint64_t atime,
				uint64_t mtime);

void fat_short_name_from_entry(const uint8_t *entry, char *name);
int fat_short_name_match(const char *name, int len, const uint8_t *entry);
int fat_name_to_short(const char *name, int len, uint8_t short_name[11]);
int fat_short_name_equal(const uint8_t *entry, const uint8_t short_name[11]);

int fat_read_inode(struct inode *inode);
int fat_write_inode(struct inode *inode);
void fat_clear_inode(struct inode *inode);

extern struct file_system_type fat;
extern struct inode_operations fat_file_iops;
extern struct inode_operations fat_dir_iops;
extern struct file_operations fat_file_fops;
extern struct file_operations fat_dir_fops;

#endif /* _FS_FAT_FAT_H */
