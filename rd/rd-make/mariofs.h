#ifndef _MARIOFS_H
#define _MARIOFS_H

#include <stdint.h>

/*
 * A file or a directory in MarioFS is organized as a block chain. The last 4 bytes
 * in a block is the next block number in the same chain, and a value
 * of 0 indicates the end of chain. All free blocks are in a chain of which
 * the first block number is free@mario_super_block.
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
	uint16_t mode;
	uint16_t flags;

	/* data block chain or device number (for special file) */
	uint32_t data;
	/* bytes of data, for directoty and special file this field is 0 */
	uint32_t size;
	/*
	 * the number of blocks occupied by this file, for directory and
	 * special file this field is 0
	 */
	uint32_t blocks;
	char name[MARIO_NAME_LEN];
} __attribute__((gcc_struct, packed));

/* directory entry modes */

#define MODE_DIR 0040000
#define MODE_CHR 0020000
#define MODE_BLK 0060000
#define MODE_REG 0100000
#define MODE_IFO 0010000
#define MODE_LNK 0120000
#define MODE_SOCK 0140000

#define MARIO_ZERO_ENTRY	0xfefefefe

/* directory entry flags */
#define FLAG_R	1
#define FLAG_W	2
#define FLAG_X	4

struct mario_super_block {
	uint32_t sector_size;	/* Bytes of a sector */
	uint32_t block_size;	/* Bytes of a block */
	uint32_t nr_blocks;	/* the number of blocks */
	uint32_t nr_free;	/* the number of free blocks */
	uint32_t magic;		/* mariofs magic number */
	uint32_t free;		/* free block chain */
	struct mario_dir_entry root;	/* root directory */
} __attribute__((gcc_struct, packed));

// directory entry offset of /
#define MARIO_ROOT_INO	((size_t) &((struct mario_super_block *)0)->root)

#endif
