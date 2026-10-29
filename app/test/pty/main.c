#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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

	assert(close(slave) == 0);
	assert(close(master) == 0);
	puts("pty-ok");
	return 0;
}
