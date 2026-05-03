#ifndef _MARIOFS_H
#define _MARIOFS_H

/*
 * A file or a directory in MarioFS is organized as a block chain. The last 4 bytes
 * in a block is the next block number in the same chain, and a value
 * of 0 indicates the end of chain. All free blocks are in a chain of which
 * the first block number is free@mario_super_block. Mariofs doesn't support
 * hard link and soft link. Offset into rd of a directory entry is used as
 * inode number, kind of weird
 */

/*
 * Directory entry offset (the number of bytes from the begining) is used as inode number.
 *
 * For "." and ".." directory entries, the data field stores the inode number
 */

#define MARIO_MAGIC	0x4518cdef

#define MARIO_ROOT_BLOCK	1	/* the block number of root directory */

#define MARIO_NAME_LEN	30

struct mario_dir_entry {
	__u16 mode;
	__u16 flags;	/* Currently I don't care about this field */

	/* data block chain or device number (for special file) */
	__u32 data;
	/* bytes of data, for directoty and special file this field is 0 */
	__u32 size;
	/*
	 * the number of blocks occupied by this file, for directory and
	 * special file this field is 0
	 */
	__u32 blocks;

	/* access time and modification time. ctime (change time) is always the same as mtime */
	__s32 atime, mtime;

	char name[MARIO_NAME_LEN];
} __attribute__((gcc_struct, packed));

#define MARIO_ENTRY_SIZE	sizeof(struct mario_dir_entry)

/* special values of data@mario_dir_entry */
#define MARIO_ZERO_ENTRY	0xfefefefe	/* an empty file */
#define MARIO_FREE_ENTRY	0xffffffff	/* entry available */

struct mario_super_block {
	__u32 sector_size;	/* Bytes of a sector */
	__u32 block_size;	/* Bytes of a block */
	__u32 nr_blocks;	/* the number of blocks */
	__u32 nr_free;		/* the number of free blocks */
	__u32 magic;		/* mariofs magic number */
	__u32 free;		/* free block chain */
	__s64 time_base;
	/*
	 * root directory entry
	 */
	struct mario_dir_entry root;
} __attribute__((gcc_struct, packed));

#define MARIO_ROOT_INO	((size_t) &((struct mario_super_block *)0)->root)

struct mario_inode_info {
	__s32 atime, mtime;
	char name[MARIO_NAME_LEN];
};

#define MARIO_INODE_ATIME(i)	((i)->u.mario_i.atime)
#define MARIO_INODE_MTIME(i)	((i)->u.mario_i.mtime)
#define MARIO_INODE_NAME(i)	((i)->u.mario_i.name)

struct mario_sb_info {
	unsigned long sec_per_block;
	unsigned long nr_blocks;
	unsigned long nr_free;
	unsigned long free;
	__s64 time_base;
	struct mario_dir_entry root;
};

#define MARIO_SEC_PER_BLOCK(sb)	((sb)->u.mario_sb.sec_per_block)
#define MARIO_NR_BLOCKS(sb)	((sb)->u.mario_sb.nr_blocks)
#define MARIO_NR_FREE(sb)	((sb)->u.mario_sb.nr_free)
#define MARIO_FREE(sb)		((sb)->u.mario_sb.free)
#define MARIO_ROOT(sb)		((sb)->u.mario_sb.root)
#define MARIO_TIME_BASE(sb)		((sb)->u.mario_sb.time_base)

extern struct file_system_type mariofs;

#endif /* _MARIOFS_H */
