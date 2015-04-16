#ifndef _FS_H
#define _FS_H

#include <types.h>

#include <fs/blkdev.h>
#include <fs/buffer.h>

#define MAJOR(dev)	((unsigned int)((dev) >> 8))
#define MINOR(dev)	((unsigned int)((dev) & 0xff))
#define MKDEV(major, minor)	(((major) << 8) | (minor))
#define NODEV	0


#endif /* _FS_H */