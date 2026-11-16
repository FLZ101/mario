#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <time.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static void test_terminal_attributes(int slave)
{
	struct termios saved;
	struct termios changed;
	struct termios observed;

	assert(tcgetattr(slave, &saved) == 0);
	assert(saved.c_lflag & ICANON);
	assert(saved.c_lflag & ECHO);
	assert(saved.c_oflag & OPOST);
	assert(saved.c_oflag & ONLCR);

	changed = saved;
	changed.c_lflag &= ~(ICANON | ECHO);
	changed.c_oflag &= ~OPOST;
	changed.c_cc[VMIN] = 3;
	changed.c_cc[VTIME] = 4;
	assert(tcsetattr(slave, TCSANOW, &changed) == 0);
	assert(tcgetattr(slave, &observed) == 0);
	assert(!(observed.c_lflag & ICANON));
	assert(!(observed.c_lflag & ECHO));
	assert(!(observed.c_oflag & OPOST));
	assert(observed.c_cc[VMIN] == 3);
	assert(observed.c_cc[VTIME] == 4);

	/* Keep subsequent I/O assertions in the PTY's default canonical mode. */
	assert(tcsetattr(slave, TCSANOW, &saved) == 0);
}

static volatile sig_atomic_t got_winch;

static void handle_winch(int signal_number)
{
	if (signal_number == SIGWINCH)
		got_winch = 1;
}

static void test_resize(int master, int slave)
{
	struct winsize expected = { .ws_row = 43, .ws_col = 97 };
	struct winsize observed;
	char ready_byte;
	int ready[2];
	int status;
	pid_t pid;

	assert(pipe(ready) == 0);
	pid = fork();
	assert(pid >= 0);
	if (!pid) {
		assert(close(master) == 0);
		assert(close(ready[0]) == 0);
		assert(setsid() > 0);
		assert(ioctl(slave, TIOCSCTTY, 0) == 0);
		assert(signal(SIGWINCH, handle_winch) != SIG_ERR);

		/* The parent changes the size only after this foreground pgrp exists. */
		assert(write(ready[1], "r", 1) == 1);
		while (!got_winch)
			pause();
		assert(ioctl(slave, TIOCGWINSZ, &observed) == 0);
		assert(!memcmp(&observed, &expected, sizeof(expected)));
		_exit(0);
	}

	assert(close(ready[1]) == 0);
	assert(read(ready[0], &ready_byte, 1) == 1);
	assert(ioctl(master, TIOCSWINSZ, &expected) == 0);
	assert(ioctl(master, TIOCGWINSZ, &observed) == 0);
	assert(!memcmp(&observed, &expected, sizeof(expected)));
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
	assert(close(ready[0]) == 0);
}

static void test_readv_nonblocking(void)
{
	int master;
	int slave;
	struct termios attributes;
	char input[32];
	char first[8];
	char second[16];
	char third[8];
	struct iovec vectors[] = {
		{ .iov_base = first, .iov_len = sizeof(first) },
		{ .iov_base = second, .iov_len = sizeof(second) },
		{ .iov_base = third, .iov_len = sizeof(third) },
	};

	assert(openpty(&master, &slave, NULL, NULL, NULL) == 0);
	assert(tcgetattr(slave, &attributes) == 0);
	cfmakeraw(&attributes);
	assert(tcsetattr(slave, TCSANOW, &attributes) == 0);
	assert(fcntl(slave, F_SETFL, O_NONBLOCK) == 0);
	memset(input, 'r', sizeof(input));
	memset(first, 'a', sizeof(first));
	memset(second, 'b', sizeof(second));
	memset(third, 'c', sizeof(third));
	assert(readv(slave, vectors, 3) == -1);
	assert(errno == EAGAIN);
	assert(write(master, input, 4) == 4);
	assert(readv(slave, vectors, 3) == 4);
	assert(!memcmp(first, input, 4));
	assert(first[4] == 'a' && second[0] == 'b' && third[0] == 'c');
	assert(write(master, input, sizeof(first)) == sizeof(first));
	assert(readv(slave, vectors, 3) == sizeof(first));
	assert(!memcmp(first, input, sizeof(first)));
	assert(second[0] == 'b' && third[0] == 'c');
	assert(write(master, input, 12) == 12);
	assert(readv(slave, vectors, 3) == 12);
	assert(!memcmp(first, input, sizeof(first)));
	assert(!memcmp(second, input + sizeof(first), 4));
	assert(second[4] == 'b' && third[0] == 'c');
	assert(write(master, input, sizeof(input)) == sizeof(input));
	assert(readv(slave, vectors, 3) == sizeof(input));
	assert(!memcmp(first, input, sizeof(first)));
	assert(!memcmp(second, input + sizeof(first), sizeof(second)));
	assert(!memcmp(third, input + sizeof(first) + sizeof(second), sizeof(third)));
	assert(readv(slave, vectors, 3) == -1);
	assert(errno == EAGAIN);
	assert(close(slave) == 0);
	assert(close(master) == 0);
	puts("pty-readv-nonblocking-ok");
}

