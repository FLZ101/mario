#include <fs/fs.h>
#include <fs/ioctl.h>

int sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg);

int sys_ioctl(unsigned int fd, unsigned int cmd, unsigned long arg)
{
	struct file *f;
	int result = -EINVAL;

	if (cmd == FIOCLEX || cmd == FIONCLEX)
		return sys_fcntl(fd, F_SETFD, cmd == FIOCLEX);
	f = get_fd_file(fd, NULL);
	if (!f)
		return -EBADF;
	if (f->f_op && f->f_op->ioctl)
		result = f->f_op->ioctl(f->f_inode, f, cmd, arg);
	release_file(f, 1);
	return result;
}
