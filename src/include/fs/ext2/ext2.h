#ifndef _FS_EXT2_EXT2_H
#define _FS_EXT2_EXT2_H

#include <types.h>

/*
 * On-disk ext2 structures and the per-superblock/per-inode state kept by the
 * kernel driver.  All fields are little-endian; the read/write helpers below
 * convert them on i386 without relying on structure assignment.
 */
struct super_block;
struct inode;
struct file_system_type;
struct inode_operations;
struct file_operations;

#define EXT2_SUPER_MAGIC		0xef53
#define EXT2_ROOT_INO			2
#define EXT2_GOOD_OLD_INODE_SIZE	128
#define EXT2_GOOD_OLD_REV		0
#define EXT2_DYNAMIC_REV		1
#define EXT2_NAME_LEN			255

#define EXT2_DIR_REC_LEN(name_len)	(((name_len) + 8 + 3) & ~3)

#define EXT2_NDIR_BLOCKS		12
#define EXT2_IND_BLOCK			EXT2_NDIR_BLOCKS
#define EXT2_DIND_BLOCK			(EXT2_IND_BLOCK + 1)
#define EXT2_TIND_BLOCK			(EXT2_DIND_BLOCK + 1)
#define EXT2_N_BLOCKS			(EXT2_TIND_BLOCK + 1)

#define EXT2_FT_UNKNOWN		0
#define EXT2_FT_REG_FILE	1
#define EXT2_FT_DIR		2
#define EXT2_FT_CHRDEV		3
#define EXT2_FT_BLKDEV		4
#define EXT2_FT_FIFO		5
#define EXT2_FT_SOCK		6
#define EXT2_FT_SYMLINK		7

#define EXT2_BTREE_FL		0x1000

#define EXT2_FEATURE_COMPAT_DIR_PREALLOC	0x0001
#define EXT2_FEATURE_COMPAT_IMAGIC_INODES	0x0002
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL		0x0004
#define EXT2_FEATURE_COMPAT_EXT_ATTR		0x0008
#define EXT2_FEATURE_COMPAT_RESIZE_INODE	0x0010
#define EXT2_FEATURE_COMPAT_DIR_INDEX		0x0020

#define EXT2_FEATURE_INCOMPAT_COMPRESSION	0x0001
#define EXT2_FEATURE_INCOMPAT_FILETYPE		0x0002
#define EXT2_FEATURE_INCOMPAT_RECOVER		0x0004
#define EXT2_FEATURE_INCOMPAT_JOURNAL_DEV	0x0008
#define EXT2_FEATURE_INCOMPAT_META_BG		0x0010

#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER	0x0001
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE	0x0002

/* ext2 superblock, located at byte offset 1024 of the block device. */
struct ext2_super_block {
	__u32 s_inodes_count;
	__u32 s_blocks_count;
	__u32 s_r_blocks_count;
	__u32 s_free_blocks_count;
	__u32 s_free_inodes_count;
	__u32 s_first_data_block;
	__u32 s_log_block_size;
	__s32 s_log_frag_size;
	__u32 s_blocks_per_group;
	__u32 s_frags_per_group;
	__u32 s_inodes_per_group;
	__u32 s_mtime;
	__u32 s_wtime;
	__u16 s_mnt_count;
	__s16 s_max_mnt_count;
	__u16 s_magic;
	__u16 s_state;
	__u16 s_errors;
	__u16 s_minor_rev_level;
	__u32 s_lastcheck;
	__u32 s_checkinterval;
	__u32 s_creator_os;
	__u32 s_rev_level;
	__u16 s_def_resuid;
	__u16 s_def_resgid;
	__u32 s_first_ino;
	__u16 s_inode_size;
	__u16 s_block_group_nr;
	__u32 s_feature_compat;
	__u32 s_feature_incompat;
	__u32 s_feature_ro_compat;
	__u8 s_uuid[16];
	__u8 s_volume_name[16];
	__u8 s_last_mounted[64];
	__u32 s_algorithm_usage_bitmap;
} __attribute__((packed));

/* Standard 32-byte group descriptor used by non-64-bit ext2 filesystems. */
struct ext2_group_desc {
	__u32 bg_block_bitmap;
	__u32 bg_inode_bitmap;
	__u32 bg_inode_table;
	__u16 bg_free_blocks_count;
	__u16 bg_free_inodes_count;
	__u16 bg_used_dirs_count;
	__u16 bg_pad;
	__u32 bg_reserved[3];
} __attribute__((packed));

