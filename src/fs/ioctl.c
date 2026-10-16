#include <fs/fs.h>
#include <fs/ioctl.h>

static int file_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	if (f->f_op && f->f_op->ioctl)
		return f->f_op->ioctl(f->f_inode, f, cmd, arg);
	return -EINVAL;
}

int sys_ioctl(unsigned int fd, unsigned int cmd, unsigned long arg)
{
	struct file *f;

	if (fd >= NR_OPEN || !(f = current->files->fd[fd]))
		return -EBADF;

	switch (cmd) {
		case FIOCLEX:
			FD_SET(fd, &current->files->close_on_exec);
			return 0;
		case FIONCLEX:
			FD_CLR(fd, &current->files->close_on_exec);
			return 0;
		default:
			if (f->f_inode && S_ISREG(f->f_inode->i_mode))
				return file_ioctl(f, cmd, arg);
			if (f->f_op && f->f_op->ioctl)
				return f->f_op->ioctl(f->f_inode, f, cmd,arg);
			return -EINVAL;
	}
}
