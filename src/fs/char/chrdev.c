#include <fs/fs.h>

#include <misc.h>
#include <errno.h>

#include <lib/stddef.h>

struct {
	struct file_operations *chrdev_fops;
} chrdevs[MAX_CHRDEV];

extern void mem_init(void);
extern void tty_init(void);

void __tinit chrdev_init(void)
{
	int i;

	for (i = 0; i < MAX_CHRDEV; i++)
		chrdevs[i].chrdev_fops = NULL;

	mem_init();
	tty_init();
}

int register_chrdev(unsigned int major, struct file_operations *chrdev_fops)
{
	if (major == NODEV || major >= MAX_CHRDEV)
		return -EINVAL;

	if (chrdevs[major].chrdev_fops)
		return -EBUSY;

	chrdevs[major].chrdev_fops = chrdev_fops;
	return 0;
}

/* check whether @major is registered */
int check_chrdev(unsigned int major)
{
	if (major == NODEV || major >= MAX_CHRDEV || !chrdevs[major].chrdev_fops)
		return -ENODEV;
	return 0;
}

int chr_file_open(struct inode *i, struct file *f)
{
	int err;
	unsigned int major;

	major = MAJOR(i->i_rdev);
	if ((err = check_chrdev(major)))
		return err;

	f->f_op = chrdevs[major].chrdev_fops;
	if (f->f_op->open)
		return f->f_op->open(i, f);
	return 0;
}

struct file_operations chrdev_fops = {
	.open = chr_file_open,
};
