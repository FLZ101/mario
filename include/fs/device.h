#ifndef _DEVICE_H
#define _DEVICE_H

#include <fs/fs.h>

typedef unsigned short dev_t;

#define MAJOR(a)	(int)((dev_t)(a) >> 8)
#define MINOR(a)	(int)((dev_t)(a) & 0xff)
#define MKDEV(a,b)	((int)((((a) & 0xff) << 8) | ((b) & 0xff)))
#define NODEV(a,b)	MKDEV(0,0)

struct device_struct {
	dev_t dev;
	struct file_operations *fops;
};

#define RD_MAJOR	1

#endif /* _DEVICE_H */