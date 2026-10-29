#include <fs/fs.h>
#include <fs/chrdev.h>
#include <fs/ioctl.h>
#include <fs/pty.h>
#include <fs/tty/tty.h>

#include <misc.h>
#include <mm/kmalloc.h>

#define FMODE_PTY_MASTER 0x100

enum pty_state {
	PTY_FREE,
	PTY_ALLOCATED,
};

struct pty_pair {
	struct tty_struct tty;
	struct ring_buffer output;
	wait_queue_t master_read_wait;
	spinlock_t lock;
	enum pty_state state;
	int number;
	int master_open;
	int slave_open;
	int slave_locked;
};

static struct pty_pair ptys[NR_PTYS];
static spinlock_t pty_lock = SPINLOCK_UNLOCKED;

static struct pty_pair *tty_to_pty(struct tty_struct *tty)
{
	return (struct pty_pair *)tty;
}

static struct tty_struct *pty_lookup(dev_t minor)
{
	return &ptys[minor].tty;
}

static int pty_write(struct tty_struct *tty, const char *buf, int count)
{
	struct pty_pair *pty = tty_to_pty(tty);
	unsigned long flags;
	int n;

	spin_lock_irqsave(&pty->lock, flags);
	if (!pty->master_open) {
		spin_unlock_irqrestore(&pty->lock, flags);
		return -EIO;
	}
	n = ring_buffer_write(&pty->output, buf, count, 0);
	if (n)
		wake_up_interruptible(&pty->master_read_wait);
	spin_unlock_irqrestore(&pty->lock, flags);
	return n;
}

static int pty_bind_slave(struct tty_struct *tty, struct file *f)
{
	struct pty_pair *pty = tty_to_pty(tty);
	unsigned long flags;

	spin_lock_irqsave(&pty->lock, flags);
	if (pty->state != PTY_ALLOCATED || !pty->master_open ||
	    pty->slave_locked) {
		spin_unlock_irqrestore(&pty->lock, flags);
		return -EIO;
	}
	pty->slave_open++;
	spin_lock(&tty->lock);
	tty->count++;
	spin_unlock(&tty->lock);
	spin_unlock_irqrestore(&pty->lock, flags);

	f->f_op = tty->driver->fops;
	f->private_data = tty;
	return 0;
}

static int pty_master_open(struct file *f)
{
	struct pty_pair *pty = NULL;
	unsigned long flags;

	spin_lock_irqsave(&pty_lock, flags);
	for (int i = 0; i < NR_PTYS; i++) {
		if (ptys[i].state == PTY_FREE) {
			pty = &ptys[i];
			pty->state = PTY_ALLOCATED;
			break;
		}
	}
	spin_unlock_irqrestore(&pty_lock, flags);
	if (!pty)
		return -ENOSPC;

	spin_lock_irqsave(&pty->lock, flags);
	pty->master_open = 1;
	pty->slave_open = 0;
	pty->slave_locked = 1;
	ring_buffer_clear(&pty->output);
	spin_lock(&pty->tty.lock);
	ring_buffer_clear(&pty->tty.read_buf);
	pty->tty.termios = default_termios;
	pty->tty.session = 0;
	pty->tty.pgrp = 0;
	pty->tty.count = 1;
	spin_unlock(&pty->tty.lock);
	spin_unlock_irqrestore(&pty->lock, flags);

	f->private_data = pty;
	f->f_mode |= FMODE_PTY_MASTER;
	return 0;
}

static int pty_open(struct inode *i, struct file *f)
{
	unsigned int minor = MINOR(i->i_rdev);
	struct tty_struct *tty;

	if (minor == PTMX_MINOR)
		return pty_master_open(f);
	if (minor >= NR_PTYS)
		return -ENXIO;
	if (get_tty(i->i_rdev, &tty))
		return -ENXIO;
	return pty_bind_slave(tty, f);
}

static int pty_master_read(struct inode *i, struct file *f, char *buf, int count)
{
	struct pty_pair *pty = f->private_data;
	unsigned long flags;
	int n;

	for (;;) {
		spin_lock_irqsave(&pty->lock, flags);
		if (!ring_buffer_empty(&pty->output)) {
			n = ring_buffer_read(&pty->output, buf, count, 1);
			wake_up_interruptible(&pty->tty.wait_write);
			spin_unlock_irqrestore(&pty->lock, flags);
			return n;
		}
		spin_unlock_irqrestore(&pty->lock, flags);
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (current->signal & ~current->blocked)
			return -ERESTARTSYS;
		sleep_on(&pty->master_read_wait, TASK_INTERRUPTIBLE, NULL);
	}
}

static int pty_master_write(struct inode *i, struct file *f, char *buf, int count)
{
	struct pty_pair *pty = f->private_data;

	for (int i = 0; i < count; i++)
		tty_receive_c(&pty->tty, get_fs_byte(buf + i));
	return count;
}