static void test_writev_backpressure(void)
{
	int master;
	int slave;
	struct termios attributes;
	char padding[4096];
	char output[4096];
	char first[8];
	char second[32];
	char third[8];
	struct iovec vectors[] = {
		{ .iov_base = first, .iov_len = sizeof(first) },
		{ .iov_base = second, .iov_len = sizeof(second) },
		{ .iov_base = third, .iov_len = sizeof(third) },
	};
	struct pollfd writable;
	ssize_t capacity;

	assert(openpty(&master, &slave, NULL, NULL, NULL) == 0);
	assert(tcgetattr(slave, &attributes) == 0);
	cfmakeraw(&attributes);
	assert(tcsetattr(slave, TCSANOW, &attributes) == 0);
	assert(fcntl(slave, F_SETFL, O_NONBLOCK) == 0);
	memset(padding, 'p', sizeof(padding));
	memset(first, 'a', sizeof(first));
	memset(second, 'b', sizeof(second));
	memset(third, 'c', sizeof(third));
	capacity = write(slave, padding, sizeof(padding));
	assert(capacity >= 24);
	writable = (struct pollfd) { .fd = slave, .events = POLLOUT };
	assert(poll(&writable, 1, 0) == 0);
	assert(writev(slave, vectors, 3) == -1);
	assert(errno == EAGAIN);
	assert(read(master, output, 24) == 24);
	assert(poll(&writable, 1, 0) == 1);
	assert(writable.revents & POLLOUT);
	assert(writev(slave, vectors, 3) == 24);
	assert(poll(&writable, 1, 0) == 0);
	assert(read(master, output, sizeof(output)) == capacity);
	assert(!memcmp(output, padding, capacity - 24));
	assert(!memcmp(output + capacity - 24, first, sizeof(first)));
	assert(!memcmp(output + capacity - 16, second, 16));

	assert(write(slave, padding, capacity) == capacity);
	assert(read(master, output, sizeof(first)) == sizeof(first));
	assert(writev(slave, vectors, 2) == sizeof(first));
	assert(read(master, output, sizeof(output)) == capacity);
	assert(!memcmp(output, padding, capacity - sizeof(first)));
	assert(!memcmp(output + capacity - sizeof(first), first, sizeof(first)));
	assert(writev(slave, vectors, 3) ==
	       sizeof(first) + sizeof(second) + sizeof(third));
	assert(read(master, output, sizeof(output)) ==
	       sizeof(first) + sizeof(second) + sizeof(third));
	assert(!memcmp(output, first, sizeof(first)));
	assert(!memcmp(output + sizeof(first), second, sizeof(second)));
	assert(!memcmp(output + sizeof(first) + sizeof(second), third, sizeof(third)));
	assert(close(slave) == 0);
	assert(close(master) == 0);
	puts("pty-writev-backpressure-ok");
}