/* Fixed portion of an ext2 inode.  Extra fields beyond the first 128 bytes
 * are not needed while 64-bit and extents are rejected. */
struct ext2_inode {
	__u16 i_mode;
	__u16 i_uid;
	__u32 i_size;
	__u32 i_atime;
	__u32 i_ctime;
	__u32 i_mtime;
	__u32 i_dtime;
	__u16 i_gid;
	__u16 i_links_count;
	__u32 i_blocks;
	__u32 i_flags;
	__u32 i_osd1;
	__u32 i_block[EXT2_N_BLOCKS];
	__u32 i_generation;
	__u32 i_file_acl;
	__u32 i_dir_acl;
	__u32 i_faddr;
} __attribute__((packed));

/* Variable-length directory entry.  The name follows the fixed header. */
struct ext2_dir_entry_2 {
	__u32 inode;
	__u16 rec_len;
	__u8 name_len;
	__u8 file_type;
	char name[];
} __attribute__((packed));

/* Kernel-side mount state for one ext2 filesystem. */
struct ext2_sb_info {
	uint32_t block_size;
	uint32_t first_data_block;
	uint32_t blocks_count;
	uint32_t inodes_count;
	uint32_t free_blocks_count;
	uint32_t free_inodes_count;
	uint32_t blocks_per_group;
	uint32_t inodes_per_group;
	uint32_t nr_groups;
	uint32_t inode_size;
	uint32_t desc_per_block;
	uint32_t incompat_features;
	uint32_t ro_compat_features;
	struct ext2_group_desc *group_desc;
	unsigned long readonly;
};

/* Keep the raw on-disk inode so write support can preserve unknown fields. */
struct ext2_inode_info {
	struct ext2_inode raw;
};

#define EXT2_SB(sb)			((sb)->u.ext2_sb)
#define EXT2_INODE(inode)		((inode)->u.ext2_i)
#define EXT2_BLOCK_SIZE(sb)		EXT2_SB(sb).block_size
#define EXT2_BLOCKS_PER_GROUP(sb)	EXT2_SB(sb).blocks_per_group
#define EXT2_INODES_PER_GROUP(sb)	EXT2_SB(sb).inodes_per_group
#define EXT2_NR_GROUPS(sb)		EXT2_SB(sb).nr_groups
#define EXT2_GROUP_DESC(sb)		EXT2_SB(sb).group_desc

/* ext2 is always little-endian on disk. */
static inline uint16_t ext2_read16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t ext2_read32(const uint8_t *p)
{
	return (uint32_t)p[0] |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static inline void ext2_write16(uint8_t *p, uint16_t value)
{
	p[0] = value & 0xff;
	p[1] = value >> 8;
}

static inline void ext2_write32(uint8_t *p, uint32_t value)
{
	p[0] = value & 0xff;
	p[1] = (value >> 8) & 0xff;
	p[2] = (value >> 16) & 0xff;
	p[3] = (value >> 24) & 0xff;
}

int ext2_read_block(struct super_block *sb, uint32_t block, void *buf);
int ext2_write_block(struct super_block *sb, uint32_t block, const void *buf);

uint32_t ext2_bmap(struct inode *inode, uint32_t block, int create);
int ext2_alloc_block(struct super_block *sb, uint32_t goal, uint32_t *block);
int ext2_free_block(struct super_block *sb, uint32_t block);
int ext2_alloc_inode(struct super_block *sb, uint16_t mode, uint32_t *ino);
int ext2_free_inode(struct super_block *sb, uint32_t ino);
int ext2_write_group_desc(struct super_block *sb, uint32_t group);

int ext2_read_inode(struct inode *inode);
int ext2_write_inode(struct inode *inode);
void ext2_clear_inode(struct inode *inode);
int ext2_write_super(struct super_block *sb);
struct super_block *ext2_read_super(struct super_block *sb);
int ext2_truncate(struct inode *inode, int size);

extern struct file_system_type ext2;
extern struct inode_operations ext2_file_iops;
extern struct inode_operations ext2_dir_iops;
extern struct inode_operations ext2_link_iops;
extern struct file_operations ext2_file_fops;
extern struct file_operations ext2_dir_fops;

#endif /* _FS_EXT2_EXT2_H */
