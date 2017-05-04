#ifndef _CHRDEV_H
#define _CHRDEV_H

#include <fs/device.h>

#include <types.h>

struct chrdev_operations {
	int (*chrdev_open)(dev_t dev);
	int (*chrdev_read)(dev_t dev, char *c);
	int (*chrdev_write)(dev_t dev, char *c);
};

int register_chrdev(unsigned int, struct chrdev_operations *);

void chrdev_init(void);
int check_chrdev(unsigned int major);
int chrdev_open(dev_t dev);
int chrdev_read(dev_t dev, char *c);
int chrdev_write(dev_t dev, char *c);

#define MAX_CHRDEV	64

#define MEM_MAJOR	1
#define TTY_MAJOR	2
#define VCS_MAJOR	3

#endif /* _CHRDEV_H */