static int pty_master_ioctl(struct inode *i, struct file *f, unsigned int cmd,
			    unsigned long arg)
{
	struct pty_pair *pty = f->private_data;
	unsigned long flags;
	int err;

	switch (cmd) {
	case TIOCGPTN:
		err = verify_area(VERIFY_WRITE, (void *)arg, sizeof(int));
		if (err)
			return err;
		put_fs_long(pty->number, (int *)arg);
		return 0;
	case TIOCSPTLCK: {
		int locked;

		err = verify_area(VERIFY_READ, (void *)arg, sizeof(locked));
		if (err)
			return err;
		locked = get_fs_long((int *)arg);
		spin_lock_irqsave(&pty->lock, flags);
		pty->slave_locked = !!locked;
		spin_unlock_irqrestore(&pty->lock, flags);
		return 0;
	}
	}
	return -ENOTTY;
}

static int pty_master_poll(struct file *f, struct poll_context *ctx)
{
	struct pty_pair *pty = f->private_data;
	unsigned long flags;
	short mask = POLLOUT;

	poll_wait(f, &pty->master_read_wait, ctx);
	spin_lock_irqsave(&pty->lock, flags);
	if (!ring_buffer_empty(&pty->output))
		mask |= POLLIN;
	spin_unlock_irqrestore(&pty->lock, flags);
	return mask;
}

static int pty_read(struct inode *i, struct file *f, char *buf, int count)
{
	if (f->f_mode & FMODE_PTY_MASTER)
		return pty_master_read(i, f, buf, count);
	return tty_read(i, f, buf, count);
}

static int pty_write_file(struct inode *i, struct file *f, char *buf, int count)
{
	if (f->f_mode & FMODE_PTY_MASTER)
		return pty_master_write(i, f, buf, count);
	return tty_write(i, f, buf, count);
}

static int pty_ioctl(struct inode *i, struct file *f, unsigned int cmd,
			     unsigned long arg)
{
	if (f->f_mode & FMODE_PTY_MASTER)
		return pty_master_ioctl(i, f, cmd, arg);
	return tty_ioctl(i, f, cmd, arg);
}

static int pty_poll(struct file *f, struct poll_context *ctx)
{
	if (f->f_mode & FMODE_PTY_MASTER)
		return pty_master_poll(f, ctx);
	return tty_poll(f, ctx);
}

static void pty_release(struct inode *i, struct file *f)
{
	struct pty_pair *pty;
	unsigned long flags;
	int reusable = 0;

	if (f->f_mode & FMODE_PTY_MASTER) {
		pty = f->private_data;
		spin_lock_irqsave(&pty->lock, flags);
		pty->master_open = 0;
		spin_lock(&pty->tty.lock);
		assert(pty->tty.count > 0);
		pty->tty.count--;
		spin_unlock(&pty->tty.lock);
		wake_up_interruptible(&pty->tty.wait_read);
		wake_up_interruptible(&pty->tty.wait_write);
		if (!pty->slave_open)
			reusable = 1;
		spin_unlock_irqrestore(&pty->lock, flags);
	} else {
		pty = tty_to_pty(f->private_data);
		spin_lock_irqsave(&pty->lock, flags);
		assert(pty->slave_open > 0);
		pty->slave_open--;
		spin_lock(&pty->tty.lock);
		assert(pty->tty.count > 0);
		pty->tty.count--;
		spin_unlock(&pty->tty.lock);
		if (!pty->master_open && !pty->slave_open)
			reusable = 1;
		spin_unlock_irqrestore(&pty->lock, flags);
	}

	if (reusable) {
		spin_lock_irqsave(&pty_lock, flags);
		pty->state = PTY_FREE;
		spin_unlock_irqrestore(&pty_lock, flags);
	}
}

static int pty_lseek(struct inode *i, struct file *f, off_t offset, int origin)
{
	return -ESPIPE;
}

static struct file_operations pty_fops = {
	.open = pty_open,
	.release = pty_release,
	.lseek = pty_lseek,
	.read = pty_read,
	.write = pty_write_file,
	.ioctl = pty_ioctl,
	.poll = pty_poll,
};

static struct tty_driver pty_driver = {
	.major = PTY_MAJOR,
	.minor = 0,
	.n = NR_PTYS,
	.tty_table = &ptys[0].tty,
	.fops = &pty_fops,
	.lookup = pty_lookup,
	.bind = pty_bind_slave,
	.write = pty_write,
};

void pty_init(void)
{
	for (int i = 0; i < NR_PTYS; i++) {
		struct pty_pair *pty = &ptys[i];

		memset(pty, 0, sizeof(*pty));
		pty->number = i;
		INIT_LOCK(&pty->lock);
		init_wait_queue(&pty->master_read_wait);
		if (ring_buffer_init(&pty->output) ||
		    tty_init_struct(&pty->tty, MKDEV(PTY_MAJOR, i), &pty_driver,
				    24, 80))
			hang("failed to initialize pty");
		pty->tty.initialized = 1;
	}
	register_tty_driver(&pty_driver);
	if (register_chrdev(PTY_MAJOR, &pty_fops))
		hang("failed to register pty device");
}
