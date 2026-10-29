#ifndef _TTY_H
#define _TTY_H

#include <fs/fs.h>
#include <fs/tty/termios.h>
#include <lib/ring_buffer.h>

struct tty_driver {
	struct tty_driver *next;

	dev_t major;
	dev_t minor; // minor of the first device
	dev_t n; // max number of devices

	struct tty_struct *tty_table;
	struct file_operations *fops;
	void (*put_char)(struct tty_struct *tty, unsigned char c);
};

void register_tty_driver(struct tty_driver *);
int get_tty(dev_t dev, struct tty_struct **tty);
int tty_bind_file(struct tty_struct *tty, struct file *f);

extern struct file_operations tty_fops;

struct tty_struct {
	dev_t dev;
	pid_t session, pgrp;
	struct tty_driver *driver;
	struct termios termios;

	struct winsize winsize;

	struct ring_buffer read_buf;
	wait_queue_t wait_read;
	/* Serialises read_buf and termios between reader context and IRQ input. */
	spinlock_t lock;

	int count;
	int initialized;
};

extern struct termios default_termios;

void tty_receive_c(struct tty_struct *tty, unsigned char c);
void tty_receive_s(struct tty_struct *tty, char *s);

#endif /* _TTY_H */
