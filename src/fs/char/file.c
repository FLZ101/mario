#include <fs/fs.h>
#include <fs/chrdev.h>

int chr_file_open(struct inode *i, struct file *f)
{
	dev_t dev = i->i_rdev;

	return chrdev_open(dev);
}

int chr_file_read(struct inode *inode, struct file *file, char *buf, int n)
{
	int i;
	dev_t dev = inode->i_rdev;

	for (i = 0; i < n; buf++, i++)
		if (chrdev_read(dev, buf))
			break;
	return i;
}

int chr_file_write(struct inode *inode, struct file *file, char *buf, int n)
{
	int i;
	dev_t dev = inode->i_rdev;

	for (i = 0; i < n; buf++, i++)
		if (chrdev_write(dev, buf))
			break;
	return i;
}

struct file_operations chrdev_fops = {
	chr_file_open,
	NULL,
	NULL,
	chr_file_read,
	chr_file_write,
	NULL,
	NULL
};