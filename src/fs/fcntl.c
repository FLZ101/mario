#include <fs/fs.h>

extern int sys_close(unsigned int fd);

static int dupfd(unsigned int fd, unsigned int arg)
{
	if (fd >= NR_OPEN || !current->files->fd[fd])
		return -EBADF;
	if (arg >= NR_OPEN)
		return -EINVAL;
	for (; arg < NR_OPEN && current->files->fd[arg]; arg++)
		;
	if (arg >= NR_OPEN)
		return -EMFILE;
	FD_CLR(arg, &current->files->close_on_exec);
	(current->files->fd[arg] = current->files->fd[fd])->f_count++;
	return arg;
}

int sys_dup2(unsigned int old_fd, unsigned int new_fd)
{
	if (old_fd >= NR_OPEN || !current->files->fd[old_fd])
		return -EBADF;
	if (new_fd == old_fd)
		return new_fd;

	if (new_fd >= NR_OPEN)
		return -EBADF;

	sys_close(new_fd);
	return dupfd(old_fd, new_fd);
}

int sys_dup(unsigned int old_fd)
{
	return dupfd(old_fd, 0);
}

int sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg)
{
	struct file *f;
	if (fd >= NR_OPEN || !(f = current->files->fd[fd]))
		return -EBADF;

	fd_set *close_on_exec = &current->files->close_on_exec;

	switch (cmd) {
	case F_DUPFD:
		return dupfd(fd, arg);

	case F_GETFD:
		return FD_ISSET(fd, close_on_exec);

	case F_SETFD:
		if (arg & 1)
			FD_SET(fd, close_on_exec);
		else
			FD_CLR(fd, close_on_exec);
		return 0;

	case F_GETFL:
		return f->f_flags;

	case F_SETFL:
		f->f_flags &= ~(O_APPEND | O_NONBLOCK);
		f->f_flags |= arg & (O_APPEND | O_NONBLOCK);
		return 0;

	default:
		return -EINVAL;
	}
}
