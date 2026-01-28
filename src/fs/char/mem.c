#include <fs/fs.h>

int zero_read(struct inode *i, struct file *f, char *buf, int count)
{
	int left;

	for (left = count; left > 0; left--) {
		put_fs_byte(0,buf);
		buf++;
	}
	return count;
}

int zero_write(struct inode *i, struct file *f, char *buf, int count)
{
	return count;
}

struct file_operations zero_fops = {
	.read = zero_read,
	.write = zero_write
};

int null_read(struct inode *i, struct file *f, char *buf, int count)
{
	return 0;
}

int null_write(struct inode *i, struct file *f, char *buf, int count)
{
	return count;
}

struct file_operations null_fops = {
	.read = null_read,
	.write = null_write
};

int mem_open(struct inode *i, struct file *f)
{
	switch (MINOR(i->i_rdev)) {
	case MEM_MINOR_ZERO:
		f->f_op = &zero_fops;
		return 0;
	case MEM_MINOR_NULL:
		f->f_op = &null_fops;
		return 0;
	default:
		return -ENODEV;
	}
}

struct file_operations mem_fops = {
    .open = mem_open,
};

void __tinit mem_init(void)
{
	register_chrdev(MEM_MAJOR, &mem_fops);
}
