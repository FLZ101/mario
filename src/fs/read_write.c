#include <fs/fs.h>
#include <fs/uio.h>
#include <net/inet.h>

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
	struct file *file = get_fd_file(fd, NULL);

	if (!file)
		return -EBADF;
	if (!file->f_inode || !(file->f_mode & 1))
		error = -EBADF;
	else if (!file->f_op || !file->f_op->read)
		error = -EINVAL;
	else if (!count)
		error = 0;
	else if (!(error = verify_area(VERIFY_WRITE, buf, count)))
		error = file->f_op->read(file->f_inode, file, buf, count);
	release_file(file, 1);
	return error;
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

static int file_iov(struct file *file, struct iovec *vec, unsigned int vlen,
		    int writing)
{
	int (*transfer)(struct inode *, struct file *, char *, int);
	int total = 0;

	if (!file->f_inode || !(file->f_mode & (writing ? 2 : 1)))
		return -EBADF;
	if (!file->f_op)
		return -EINVAL;
	if (file->f_op == &inet_fops)
		return -EOPNOTSUPP;
	transfer = writing ? file->f_op->write : file->f_op->read;
	if (!transfer)
		return -EINVAL;
	for (unsigned int index = 0; index < vlen; ++index) {
		void *buf = vec[index].iov_base;
		size_t count = vec[index].iov_len;
		int error;

		if (!buf || !count)
			continue;

		error = verify_area(writing ? VERIFY_READ : VERIFY_WRITE, buf, count);
		if (error)
			return error;

		error = transfer(file->f_inode, file, buf, count);
		if (error < 0)
			return total ? total : error;

		total += error;
		if ((size_t)error < count)
			break;
	}
	return total;
}

static int do_iov(unsigned int fd, struct iovec *vec, unsigned int vlen, int writing)
{
	struct file *file = get_fd_file(fd, NULL);
	int result;

	if (!file)
		return -EBADF;
	result = file_iov(file, vec, vlen, writing);
	release_file(file, 1);
	return result;
}

int sys_readv(unsigned int fd, struct iovec *vec, int vlen)
{
	return do_iov(fd, vec, vlen, 0);
}

int sys_write(unsigned int fd, char *buf, unsigned int count)
{
	int error;
	struct file *file = get_fd_file(fd, NULL);

	if (!file)
		return -EBADF;
	if (!file->f_inode || !(file->f_mode & 2))
		error = -EBADF;
	else if (!file->f_op || !file->f_op->write)
		error = -EINVAL;
	else if (!count)
		error = 0;
	else if (!(error = verify_area(VERIFY_READ, buf, count)))
		error = file->f_op->write(file->f_inode, file, buf, count);
	release_file(file, 1);
	return error;
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
	return do_iov(fd, vec, vlen, 1);
}
