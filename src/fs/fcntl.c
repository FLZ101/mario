#include <fs/fs.h>

extern int sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg);

int sys_dup2(unsigned int old_fd, unsigned int new_fd)
{
	return dup_fd_file(old_fd, new_fd, 1, 0);
}

int sys_dup(unsigned int old_fd)
{
	return dup_fd_file(old_fd, 0, 0, 0);
}

int sys_fcntl64(unsigned int fd, unsigned int cmd, unsigned long arg)
{
	return sys_fcntl(fd, cmd, arg);
}
