#include <fs/fs.h>

/*
 * Note that while the flag value (low two bits) for sys_open means:
 *	00 - read-only
 *	01 - write-only
 *	10 - read-write
 *	11 - special
 * it is changed into
 *	00 - no permissions needed
 *	01 - read-permission
 *	10 - write-permission
 *	11 - read-write
 * for the internal routines (i.e. open_namei()/follow_link() etc). 00 is
 * used by symlinks.
 */
int do_open(char *filename, int flags)
{
	struct inode *i;
	struct file *f;
	int namei_flags, error, fd;

	for (fd = 0; ; fd++) {
		if (fd == NR_OPEN)
			return -EMFILE;
		if (!current->files->fd[fd])
			break;
	}
	FD_CLR(fd, &current->files->close_on_exec);
	if (!(f = get_empty_file()))
		return -ENFILE;
	current->files->fd[fd] = f;

	f->f_flags = namei_flags = flags;
	f->f_mode = (namei_flags+1) & O_ACCMODE;
	if (f->f_mode)
		namei_flags++;
	if (namei_flags & (O_TRUNC | O_CREAT))
		namei_flags |= 2;

	error = open_namei(filename, namei_flags, &i, NULL);
	if (error) {
		current->files->fd[fd] = NULL;
		put_file(f);
		return error;
	}
	f->f_inode = i;
	f->f_pos = 0;
	f->f_op = NULL;
	if (i->i_fop)
		f->f_op = i->i_fop;
	if (f->f_op && f->f_op->open) {
		error = f->f_op->open(i, f);
		if (error) {
			put_file(f);
			return error;
		}
	}
	f->f_flags &= ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);
	return fd;
}

int sys_open(const char *filename, int flags)
{
	char *tmp;
	int error;

	error = getname(filename, &tmp);
	if (error)
		return error;
	error = do_open(tmp, flags);
	putname(tmp);
	return error;
}

int sys_creat(const char *pathname)
{
	return sys_open(pathname, O_CREAT | O_WRONLY | O_TRUNC);
}

int sys_close(unsigned int fd)
{
	struct file *f;

	if (fd >= NR_OPEN)
		return -EBADF;
	FD_CLR(fd, &current->files->close_on_exec);
	if (!(f = current->files->fd[fd]))
		return -EBADF;
	current->files->fd[fd] = NULL;
	put_file(f);
	return 0;
}

int sys_truncate(const char *path, int length)
{
	struct inode *inode;
	int error;

	error = namei(path, &inode);
	if (error)
		return error;
	if (S_ISDIR(inode->i_mode))
		return -EACCES;
	if (!inode->i_op || !inode->i_op->truncate)
		return -EINVAL;
	down(&inode->i_sem);
	error = inode->i_op->truncate(inode, length);
	up(&inode->i_sem);
	iput(inode);
	return error;
}

int sys_ftruncate(unsigned int fd, int length)
{
	struct inode *inode;
	struct file *file;
	int error;

	if (fd >= NR_OPEN || !(file = current->files->fd[fd]))
		return -EBADF;
	if (!(inode = file->f_inode))
		return -ENOENT;
	if (S_ISDIR(inode->i_mode) || !(file->f_mode & 2))
		return -EACCES;
	if (!inode->i_op || !inode->i_op->truncate)
		return -EINVAL;
	down(&inode->i_sem);
	error = inode->i_op->truncate(inode, length);
	up(&inode->i_sem);
	iput(inode);
	return error;
}

int sys_chdir(const char *filename)
{
	struct inode *inode;
	int error;

	error = namei(filename, &inode);
	if (error)
		return error;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	iput(current->fs->pwd);
	current->fs->pwd = inode;
	return 0;
}

int sys_fchdir(unsigned int fd)
{
	struct inode *inode;
	struct file *file;

	if (fd >= NR_OPEN || !(file = current->files->fd[fd]))
		return -EBADF;
	if (!(inode = file->f_inode))
		return -ENOENT;
	if (!S_ISDIR(inode->i_mode))
		return -ENOTDIR;
	iput(current->fs->pwd);
	iref(inode);
	current->fs->pwd = inode;
	return 0;
}

int sys_chroot(char *filename)
{
	struct inode *inode;
	int error;

	error = namei(filename, &inode);
	if (error)
		return error;
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}
	iput(current->fs->root);
	current->fs->root = inode;
	return 0;
}
