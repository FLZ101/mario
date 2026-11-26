#include <fs/fs.h>
#include <fs/ioctl.h>
#include <mm/uaccess.h>

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
	if (cmd == FIONBIO) {
		int *enabled = (int *)arg;

		result = verify_area(VERIFY_READ, enabled, sizeof(*enabled));
		if (!result)
			file_set_nonblock(f, get_fs_long(enabled) != 0);
	} else if (f->f_op && f->f_op->ioctl) {
		result = f->f_op->ioctl(f->f_inode, f, cmd, arg);
	}
	release_file(f, 1);
	return result;
}
