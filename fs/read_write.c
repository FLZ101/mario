#include <fs/fs.h>

int sys_lseek(unsigned int fd, off_t offset, unsigned int origin)
{
	int tmp = -1;
	struct file *f;

	if (fd >= NR_OPEN || !(f = current->files->fd[fd]) || !(f->f_inode))
		return -EBADF;

	if (origin > 2)
		return -EINVAL;
	if (f->f_op && f->f_op->lseek)
		return f->f_op->lseek(f->f_inode, f, offset, origin);

	/* the default lseek */
	switch (origin) {
	case 0:
		tmp = offset;
		break;
	case 1:
		tmp = f->f_pos + offset;
		break;
	case 2:
		tmp = f->f_inode->i_size + offset;
		break;
	}
	/*
	 * file position can not be bigger than file size !!!
	 */
	if (tmp < 0 || tmp >= f->f_inode->i_size)
		return -EINVAL;

	return f->f_pos = tmp;
}

int sys_read(unsigned int fd, char *buf, unsigned int count)
{
	int error;
	struct file *f;
	struct inode *i;

	if (fd >= NR_OPEN || !(f = current->files->fd[fd]) || !(i = f->f_inode))
		return -EBADF;
	if (!(f->f_mode & 1))
		return -EBADF;
	if (!f->f_op || !f->f_op->read)
		return -EINVAL;

	if (!count)
		return 0;
	error = verify_area(VERIFY_WRITE, buf, count);
	if (error)
		return error;

	return f->f_op->read(i, f, buf, count);
}

int sys_write(unsigned int fd, char *buf, unsigned int count)
{
	int error;
	struct file *f;
	struct inode *i;

	if (fd >= NR_OPEN || !(f = current->files->fd[fd]) || !(i = f->f_inode))
		return -EBADF;
	if (!(f->f_mode & 2))
		return -EBADF;
	if (!f->f_op || !f->f_op->write)
		return -EINVAL;

	if (!count)
		return 0;
	error = verify_area(VERIFY_READ, buf, count);
	if (error)
		return error;
	return f->f_op->write(i, f, buf, count);
}