#ifndef _DEVICE_H
#define _DEVICE_H

#define MAJOR(dev)	((unsigned int)((dev) >> 8))
#define MINOR(dev)	((unsigned int)((dev) & 0xff))
#define MKDEV(major, minor)	(((major) << 8) | (minor))
#define NODEV	0

#endif /* _DEVICE_H */