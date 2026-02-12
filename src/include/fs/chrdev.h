#ifndef _CHRDEV_H
#define _CHRDEV_H

#include <fs/device.h>

#include <types.h>

struct file_operations;

void chrdev_init(void);

int register_chrdev(unsigned int, struct file_operations *);

#define MAX_CHRDEV	16

#define MEM_MAJOR	1

#define MEM_MINOR_ZERO	0	// /dev/zero
#define MEM_MINOR_NULL	1	// /dev/null

#define TTY_MAJOR	2

#define TTY_MINOR_0	0	// /dev/tty0. current console
#define TTY_MINOR_1	1	// /dev/tty1. first console
#define NUM_CONSOLE 6

#define TTY_MINOR_S_0	64	// /dev/ttyS0. first serial
#define NUM_SERIAL	4

#define TTY_MINOR		128	// /dev/tty. current tty

#endif /* _CHRDEV_H */
