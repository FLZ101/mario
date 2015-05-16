#ifndef _MMAN_H
#define _MMAN_H

#define PROT_NONE	0x00
#define PROT_READ	0x01
#define PROT_WRITE	0x02
#define PROT_EXEC	0x04

#define MAP_SHARED	0x01
#define MAP_PRIVATE	0x02
#define MAP_TYPE	0x0f
#define MAP_FIXED	0x10
#define MAP_ANONYMOUS	0x20

#define MS_ASYNC	1	/* sync memory asynchronously */
#define MS_INVALIDATE	2	/* invalidate the caches */
#define MS_SYNC		4	/* synchronous memory sync */

struct mmap_arg_struct {
	unsigned long addr;
	unsigned long len;
	unsigned long prot;
	unsigned long flags;
	unsigned long fd;
	unsigned long offset;
};

#endif /* _MMAN_H */