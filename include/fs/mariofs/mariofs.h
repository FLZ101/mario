#ifndef _MARIOFS_H
#define _MARIOFS_H

#include <fs/fs.h>

#include <types.h>

#define MARIO_MAGIC	0x4518qwer

#define MARIO_ROOT	2	/* the block number of root directory */

struct mario_super_block {
	__u32 sector_size;	/* Bytes of a sector */
	__u32 block_size;	/* Bytes of a block */
	__u32 nr_blocks;	/* number of blocks */
	__u32 nr_free;		/* number of free blocks */
	__u32 free;		/* free block chain */
	__u32 magic;
};

struct mario_sb_info {
	__u32 nr_blocks;
	__u32 nr_free;
	__u32 free;
};

struct mario_inode {
	int n;
};

struct mario_inode_info {
	int n;
};

extern struct super_block sb;

#endif /* _MARIOFS */