#include <fs/fs.h>

static void copy_stat(struct inode *inode, struct stat *statbuf)
{
	struct stat tmp;

	memset(&tmp, 0, sizeof(tmp));
	tmp.st_dev = inode->i_dev;
	tmp.st_ino = inode->i_ino;
	tmp.st_mode = inode->i_mode;
	tmp.st_nlink = inode->i_nlink;
	tmp.st_rdev = inode->i_rdev;
	tmp.st_size = inode->i_size;
	tmp.st_blksize = inode->i_block_size;
	tmp.st_blocks = inode->i_nr_block;
	memcpy_tofs((char *)statbuf, (char *)&tmp, sizeof(tmp));
}

int sys_stat(char *filename, struct stat *statbuf)
{
	struct inode *inode;
	int error;

	error = verify_area(VERIFY_WRITE, statbuf, sizeof(struct stat));
	if (error)
		return error;
	error = namei(filename, &inode);
	if (error)
		return error;
	copy_stat(inode, statbuf);
	iput(inode);
	return 0;
}

int sys_fstat(unsigned int fd, struct stat *statbuf)
{
	struct file *f;
	struct inode *inode;
	int error;

	error = verify_area(VERIFY_WRITE, statbuf, sizeof(struct stat));
	if (error)
		return error;
	if (fd >= NR_OPEN || !(f=current->files->fd[fd]) || !(inode=f->f_inode))
		return -EBADF;
	copy_stat(inode, statbuf);
	return 0;
}