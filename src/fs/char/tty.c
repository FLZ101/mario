#include <fs/fs.h>
#include <fs/tty/tty.h>

#include <misc.h>

#include <mm/page_alloc.h>

struct termios default_termios = {
	.c_cflag = 0,
	.c_iflag = ICRNL,
	.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | TOSTOP,
	.c_oflag = OPOST | ONLCR,
	.c_cc = {
		003, 	// Ctrl-C
		034, 	/* Ctrl-\ */
		0x7f, 	// Delete
		025, 	// Ctrl-U, Ctrl-X
		004, 	// Ctrl-D
		0,
		0,
		0,
		021, 	// Ctrl-Q
		023, 	// Ctrl-S
		032, 	// Ctrl-Z
		000, 	// NUL
	 }
};

void tty_put_c(struct tty_struct *tty, unsigned char c)
{
	tcflag_t c_oflag = tty->termios.c_oflag;
	if (c_oflag & OPOST) {
		if (c == '\n') {
			if (c_oflag & ONLCR) {
				tty->driver->put_char(tty, '\r');
			}
		}
	}
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

	tcflag_t c_iflag = tty->termios.c_iflag;
	tcflag_t c_lflag = tty->termios.c_lflag;
	cc_t *c_cc = tty->termios.c_cc;

	if (c_lflag & ISIG) {
		if (c == c_cc[VINTR]) {
			kill_pg(tty->pgrp, SIGINT, 0);
			goto tail_1;
		} else if (c == c_cc[VQUIT]) {
			kill_pg(tty->pgrp, SIGQUIT, 0);
			goto tail_1;
		} else if (c == c_cc[VSUSP]) {
			kill_pg(tty->pgrp, SIGTSTP, 0);
			goto tail_1;
		}
	}

	if (c_lflag & ICANON) {
		if (c == '\r') {
			if (c_iflag & IGNCR)
				goto tail_2;
			if (c_iflag & ICRNL)
				c = '\n';
		} else if (c == '\n' && (c_iflag & INLCR)) {
			c = '\r';
		}

		if (c == c_cc[VERASE]) {
			ring_buffer_pop(rb);
			goto tail_1;
		}

		if (c == c_cc[VEOF] || c == '\n' || c == '\r' || c == c_cc[VEOL]) {
			wake = 1;
		}
	} else {
		wake = 1;
	}

	ring_buffer_write(rb, &c, 1, 0);
tail_1:
	if (c_lflag & ECHO) {
		if (c == c_cc[VERASE] && c_lflag & ECHOE) {
			tty_put_s(tty, "\b \b");
			goto tail_2;
		}

		if (c_lflag & ECHOCTL) {
			// ASCII control characters (0–31 and 127) other than TAB, NL, and CR are
			// echoed as ^X, where X is the character obtained by adding 0x40 to
			// the control character.
			if (0 <= c && c <= 31 && c != '\t' && c != '\n' && c != '\r') {
				tty_put_c(tty, '^');
				tty_put_c(tty, c + 0x40);
				goto tail_2;
			}
			if (c == 127) {
				tty_put_s(tty, "^?");
				goto tail_2;
			}
		}
		tty_put_c(tty, c);
	}

tail_2:
	if (wake)
		wake_up_interruptible(&tty->wait_read);
	RELEASE_LOCK(&tty->lock);
}

void tty_receive_s(struct tty_struct *tty, char *s)
{
	char c;
	while ((c = *s++))
		tty_receive_c(tty, c);
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
	if (current->signal & ~current->blocked)
		return -ERESTARTSYS;

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
	assert(n == m);

	// Except in the case of EOF, the line delimiter is included in the buffer returned by read(2).
	if (buf[n-1] == c_cc[VEOF])
		--n;

	RELEASE_LOCK(&tty->lock);
	return n;
}

