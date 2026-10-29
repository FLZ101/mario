#include <fs/fs.h>
#include <fs/tty/tty.h>
#include <fs/tty/console.h>
#include <fs/pty.h>

#include <misc.h>

#include <mm/page_alloc.h>

struct termios default_termios = {
	.c_cflag = 0,
	.c_iflag = ICRNL,
	/* Match the usual default: background writes are allowed unless TOSTOP is enabled. */
	.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL,
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

int tty_init_struct(struct tty_struct *tty, dev_t dev,
			    struct tty_driver *driver, unsigned int rows,
			    unsigned int cols)
{
	int err;

	memset(tty, 0, sizeof(*tty));
	tty->dev = dev;
	tty->driver = driver;
	if ((err = ring_buffer_init(&tty->read_buf)))
		return err;
	init_wait_queue(&tty->wait_read);
	init_wait_queue(&tty->wait_input);
	init_wait_queue(&tty->wait_write);
	INIT_LOCK(&tty->lock);
	tty->termios = default_termios;
	tty->winsize.ws_col = cols;
	tty->winsize.ws_row = rows;
	return 0;
}

int tty_output_char(struct tty_struct *tty, unsigned char c, char output[2])
{
	unsigned long flags;
	tcflag_t c_oflag;
	int count = 0;

	spin_lock_irqsave(&tty->lock, flags);
	c_oflag = tty->termios.c_oflag;
	spin_unlock_irqrestore(&tty->lock, flags);

	if (c_oflag & OPOST) {
		if (c == '\n' && (c_oflag & ONLCR))
			output[count++] = '\r';
	}
	output[count++] = c;
	return count;
}

int tty_put_c(struct tty_struct *tty, unsigned char c)
{
	char output[2];
	int count;
	int written;

	count = tty_output_char(tty, c, output);
	written = tty->driver->write(tty, output, count);
	if (written < 0)
		return written;
	return written == count ? 0 : -EAGAIN;
}

void tty_put_s(struct tty_struct *tty, char *s)
{
	char c;
	while ((c = *s++))
		tty_put_c(tty, c);
}


int tty_receive_c(struct tty_struct *tty, unsigned char c)
{
	unsigned long flags;
	cc_t c_cc[NCCS];
	tcflag_t c_iflag, c_lflag;

	spin_lock_irqsave(&tty->lock, flags);
	if (!tty->count) {
		spin_unlock_irqrestore(&tty->lock, flags);
		return -EIO;
	}

	struct ring_buffer *rb = &tty->read_buf;
	int popped = -1;

	int wake = 0;

	c_iflag = tty->termios.c_iflag;
	c_lflag = tty->termios.c_lflag;
	memcpy(c_cc, tty->termios.c_cc, sizeof(c_cc));
	spin_unlock_irqrestore(&tty->lock, flags);

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

	if (c == '\r') {
		if (c_iflag & IGNCR)
			goto tail_2;
		if (c_iflag & ICRNL)
			c = '\n';
	} else if (c == '\n' && (c_iflag & INLCR)) {
		c = '\r';
	}

	if (c_lflag & ICANON) {
		if (c == c_cc[VERASE]) {
			spin_lock_irqsave(&tty->lock, flags);
			popped = ring_buffer_pop(rb);
			spin_unlock_irqrestore(&tty->lock, flags);
			goto tail_1;
		}

		if (c == c_cc[VEOF] || c == '\n' || c == '\r' || c == c_cc[VEOL]) {
			wake = 1;
		}
	} else {
		wake = 1;
	}

	spin_lock_irqsave(&tty->lock, flags);
	if (ring_buffer_full(rb)) {
		spin_unlock_irqrestore(&tty->lock, flags);
		return -EAGAIN;
	}
	ring_buffer_write(rb, &c, 1, 0);
	spin_unlock_irqrestore(&tty->lock, flags);
tail_1:
	if (c_lflag & ECHO) {
		if (c == c_cc[VERASE] && c_lflag & ECHOE) {
			if (popped != -1)
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
	return 0;
}

void tty_receive_s(struct tty_struct *tty, char *s)
{
	if (!tty->count)
		return;

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

int get_tty(dev_t dev, struct tty_struct **tty)
{
	dev_t major = MAJOR(dev);
	dev_t minor = MINOR(dev);

	for (struct tty_driver *p = tty_drivers; p; p = p->next) {
		if (p->major == major &&
		    p->minor <= minor && minor < p->minor + p->n) {
			if (p->lookup)
				*tty = p->lookup(minor);
			else
				*tty = &p->tty_table[minor - p->minor];
			return 0;
		}
	}
	return -EINVAL;
}

int tty_bind_file(struct tty_struct *tty, struct file *f)
{
	unsigned long flags;

	if (tty->driver->bind)
		return tty->driver->bind(tty, f);

	spin_lock_irqsave(&tty->lock, flags);
	if (!tty->initialized) {
		spin_unlock_irqrestore(&tty->lock, flags);
		return -EINVAL;
	}
	++tty->count;
	spin_unlock_irqrestore(&tty->lock, flags);

	f->f_op = tty->driver->fops;
	f->private_data = tty;
	return 0;
}

int tty_set_controlling(struct tty_struct *tty)
{
	unsigned long flags;

	if (!current->leader)
		return -EPERM;
	if (current->tty && current->tty != tty)
		return -EPERM;

	spin_lock_irqsave(&tty->lock, flags);
	if (tty->session && tty->session != current->session) {
		spin_unlock_irqrestore(&tty->lock, flags);
		return -EPERM;
	}
	current->tty = tty;
	tty->session = current->session;
	tty->pgrp = current->pgrp;
	spin_unlock_irqrestore(&tty->lock, flags);
	return 0;
}

void tty_try_set_controlling(struct tty_struct *tty, int noctty)
{
	if (!noctty && current->leader && !current->tty && !tty->session)
		tty_set_controlling(tty);
}

int tty_open(struct inode *i, struct file *f)
{
	int noctty = f->f_flags & O_NOCTTY;

	dev_t dev = i->i_rdev;
	dev_t minor = MINOR(dev);

	if (minor == TTY_MINOR) {
		if (!current->tty)
			return -ENXIO;
		dev = current->tty->dev;
	} else if (minor == TTY_MINOR_0) {
		dev = get_fg_console_dev();
	} else if (minor == SYSTEM_CONSOLE_MINOR) {
		dev = MKDEV(TTY_MAJOR, TTY_MINOR_1);
		noctty = 1;
	}
	struct tty_struct *tty = NULL;
	int err = get_tty(dev, &tty);
	if (err)
		return err;

	err = tty_bind_file(tty, f);
	if (err)
		return err;

	tty_try_set_controlling(tty, noctty);
	return 0;
}

int get_n_canon_read(struct ring_buffer *rb, cc_t *c_cc) {
	size_t mask = rb->size - 1;
	size_t p = rb->tail;
	int n = 0;

	if (ring_buffer_full(rb))
		return ring_buffer_avail(rb);

	while (p != rb->head) {
		uint8_t ch = (uint8_t) rb->data[p];

		p = (p + 1) & mask;
		++n;

		if (ch == '\n' || ch == c_cc[VEOF] || ch == c_cc[VEOL])
			return n;
	}
	return 0;
}

// return number of bytes to read
int get_n_read(struct tty_struct *tty)
{
	struct ring_buffer *rb = &tty->read_buf;
	tcflag_t c_lflag = tty->termios.c_lflag;
	cc_t *c_cc = tty->termios.c_cc;

	/* Caller must hold tty->lock. */
	if (c_lflag & ICANON)
		return get_n_canon_read(rb, c_cc);
	else
		return ring_buffer_avail(rb);
}

int tty_read(struct inode *i, struct file *f, char *buf, int count)
{
	struct tty_struct *tty = (struct tty_struct *)f->private_data;
	unsigned long flags;
	cc_t eof;
	if (!tty)
		return -EIO;

	if (current->tty == tty) {
		if (current->pgrp != tty->pgrp) {
			kill_pg(current->pgrp, SIGTTIN, 1);
			return -ERESTARTSYS;
		}
	}

	struct ring_buffer *rb = &tty->read_buf;

try:
	if (current->signal & ~current->blocked)
		return -ERESTARTSYS;

	spin_lock_irqsave(&tty->lock, flags);
	int n = get_n_read(tty);
	if (!n) {
		if (tty->hung_up) {
			spin_unlock_irqrestore(&tty->lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&tty->lock, flags);
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		sleep_on(&tty->wait_read, TASK_INTERRUPTIBLE, NULL);
			goto try;
	}

	n = MIN(n, count);

	int m = ring_buffer_read(rb, buf, n, 1);
	eof = tty->termios.c_cc[VEOF];
	spin_unlock_irqrestore(&tty->lock, flags);
	wake_up_interruptible(&tty->wait_input);
	assert(n == m);

	// Except in the case of EOF, the line delimiter is included in the buffer returned by read(2).
	if (buf[n-1] == eof)
		--n;
	return n;
}

static int tty_check(struct tty_struct *tty)
{
	unsigned long flags;
	int stop = 0;

	if (!tty)
		return -EIO;

	/* Stop a background writer only when the tty has TOSTOP enabled. */
	spin_lock_irqsave(&tty->lock, flags);
	if (tty->hung_up) {
		spin_unlock_irqrestore(&tty->lock, flags);
		return -EIO;
	}
	if (current->tty == tty &&
	    (tty->termios.c_lflag & TOSTOP) &&
	    current->pgrp != tty->pgrp)
		stop = 1;
	spin_unlock_irqrestore(&tty->lock, flags);
	if (stop) {
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

	for (int i = 0; i < count; ++i) {
		char output[2];
		unsigned char c = get_fs_byte(buf + i);
		int output_count = tty_output_char(tty, c, output);
		int output_done = 0;

		while (output_done < output_count) {
			int n = tty->driver->write(tty, output + output_done,
						   output_count - output_done);

			if (n < 0)
				return i ? i : n;
			if (n > output_count - output_done)
				hang("tty output driver wrote too much");
			output_done += n;
			if (output_done == output_count)
				break;
			if (f->f_flags & O_NONBLOCK)
				return i ? i : -EAGAIN;
			if (!output_done && (current->signal & ~current->blocked))
				return i ? i : -ERESTARTSYS;
			sleep_on(&tty->wait_write, TASK_INTERRUPTIBLE, NULL);
		}
	}
	return count;
}

static int tty_lseek(struct inode *i, struct file *f, off_t offset, int orig)
{
	return -ESPIPE;
}

static int set_termios(struct tty_struct * tty, unsigned long arg, int opt)
{
	struct termios tmp_termios;
	unsigned long flags;
	int err = verify_area(VERIFY_READ, (void *) arg, sizeof(struct termios));
	if (err)
		return err;
	memcpy_fromfs(&tmp_termios, (struct termios *) arg, sizeof (struct termios));

	spin_lock_irqsave(&tty->lock, flags);
	tty->termios = tmp_termios;
	spin_unlock_irqrestore(&tty->lock, flags);
	return 0;
}

int session_of_pgrp(int pgrp)
{
	struct task_struct *p;
	unsigned long flags;
	int fallback;

	fallback = -1;
	spin_lock_irqsave(&tasklist_lock, flags);
	for_each_task(p) {
 		if (p->session <= 0)
 			continue;
		if (p->pgrp == pgrp)
			goto found;
		if (p->pid == pgrp)
			fallback = p->session;
	}
	spin_unlock_irqrestore(&tasklist_lock, flags);
	return fallback;

found:
	fallback = p->session;
	spin_unlock_irqrestore(&tasklist_lock, flags);
	return fallback;
}

int tty_ioctl(struct inode *i, struct file *f, unsigned int cmd, unsigned long arg)
{
	int err = 0;
	int opt = 0;
	struct tty_struct *tty = (struct tty_struct *) f->private_data;

	if (!tty)
		return -EIO;

	if (current->tty == tty) {
		if (cmd != TIOCSPGRP && current->pgrp != tty->pgrp) {
			kill_pg(current->pgrp, SIGTTOU, 1);
			return -ERESTARTSYS;
		}
	}

	switch (cmd) {
		case TCGETS:
		{
			struct termios tmp;
			unsigned long flags;

			err = verify_area(VERIFY_WRITE, (void *) arg, sizeof (struct termios));
			if (err)
				return err;
			spin_lock_irqsave(&tty->lock, flags);
			tmp = tty->termios;
			spin_unlock_irqrestore(&tty->lock, flags);
			memcpy_tofs((struct termios *) arg, &tmp, sizeof (tmp));
			return 0;
		}
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
		case TIOCSWINSZ: {
			err = verify_area(VERIFY_READ, (void *) arg, sizeof (struct winsize));
			if (err)
				return err;

			struct winsize ws;
			memcpy_fromfs(&ws, (struct winsize *) arg, sizeof (struct winsize));
			if (memcmp(&ws, &tty->winsize, sizeof(struct winsize))) {
				tty->winsize = ws;
				if (tty->pgrp > 0)
					kill_pg(tty->pgrp, SIGWINCH, 1);
			}
			return 0;
		}
		case TIOCSTI:
			if (current->tty != tty)
				return -EPERM;
			err = verify_area(VERIFY_READ, (void *) arg, 1);
			if (err)
				return err;
			unsigned char ch = get_fs_byte((char *) arg);
			tty_receive_c(tty, ch);
			return 0;
		case TIOCSCTTY:
			return tty_set_controlling(tty);
		case TIOCNOTTY:
			if (current->tty != tty)
				return -ENOTTY;
			current->tty = NULL;
			if (tty->session == current->session) {
				tty->session = 0;
				tty->pgrp = 0;
			}
			return 0;
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
		case TCFLSH:
			switch (arg) {
			case TCOFLUSH:
				return 0;
			case TCIFLUSH:
			case TCIOFLUSH:
				{
					unsigned long flags;

					spin_lock_irqsave(&tty->lock, flags);
					ring_buffer_clear(&tty->read_buf);
					spin_unlock_irqrestore(&tty->lock, flags);
				}
				return 0;
			}
	}
	return -EINVAL;
}

static void tty_release(struct inode *i, struct file *f)
{
	struct tty_struct *tty = (struct tty_struct *)f->private_data;
	unsigned long flags;
	if (!tty)
		return;

	spin_lock_irqsave(&tty->lock, flags);
	assert(tty->count > 0);
	if (!--tty->count)
		ring_buffer_clear(&tty->read_buf);
	spin_unlock_irqrestore(&tty->lock, flags);
}

int tty_poll(struct file *f, struct poll_context *ctx)
{
	struct tty_struct *tty = (struct tty_struct *)f->private_data;
	if (!tty) {
		poll_wait(f, NULL, ctx);
		return POLLNVAL;
	}

	poll_wait(f, &tty->wait_read, ctx);

	if (tty->count < 1)
		return POLLHUP;

	short mask = POLLOUT;
	{
		unsigned long flags;
		int readable;

		spin_lock_irqsave(&tty->lock, flags);
	if (tty->hung_up) {
		spin_unlock_irqrestore(&tty->lock, flags);
		return POLLIN | POLLHUP;
	}
		readable = get_n_read(tty) > 0;
		spin_unlock_irqrestore(&tty->lock, flags);
		if (readable)
			mask |= POLLIN;
	}
	return mask;
}

struct file_operations tty_fops = {
	.lseek = tty_lseek,
	.open = tty_open,
	.read = tty_read,
	.write = tty_write,
	.release = tty_release,
	.ioctl = tty_ioctl,
	.poll = tty_poll
};

extern void ps2_init(void);
extern void console_init(void);
extern void serial_init(void);

void tty_init(void)
{
	ps2_init();
	console_init();
	serial_init();
	register_chrdev(TTY_MAJOR, &tty_fops);
	pty_init();
}
