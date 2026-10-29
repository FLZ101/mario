#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
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
	char input[2048];
	char echoed[4096];
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

	master = open("/dev/ptmx", O_RDWR);
	assert(master >= 0);
	assert(ioctl(master, TIOCGPTN, &number) == 0);
	snprintf(path, sizeof(path), "/dev/pts/%d", number);
	assert(ioctl(master, TIOCSPTLCK, &locked) == 0);
	slave = open(path, O_RDWR | O_NOCTTY);
	assert(slave >= 0);
	memset(input, 1, sizeof(input));
	assert(fcntl(master, F_SETFL, O_NONBLOCK) == 0);
	assert(write(master, input, sizeof(input)) == sizeof(input));
	assert(write(master, "x", 1) == -1);
	assert(errno == EAGAIN);
	{
		struct pollfd pfd = { .fd = master, .events = POLLIN | POLLOUT };

		assert(poll(&pfd, 1, 0) == 1);
		assert(!(pfd.revents & POLLOUT));
	}
	assert(read(master, echoed, sizeof(echoed)) == sizeof(echoed));
	for (int i = 0; i < sizeof(echoed); i += 2) {
		assert(echoed[i] == '^');
		assert(echoed[i + 1] == 'A');
	}
	assert(write(master, "x\n", 2) == 2);
	assert(read(slave, input, sizeof(input)) == sizeof(input));
	assert(read(slave, buf, 2) == 2);
	assert(buf[0] == 'x');
	assert(buf[1] == '\n');
	assert(read(master, buf, 3) == 3);
	assert(!memcmp(buf, "x\r\n", 3));
	assert(close(slave) == 0);
	assert(close(master) == 0);
	{
		char shell_output[2048];
		int status;
		int length = 0;
		pid_t pid = forkpty(&master, NULL, NULL, NULL);

		assert(pid >= 0);
		if (!pid) {
			execl("/bin/oksh", "oksh", "-c", "echo pty-shell", NULL);
			_exit(127);
		}
		for (;;) {
			int n = read(master, shell_output + length,
					 sizeof(shell_output) - length - 1);

			if (n > 0) {
				length += n;
				continue;
			}
			assert(n == -1);
			assert(errno == EIO);
			break;
		}
		shell_output[length] = 0;
		assert(strstr(shell_output, "pty-shell\r\n"));
		assert(waitpid(pid, &status, 0) == pid);
		assert(WIFEXITED(status));
		assert(WEXITSTATUS(status) == 0);
		assert(close(master) == 0);
	}

	puts("pty-ok");
	return 0;
}
