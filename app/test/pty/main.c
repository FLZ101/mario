#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
	char path[32];
	char buf[16];
	int locked = 0;
	int number;
	int master;
	int slave;

	master = open("/dev/ptmx", O_RDWR);
	assert(master >= 0);
	assert(ioctl(master, TIOCGPTN, &number) == 0);
	assert(number >= 0);
	snprintf(path, sizeof(path), "/dev/pts/%d", number);

	slave = open(path, O_RDWR);
	assert(slave == -1);
	assert(errno == EIO);

	assert(ioctl(master, TIOCSPTLCK, &locked) == 0);
	slave = open(path, O_RDWR | O_NOCTTY);
	assert(slave >= 0);
	if (!fork()) {
		int tty;

		assert(setsid() > 0);
		assert(ioctl(slave, TIOCSCTTY, 0) == 0);
		tty = open("/dev/tty", O_RDWR);
		assert(tty >= 0);
		assert(write(tty, "ctty\n", 5) == 5);
		assert(signal(SIGHUP, SIG_IGN) != SIG_ERR);
		assert(ioctl(slave, TIOCNOTTY) == 0);
		assert(open("/dev/tty", O_RDWR) == -1);
		assert(errno == ENXIO);
		_exit(0);
	}
	assert(wait(NULL) > 0);
	assert(read(master, buf, 6) == 6);
	assert(!memcmp(buf, "ctty\r\n", 6));

	assert(write(master, "ping\n", 5) == 5);
	assert(read(slave, buf, 5) == 5);
	assert(!memcmp(buf, "ping\n", 5));
	assert(read(master, buf, 6) == 6);
	assert(!memcmp(buf, "ping\r\n", 6));

	assert(write(slave, "pong\n", 5) == 5);
	assert(read(master, buf, 6) == 6);
	assert(!memcmp(buf, "pong\r\n", 6));

	assert(write(slave, "drain\n", 6) == 6);
	assert(close(slave) == 0);
	assert(read(master, buf, 7) == 7);
	assert(!memcmp(buf, "drain\r\n", 7));
	assert(read(master, buf, sizeof(buf)) == -1);
	assert(errno == EIO);
	{
		struct pollfd pfd = { .fd = master, .events = POLLIN };

		assert(poll(&pfd, 1, 0) == 1);
		assert((pfd.revents & (POLLERR | POLLHUP)) ==
		       (POLLERR | POLLHUP));
	}
	assert(close(master) == 0);

	master = open("/dev/ptmx", O_RDWR);
	assert(master >= 0);
	assert(ioctl(master, TIOCGPTN, &number) == 0);
	assert(number == 0);
	snprintf(path, sizeof(path), "/dev/pts/%d", number);
	assert(ioctl(master, TIOCSPTLCK, &locked) == 0);
	slave = open(path, O_RDWR | O_NOCTTY);
	assert(slave >= 0);
	{
		int ready[2];
		int status;

		assert(pipe(ready) == 0);
		if (!fork()) {
			char byte;

			assert(close(master) == 0);
			assert(close(ready[0]) == 0);
			assert(write(ready[1], "r", 1) == 1);
			assert(read(slave, &byte, 1) == 0);
			assert(write(slave, "x", 1) == -1);
			assert(errno == EIO);
			_exit(0);
		}
		assert(close(ready[1]) == 0);
		assert(read(ready[0], buf, 1) == 1);
		assert(close(slave) == 0);
		assert(close(master) == 0);
		assert(wait(&status) > 0);
		assert(WIFEXITED(status));
		assert(WEXITSTATUS(status) == 0);
		assert(close(ready[0]) == 0);
	}

	puts("pty-ok");
	return 0;
}