static int tty_check(struct tty_struct *tty)
{
	if (!tty)
		return -EIO;

	if (current->tty != tty)
		return -EPERM;

	if (current->pgrp != tty->pgrp) {
		kill_pg(current->pgrp, SIGTTOU, 1);
		return -ERESTARTSYS;
	}
	return 0;
}

int tty_write(struct inode *i, struct file *f, char *buf, int count)
{
	struct tty_struct *tty = (struct tty_struct *)f->private_data;
	int err = tty_check(tty);
	if (err)
		return err;

	ACQUIRE_LOCK(&tty->lock);

	int n = tty->driver->write(tty, (unsigned char *) buf, count);

	RELEASE_LOCK(&tty->lock);

	return n;
}

static int tty_lseek(struct inode *i, struct file *f, off_t offset, int orig)
{
	return -ESPIPE;
}

static int set_termios(struct tty_struct * tty, unsigned long arg, int opt)
{
	struct termios tmp_termios;
	int err = verify_area(VERIFY_READ, (void *) arg, sizeof(struct termios));
	if (err)
		return err;
	memcpy_fromfs(&tmp_termios, (struct termios *) arg, sizeof (struct termios));

	cli();
	tty->termios = tmp_termios;
	sti();
	return 0;
}

int session_of_pgrp(int pgrp)
{
	struct task_struct *p;
	int fallback;

	fallback = -1;
	for_each_task(p) {
 		if (p->session <= 0)
 			continue;
		if (p->pgrp == pgrp)
			return p->session;
		if (p->pid == pgrp)
			fallback = p->session;
	}
	return fallback;
}

static int tty_ioctl(struct inode *i, struct file *f, unsigned int cmd, unsigned long arg)
{
	int opt = 0;
	struct tty_struct *tty = (struct tty_struct *) f->private_data;
	int err = tty_check(tty);
	if (err)
		return err;

	switch (cmd) {
		case TCGETS:
			err = verify_area(VERIFY_WRITE, (void *) arg, sizeof (struct termios));
			if (err)
				return err;
			memcpy_tofs((struct termios *) arg, &tty->termios, sizeof (struct termios));
			return 0;
		case TCSETSF: // Allow the output buffer to drain, discard pending input
		case TCSETSW: // Allow the output buffer to drain
		case TCSETS:
			return set_termios(tty, arg, opt);
		case TIOCGWINSZ:
			err = verify_area(VERIFY_WRITE, (void *) arg, sizeof (struct winsize));
			if (err)
				return err;
			memcpy_tofs((struct winsize *) arg, &tty->winsize, sizeof (struct winsize));
			return 0;
		case TIOCSWINSZ:
			return -EINVAL;
		case TIOCSTI:
			if (current->tty != tty)
				return -EPERM;
			err = verify_area(VERIFY_READ, (void *) arg, 1);
			if (err)
				return err;
			unsigned char ch = get_fs_byte((char *) arg);
			tty_put_c(tty, ch);
			return 0;
		case TIOCSCTTY:
			return -EINVAL;
		case TIOCNOTTY:
			return -EINVAL;
		case TIOCGPGRP:
			if (current->tty != tty)
				return -ENOTTY;
			err = verify_area(VERIFY_WRITE, (void *) arg, sizeof (pid_t));
			if (err)
				return err;
			put_fs_long(tty->pgrp, (pid_t *) arg);
			return 0;
		case TIOCSPGRP:
			if (!current->tty ||
			    (current->tty != tty) ||
			    (tty->session != current->session))
				return -ENOTTY;
			pid_t pgrp = get_fs_long((pid_t *) arg);
			if (pgrp < 0)
				return -EINVAL;
			if (session_of_pgrp(pgrp) != current->session)
				return -EPERM;
			tty->pgrp = pgrp;
			return 0;
		case TIOCGSID:
			if (current->tty != tty)
				return -ENOTTY;
			err = verify_area(VERIFY_WRITE, (void *) arg, sizeof (pid_t));
			if (err)
				return err;
			put_fs_long(tty->session, (pid_t *) arg);
			return 0;
	}
	return -EINVAL;
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
