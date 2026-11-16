#include <fs/fs.h>
#include <fs/uio.h>

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
	case SEEK_SET:
		tmp = offset;
		break;
	case SEEK_CUR:
		tmp = f->f_pos + offset;
		break;
	case SEEK_END:
		tmp = f->f_inode->i_size + offset;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * File hole is not supported yet
	 */
	if (tmp < 0 || tmp > f->f_inode->i_size)
		return -EINVAL;

	return f->f_pos = tmp;
}

int sys__llseek(unsigned int fd, unsigned long offset_high,
	unsigned long offset_low, loff_t *result, unsigned int whence)
{
	int err = verify_area(VERIFY_WRITE, result, sizeof(loff_t));
	if (err)
		return err;

	loff_t offset = ((loff_t) offset_high << 32) | offset_low;
	if (offset != (loff_t) (off_t) offset)
		return -EINVAL;

	err = sys_lseek(fd, offset, whence);
	if (err >= 0) {
		loff_t tmp = err;
		memcpy_tofs(result, &tmp, sizeof(loff_t));
		return 0; // !!!
	}
	return err;
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
	// only if count > 0 then return 0 means EOF

	error = verify_area(VERIFY_WRITE, buf, count);
	if (error)
		return error;
	return f->f_op->read(i, f, buf, count);
}

/* i386 passes the 64-bit pread/pwrite offset as low word, then high word. */
static int positional_offset(unsigned long offset_low, unsigned long offset_high,
			     off64_t *offset)
{
	*offset = (off64_t)(int32_t)offset_high * 0x100000000LL + offset_low;
	return *offset < 0 ? -EINVAL : 0;
}

int sys_pread64(unsigned int fd, char *buf, unsigned int count,
		unsigned long offset_low, unsigned long offset_high)
{
	int error;
	struct file *f;
	struct inode *i;
	off64_t offset;

	if (fd >= NR_OPEN || !(f = current->files->fd[fd]) || !(i = f->f_inode))
		return -EBADF;
	if (!(f->f_mode & 1))
		return -EBADF;
	if (!f->f_op || !f->f_op->read_at)
		return -ESPIPE;
	if (count > 0x7fffffffU)
		return -EINVAL;
	if ((error = positional_offset(offset_low, offset_high, &offset)))
		return error;
	if (!count)
		return 0;
	if ((error = verify_area(VERIFY_WRITE, buf, count)))
		return error;

	/* The callback updates this local position after a short successful read. */
	return f->f_op->read_at(i, f, buf, count, &offset);
}

int sys_readv(unsigned int fd, struct iovec *vec, int vlen)
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

	if (!vlen)
		return 0;

	int total = 0;
	for (unsigned int idx = 0; idx < vlen; ++idx) {
		void *buf = vec[idx].iov_base;
		size_t count = vec[idx].iov_len;

		if (!buf || !count)
			continue;

		error = verify_area(VERIFY_WRITE, buf, count);
		if (error)
			return error;

		error = f->f_op->read(i, f, buf, count);
		if (error < 0)
			return total ? total : error;

		total += error;
		if ((size_t)error < count)
			break;
	}
	return total;
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

int sys_pwrite64(unsigned int fd, char *buf, unsigned int count,
		 unsigned long offset_low, unsigned long offset_high)
{
	int error;
	struct file *f;
	struct inode *i;
	off64_t offset;

	if (fd >= NR_OPEN || !(f = current->files->fd[fd]) || !(i = f->f_inode))
		return -EBADF;
	if (!(f->f_mode & 2))
		return -EBADF;
	if (!f->f_op || !f->f_op->write_at)
		return -ESPIPE;
	if (count > 0x7fffffffU)
		return -EINVAL;
	if ((error = positional_offset(offset_low, offset_high, &offset)))
		return error;
	if (!count)
		return 0;
	if ((error = verify_area(VERIFY_READ, buf, count)))
		return error;

	/* O_APPEND belongs to write(); pwrite64() always uses this exact offset. */
	return f->f_op->write_at(i, f, buf, count, &offset);
}

int sys_writev(unsigned int fd, struct iovec *vec, unsigned int vlen)
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

	if (!vlen)
		return 0;

	int total = 0;
	for (unsigned int idx = 0; idx < vlen; ++idx) {
		void *buf = vec[idx].iov_base;
		size_t count = vec[idx].iov_len;

		if (!buf || !count)
			continue;

		error = verify_area(VERIFY_READ, buf, count);
		if (error)
			return error;

		error = f->f_op->write(i, f, buf, count);
		if (error < 0)
			return total ? total : error;

		total += error;
		if ((size_t)error < count)
			break;
	}
	return total;
}
