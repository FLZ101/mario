#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include <mario/input.h>

int main(void)
{
	struct mario_psaux_event event;
	struct pollfd pollfd;
	int fd = open("/dev/psaux", O_RDONLY | O_NONBLOCK);

	assert(fd >= 0);
	assert(open("/dev/psaux", O_RDONLY) == -1);
	assert(errno == EBUSY);
	assert(open("/dev/psaux", O_WRONLY) == -1);
	assert(errno == EACCES);
	assert(read(fd, &event, sizeof(event) - 1) == -1);
	assert(errno == EINVAL);
	assert(read(fd, &event, sizeof(event)) == -1);
	assert(errno == EAGAIN);
	printf("psaux: ready\n");
	fflush(stdout);
	pollfd = (struct pollfd) { .fd = fd, .events = POLLIN };
	for (unsigned int index = 0; index < 5; index++) {
		assert(poll(&pollfd, 1, -1) == 1);
		assert(pollfd.revents == POLLIN);
		assert(read(fd, &event, sizeof(event)) == sizeof(event));
		assert(event.flags == 0 && event.reserved == 0);
		printf("psaux: %d %d %u wheel %d\n", event.dx, event.dy, event.buttons, event.dz);
		fflush(stdout);
		pollfd.revents = 0;
	}
	return 0;
}
