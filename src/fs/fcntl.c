#include <fs/fs.h>

extern int sys_close(unsigned int fd);

static int dupfd(unsigned int fd, unsigned int arg)
{
	if (fd >= NR_OPEN || !current->files->fd[fd])
		return -EBADF;
	if (arg >= NR_OPEN)
		return -EINVAL;
	for (; arg < NR_OPEN && current->files->fd[fd]; arg++)
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