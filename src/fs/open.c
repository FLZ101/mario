#include <fs/fs.h>

extern int get_base_inode(int dirfd, int path_empty, struct inode **res_inode);

extern uint64_t current_time();

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
int do_openat(unsigned int dirfd, char *filename, int flags)
{
	struct inode *i;
	struct file *f;
	int namei_flags, error, fd;

	struct inode *base_i = NULL;

	for (fd = 0; ; fd++) {
		if (fd == NR_OPEN)
			return -EMFILE;
		if (!current->files->fd[fd])
			break;
	}
	if (flags & O_CLOEXEC)
		FD_SET(fd, &current->files->close_on_exec);
	else
		FD_CLR(fd, &current->files->close_on_exec);
	if (!(f = get_empty_file()))
		return -ENFILE;

	f->f_flags = namei_flags = flags;
	f->f_mode = (namei_flags+1) & O_ACCMODE;
	if (f->f_mode)
		namei_flags++;
	if (namei_flags & (O_TRUNC | O_CREAT))
		namei_flags |= 2;

	error = get_base_inode(dirfd, 0, &base_i);
	if (error) {
		put_file(f);
		return error;
	}
	error = open_namei(filename, namei_flags, &i, base_i);
	if (error) {
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
	current->files->fd[fd] = f;
	f->f_flags &= ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);
	i->i_atime = i->i_mtime = i->i_ctime = current_time();
	return fd;
}

int sys_openat(int dirfd, char *pathname, int flags)
{
	char *tmp;
	int error;

	error = getname(pathname, &tmp);
	if (error)
		return error;
	error = do_openat(dirfd, tmp, flags);
	putname(tmp);
	return error;
}

int sys_open(char *filename, int flags)
{
	return sys_openat(AT_FDCWD, filename, flags);
}

int sys_creat(char *pathname)
{
	return sys_open(pathname, O_CREAT | O_WRONLY | O_TRUNC);
}

int sys_symlinkat(char *target, int newdirfd, char *linkpath)
{
	char *name;
	int error = getname(linkpath, &name);
	if (error)
		return error;

	char *target_name;
	error = getname(target, &target_name);
	if (error) {
		putname(name);
		return error;
	}

	struct inode *base_i;
	error = get_base_inode(newdirfd, 0, &base_i);
	if (error)
		goto tail_1;

	int basename_len;
	char *basename;
	struct inode *inode;
	error = dir_namei(name, &basename_len, &basename, base_i, &inode);
	if (error) {
		goto tail_1;
	}

	if (!inode->i_op || !inode->i_op->symlink) {
		iput(inode);
		error = -EBADF;
		goto tail_1;
	}

	error = inode->i_op->symlink(inode, basename, basename_len, target_name);

tail_1:
	putname(name);
	putname(target_name);
	return error;
}

int sys_symlink(char *target, char *linkpath)
{
	return sys_symlinkat(target, AT_FDCWD, linkpath);
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
	release_file(f, 1);
	return 0;
}

int sys_truncate(const char *path, int length)
{
	struct inode *inode;
	int error;

	error = namei(path, &inode, 0);
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

int sys_truncate64(const char *path, int length_lo, int length_hi)
{
	return sys_truncate(path, length_lo);
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
	return error;
}

int sys_ftruncate64(unsigned int fd, int length_lo, int length_hi)
{
	return sys_ftruncate(fd, length_lo);
}

int sys_chdir(const char *filename)
{
	struct inode *inode;
	int error;

	error = namei(filename, &inode, 1);
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

	error = namei(filename, &inode, 1);
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

int sys_faccessat(int dirfd, const char *pathname, int mode, int flags)
{
	struct inode *inode;
	int err = namei_at(dirfd, pathname, &inode, !(flags & AT_SYMLINK_NOFOLLOW));
	if (err)
		return err;
	iput(inode);
	return 0;
}

int sys_access(const char *pathname, int mode)
{
	return sys_faccessat(AT_FDCWD, pathname, mode, 0);
}

static int do_utimensat(struct inode *inode, struct timespec64 *times, int flags)
{
	uint64_t cur = current_time();

	if (!times) {
		inode->i_atime = inode->i_mtime = cur;
	} else {
		if (times[0].tv_nsec == UTIME_NOW) {
			inode->i_atime = cur;
		} else if (times[0].tv_nsec != UTIME_OMIT) {
			inode->i_atime = times[0].tv_sec;
		}

		if (times[1].tv_nsec == UTIME_NOW) {
			inode->i_mtime = cur;
		} else if (times[1].tv_nsec != UTIME_OMIT) {
			inode->i_mtime = times[1].tv_sec;
		}
	}
	inode->i_ctime = cur;
	set_bit(I_Dirty, &inode->i_state);
	return 0;
}

int sys_utimensat_time64(int dirfd, char *pathname, struct timespec64 *times, int flags)
{
	struct inode *inode;
	int err = namei_at(dirfd, pathname, &inode, !(flags & AT_SYMLINK_NOFOLLOW));
	if (err)
		return err;

	struct timespec64 k[2];

	if (times) {
		err = verify_area(VERIFY_READ, times, sizeof(k));
		if (err)
			goto tail;
		memcpy_fromfs(k, times, sizeof(k));
	}

	err = do_utimensat(inode, times ? k : NULL, flags);

tail:
	iput(inode);
	return err;
}

int sys_utimensat(int dirfd, char *pathname, struct timespec *times, int flags)
{
	struct inode *inode;
	int err = namei_at(dirfd, pathname, &inode, !(flags & AT_SYMLINK_NOFOLLOW));
	if (err)
		return err;

	struct timespec k[2];
	struct timespec64 k64[2];

	if (times) {
		err = verify_area(VERIFY_READ, times, sizeof(k));
		if (err)
			goto tail;
		memcpy_fromfs(k, times, sizeof(k));

		k64[0].tv_sec = k[0].tv_sec;
		k64[0].tv_nsec = k[0].tv_nsec;
		k64[1].tv_sec = k[1].tv_sec;
		k64[1].tv_nsec = k[1].tv_nsec;
	}

	err = do_utimensat(inode, times ? k64 : NULL, flags);

tail:
	iput(inode);
	return err;
}

int sys_fsync(int fd)
{
	return 0;
}
