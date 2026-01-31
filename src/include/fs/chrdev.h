#ifndef _CHRDEV_H
#define _CHRDEV_H

#include <fs/device.h>

#include <types.h>

struct file_operations;

void chrdev_init(void);

int register_chrdev(unsigned int, struct file_operations *);

#define MAX_CHRDEV	64

#define MEM_MAJOR	1

#define MEM_MINOR_ZERO	0	// /dev/zero
#define MEM_MINOR_NULL	1	// /dev/null

#define TTY_MAJOR	2

#define TTY_MINOR_0		0	// /dev/tty0
#define TTY_MINOR_MAX	6	// /dev/tty6

#define TTY_MINOR_S_0	64	// /dev/ttyS0
#define TTY_MINOR_S_MAX	67	// /dev/ttyS3

#define TTY_MINOR		128	// /dev/tty

#endif /* _CHRDEV_H */
