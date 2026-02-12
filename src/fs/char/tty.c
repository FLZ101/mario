#include <fs/fs.h>
#include <fs/tty/tty.h>

#include <misc.h>

#include <mm/page_alloc.h>

struct termios default_termios = {
	.c_cflag = 0,
	.c_iflag = 0,
	.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | TOSTOP,
	.c_oflag = 0,
	.c_cc = {
		003, // Ctrl-C
		034, /* Ctrl-\ */
		010, // Ctrl-H
		025, // Ctrl-U, Ctrl-X
		004, // Ctrl-D
		0,
		0,
		0,
		021, // Ctrl-Q
		023, // Ctrl-S
		032, // Ctrl-Z
		000, // NUL
	 }
};

void tty_receive_s(struct tty_struct *tty, char *s)
{
	char c;
	while ((c = *s++))
		tty_receive_c(tty, c);
}

void tty_put_c(struct tty_struct *tty, char c)
{
	tty->driver->put_char(tty, c);
}

void tty_put_s(struct tty_struct *tty, char *s)
{
	char c;
	while ((c = *s++))
		tty_put_c(tty, c);
}

void tty_receive_c(struct tty_struct *tty, char c)
{
	struct ring_buffer *rb = &tty->read_buf;

	ACQUIRE_LOCK(&tty->lock);

	int wake = 0;

	tcflag_t c_lflag = tty->termios.c_lflag;
	cc_t *c_cc = tty->termios.c_cc;

	if (c_lflag & ISIG) {
		if (c == c_cc[VINTR]) {
			kill_pg(SIGINT, tty->pgrp, 0);
			goto tail;
		} else if (c == c_cc[VQUIT]) {
			kill_pg(SIGQUIT, tty->pgrp, 0);
			goto tail;
		} else if (c == c_cc[VSUSP]) {
			kill_pg(SIGTSTP, tty->pgrp, 0);
			goto tail;
		}
	}

	if (c_lflag & ICANON) {
		if (c == c_cc[VERASE]) {
			ring_buffer_pop(rb);

			if (c_lflag & ECHO) {
				if (c_lflag & ECHOE) {
					tty_put_c(tty, '\b');
					goto tail;
				}
			}
		}

		if (c == c_cc[VKILL]) {
			while (1) {
				int ch = ring_buffer_peek(rb);
				// empty
				if (ch < 0)
					break;
				if (ch == c_cc[VEOF] || ch == '\n' || ch == c_cc[VEOL])
					break;
				ring_buffer_pop(rb);
			}
			goto tail;
		}

		if (c == c_cc[VEOF] || c == '\n' || c == c_cc[VEOL]) {
			wake = 1;
		}
	} else {
		wake = 1;
	}

	ring_buffer_write(rb, &c, 1, 0);
	if (c_lflag & ECHO) {
		if (0 <= c && c <= 037) {
			// control codes
			if (c_lflag & ECHOCTL) {
				tty_put_c(tty, c + 0x40);
			}
		} else {
			tty_put_c(tty, c);
		}
	}

tail:
	if (wake)
		wake_up_interruptible(&tty->wait_read);
	RELEASE_LOCK(&tty->lock);
}

static struct tty_driver *tty_drivers = NULL;

void register_tty_driver(struct tty_driver *driver)
{
	driver->next = tty_drivers;
	tty_drivers = driver;
}

int get_tty(dev_t minor, struct tty_struct **tty)
{
	for (struct tty_driver *p = tty_drivers; p; p = p->next) {
		if (p->minor <= minor && minor < p->minor + p->n) {
			*tty = &p->tty_table[minor - p->minor];
			return 0;
		}
	}
	return -EINVAL;
}

int tty_open(struct inode *i, struct file *f)
{
	dev_t dev = i->i_rdev;
	dev_t minor = MINOR(dev);

	if (minor == TTY_MINOR) {
		if (!current->tty)
			return -ENXIO;
		dev = current->tty->dev;
		minor = MINOR(dev);
	}

	struct tty_struct* tty = NULL;
	int err = get_tty(minor, &tty);
	if (err)
		return err;

	f->private_data = tty;

	int noctty = f->f_flags & O_NOCTTY;
	if (!noctty &&
	    current->leader &&
	    !current->tty &&
	    tty->session == 0) {
		current->tty = tty;
		tty->session = current->session;
		tty->pgrp = current->pgrp;
	}
	return 0;
}

// return maximum number of bytes to read in canonical mode
int max_canon_read(struct ring_buffer *rb, cc_t *c_cc) {
	size_t mask = rb->size - 1;
	size_t p = rb->head;
	int n = 0;

	while (p != rb->tail) {
		uint8_t ch = (uint8_t) rb->data[p];

		p = (p + 1) & mask;
		++n;

		if (ch == '\n' || ch == c_cc[VEOF] || ch == c_cc[VEOL])
			break;
	}
	return n;
}

int tty_read(struct inode *i, struct file *f, char *buf, int count)
{
	struct tty_struct *tty = (struct tty_struct *)f->private_data;
	if (!tty)
		return -EIO;

	if (current->tty != tty)
		return -EPERM;

	if (current->pgrp != tty->pgrp) {
		kill_pg(current->pgrp, SIGTTIN, 1);
		return -ERESTARTSYS;
	}

	struct ring_buffer *rb = &tty->read_buf;

try:
	ACQUIRE_LOCK(&tty->lock);

	tcflag_t c_lflag = tty->termios.c_lflag;
	cc_t *c_cc = tty->termios.c_cc;
	int n = 0;
	if (c_lflag & ICANON) {
		while (!(n = max_canon_read(rb, c_cc))) {
			sleep_on(&tty->wait_read, TASK_INTERRUPTIBLE, &tty->lock);
			goto try;
		}
	} else {
		while (!(n = ring_buffer_avail(rb))) {
			sleep_on(&tty->wait_read, TASK_INTERRUPTIBLE, &tty->lock);
			goto try;
		}
	}

	n = MIN(n, count);

	int m = ring_buffer_read(rb, buf, n, 1);
	early_assert(n == m);

	if (buf[n-1] == c_cc[VEOF] || buf[n-1] == c_cc[VEOL])
		--n;

	RELEASE_LOCK(&tty->lock);
	return n;
}

int tty_write(struct inode *i, struct file *f, char *buf, int count)
{
	struct tty_struct *tty = (struct tty_struct *)f->private_data;
	if (!tty)
		return -EIO;

	if (current->tty != tty)
		return -EPERM;

	if (current->pgrp != tty->pgrp) {
		kill_pg(current->pgrp, SIGTTOU, 1);
		return -ERESTARTSYS;
	}

	ACQUIRE_LOCK(&tty->lock);

	int n = tty->driver->write(tty, (unsigned char *) buf, count);

	RELEASE_LOCK(&tty->lock);

	return n;
}

static int tty_lseek(struct inode *i, struct file *f, off_t offset, int orig)
{
	return -ESPIPE;
}

static int tty_ioctl(struct inode *i, struct file *f, unsigned int cmd, unsigned long arg)
{
	return 0;
}

static void tty_release(struct inode *i, struct file *f)
{
}

struct file_operations tty_fops = {
	.lseek = tty_lseek,
	.open = tty_open,
	.read = tty_read,
	.write = tty_write,
	.release = tty_release,
	.ioctl = tty_ioctl
};

extern void ps2_init(void);
extern void console_init(void);
extern void uart_init(void);

void __tinit tty_init(void)
{
	ps2_init();
	console_init();
	uart_init();
	register_chrdev(TTY_MAJOR, &tty_fops);
}
