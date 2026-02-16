#include <termios.h>
#include <unistd.h>

int tcgetattr(int fd, struct termios *termios_p)
{
	return ioctl(fd, TCGETS, termios_p);
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p)
{
	int cmd;

	switch (optional_actions) {
	case TCSANOW:
		cmd = TCSETS;
		break;
	case TCSADRAIN:
		cmd = TCSETSW;
		break;
	case TCSAFLUSH:
		cmd = TCSETSF;
		break;
	default:
		__set_errno(EINVAL);
		return -1;
	}

	return ioctl(fd, cmd, (void *) termios_p);
}

pid_t tcgetsid(int fd)
{
	pid_t sid;
	if (-1 == ioctl(fd, TIOCGSID, &sid))
		return -1;
	return sid;
}

pid_t tcgetpgrp(int fd)
{
	pid_t pgrp;
	if (-1 == ioctl(fd, TIOCGPGRP, &pgrp))
		return -1;
	return pgrp;
}

int tcsetpgrp(int fd, pid_t pgrp)
{
	return ioctl(fd, TIOCSPGRP, &pgrp);
}
