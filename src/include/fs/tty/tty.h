#ifndef _TTY_H
#define _TTY_H

#include <fs/fs.h>
#include <fs/tty/termios.h>
#include <lib/ring_buffer.h>

struct tty_driver {
	struct tty_driver *next;

	dev_t minor; // minor of the first device
	dev_t n; // max number of devices

	struct tty_struct *tty_table;
	int (*write)(struct tty_struct *tty, unsigned char *buf, int count);
	void (*put_char)(struct tty_struct *tty, unsigned char c);
};

void register_tty_driver(struct tty_driver *);

struct tty_struct {
	dev_t dev;
	pid_t session, pgrp;
	struct tty_driver *driver;
	struct termios termios;

	struct winsize winsize;

	struct ring_buffer read_buf;
	spinlock_t lock;
	wait_queue_t wait_read;
};

extern struct termios default_termios;

void tty_receive_c(struct tty_struct *tty, char c);
void tty_receive_s(struct tty_struct *tty, char *s);

#endif /* _TTY_H */
