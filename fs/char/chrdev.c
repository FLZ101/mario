#include <fs/chrdev.h>

#include <misc.h>
#include <errno.h>

#include <lib/stddef.h>

struct {
	struct chrdev_operations *chrdev_ops;
} chrdevs[MAX_CHRDEV];

extern void tty_init(void);

void __tinit chrdev_init(void)
{
	int i;

	for (i = 0; i < MAX_CHRDEV; i++)
		chrdevs[i].chrdev_ops = NULL;

	tty_init();
}

int register_chrdev(unsigned int major, struct chrdev_operations *chrdev_ops)
{
	if (major == NODEV || major >= MAX_CHRDEV)
		return -EINVAL;

	if (chrdevs[major].chrdev_ops)
		return -EBUSY;

	chrdevs[major].chrdev_ops = chrdev_ops;
	return 0;
}

/* check whether @major is registered */
int check_chrdev(unsigned int major)
{
	if (major == NODEV || major >= MAX_CHRDEV)
		return 0;
	if (!chrdevs[major].chrdev_ops)
		return 0;
	return 1;
}

int chrdev_open(dev_t dev)
{
	unsigned int major;
	struct chrdev_operations *chrdev_ops;

	major = MAJOR(dev);
	if (!check_chrdev(major))
		return -EINVAL;

	chrdev_ops = chrdevs[major].chrdev_ops;
	if (chrdev_ops->chrdev_open)
		return chrdev_ops->chrdev_open(dev);
	return 0;
}

int chrdev_read(dev_t dev, char *c)
{
	unsigned int major;
	struct chrdev_operations *chrdev_ops;

	major = MAJOR(dev);
	if (!check_chrdev(major))
		return -EINVAL;

	chrdev_ops = chrdevs[major].chrdev_ops;
	if (chrdev_ops->chrdev_read)
		return chrdev_ops->chrdev_read(dev, c);
	return 0;
}

int chrdev_write(dev_t dev, char *c)
{
	unsigned int major;
	struct chrdev_operations *chrdev_ops;

	major = MAJOR(dev);
	if (!check_chrdev(major))
		return -EINVAL;

	chrdev_ops = chrdevs[major].chrdev_ops;
	if (chrdev_ops->chrdev_write)
		return chrdev_ops->chrdev_write(dev, c);
	return 0;
}
