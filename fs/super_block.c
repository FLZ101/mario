#include <fs/fs.h>

#define NR_SUPER	32

struct super_block super_blocks[NR_SUPER];

struct super_block *read_super(struct super_block *sb, void *data)
{
	return NULL;
}