static void test_slave_poll_read_timeout(void)
{
	int master;
	int slave;
	int start[2];
	int release[2];
	int status;
	pid_t child;
	struct termios attributes;
	struct pollfd readable;
	struct timespec started;
	struct timespec finished;
	char padding[4096];
	char signal_byte;
	long elapsed_ms;

	assert(openpty(&master, &slave, NULL, NULL, NULL) == 0);
	assert(tcgetattr(slave, &attributes) == 0);
	cfmakeraw(&attributes);
	assert(tcsetattr(slave, TCSANOW, &attributes) == 0);
	assert(fcntl(slave, F_SETFL, O_NONBLOCK) == 0);
	memset(padding, 'p', sizeof(padding));
	assert(write(slave, padding, sizeof(padding)) >= 24);
	assert(pipe(start) == 0);
	assert(pipe(release) == 0);
	child = fork();
	assert(child >= 0);
	if (!child) {
		assert(close(slave) == 0);
		assert(close(start[1]) == 0);
		assert(close(release[1]) == 0);
		assert(read(start[0], &signal_byte, 1) == 1);
		assert(usleep(50000) == 0);
		assert(read(master, padding, 24) == 24);
		assert(read(release[0], &signal_byte, 1) == 1);
		_exit(0);
	}
	assert(close(start[0]) == 0);
	assert(close(release[0]) == 0);
	readable = (struct pollfd) { .fd = slave, .events = POLLIN };
	assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
	assert(write(start[1], "s", 1) == 1);
	assert(poll(&readable, 1, 200) == 0);
	assert(clock_gettime(CLOCK_MONOTONIC, &finished) == 0);
	elapsed_ms = (finished.tv_sec - started.tv_sec) * 1000 +
		(finished.tv_nsec - started.tv_nsec) / 1000000;
	assert(elapsed_ms >= 150);
	assert(readable.revents == 0);
	assert(write(release[1], "r", 1) == 1);
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(close(start[1]) == 0);
	assert(close(release[1]) == 0);
	assert(close(slave) == 0);
	assert(close(master) == 0);
	puts("pty-slave-poll-timeout-ok");
}

static void test_slave_poll_wakeup(void)
{
	int master;
	int slave;
	int start[2];
	int release[2];
	int status;
	pid_t child;
	struct termios attributes;
	struct pollfd writable;
	char padding[4096];
	char signal_byte;
	ssize_t capacity;

	assert(openpty(&master, &slave, NULL, NULL, NULL) == 0);
	assert(tcgetattr(slave, &attributes) == 0);
	cfmakeraw(&attributes);
	assert(tcsetattr(slave, TCSANOW, &attributes) == 0);
	assert(fcntl(slave, F_SETFL, O_NONBLOCK) == 0);
	memset(padding, 'p', sizeof(padding));
	capacity = write(slave, padding, sizeof(padding));
	assert(capacity >= 24);
	writable = (struct pollfd) { .fd = slave, .events = POLLOUT };
	assert(poll(&writable, 1, 0) == 0);
	assert(pipe(start) == 0);
	assert(pipe(release) == 0);
	child = fork();
	assert(child >= 0);
	if (!child) {
		assert(close(slave) == 0);
		assert(close(start[1]) == 0);
		assert(close(release[1]) == 0);
		assert(read(start[0], &signal_byte, 1) == 1);
		assert(usleep(50000) == 0);
		assert(read(master, padding, 24) == 24);
		assert(read(release[0], &signal_byte, 1) == 1);
		_exit(0);
	}
	assert(close(start[0]) == 0);
	assert(close(release[0]) == 0);
	assert(write(start[1], "s", 1) == 1);
	assert(poll(&writable, 1, 1000) == 1);
	assert(writable.revents & POLLOUT);
	assert(write(release[1], "r", 1) == 1);
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(close(start[1]) == 0);
	assert(close(release[1]) == 0);
	assert(close(slave) == 0);
	assert(close(master) == 0);
	puts("pty-slave-poll-ok");
}

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

	test_readv_nonblocking();
	test_writev_backpressure();
	test_slave_poll_read_timeout();
	test_slave_poll_wakeup();
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
	test_terminal_attributes(slave);
	test_resize(master, slave);
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
	puts("pty-setup-ok");

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
	puts("pty-resize-hangup-ok");

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
