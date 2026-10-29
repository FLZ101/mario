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
	struct tty_struct *(*lookup)(dev_t minor);
	int (*bind)(struct tty_struct *tty, struct file *f);
	int (*write)(struct tty_struct *tty, const char *buf, int count);
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
	wait_queue_t wait_write;
	/* Serialises read_buf and termios between reader context and IRQ input. */
	spinlock_t lock;

	int count;
	int initialized;
};

extern struct termios default_termios;

int tty_init_struct(struct tty_struct *tty, dev_t dev,
			    struct tty_driver *driver, unsigned int rows,
			    unsigned int cols);
int tty_output_char(struct tty_struct *tty, unsigned char c, char output[2]);
int tty_put_c(struct tty_struct *tty, unsigned char c);
void tty_put_s(struct tty_struct *tty, char *s);
void tty_receive_c(struct tty_struct *tty, unsigned char c);
void tty_receive_s(struct tty_struct *tty, char *s);
int tty_read(struct inode *i, struct file *f, char *buf, int count);
int tty_write(struct inode *i, struct file *f, char *buf, int count);
int tty_ioctl(struct inode *i, struct file *f, unsigned int cmd, unsigned long arg);
int tty_poll(struct file *f, struct poll_context *ctx);

#endif /* _TTY_H */
