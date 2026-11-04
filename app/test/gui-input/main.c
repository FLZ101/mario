#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include <mario/input.h>

int main(void)
{
	struct mario_gui_key_event event;
	struct pollfd pollfd;
	int fb = open("/dev/fb0", O_RDWR);
	int gui;

	assert(fb >= 0);
	gui = open("/dev/gui", O_RDONLY | O_NONBLOCK);
	assert(gui >= 0);
	assert(open("/dev/gui", O_RDONLY) == -1);
	assert(errno == EBUSY);
	assert(open("/dev/gui", O_WRONLY) == -1);
	assert(errno == EACCES);
	assert(read(gui, &event, sizeof(event) - 1) == -1);
	assert(errno == EINVAL);
	assert(read(gui, &event, sizeof(event)) == -1);
	assert(errno == EAGAIN);
	printf("gui-input: ready\n");
	fflush(stdout);
	pollfd = (struct pollfd) { .fd = gui, .events = POLLIN };
	assert(poll(&pollfd, 1, -1) == 1);
	assert(pollfd.revents == POLLIN);
	printf("gui-input: poll\n");
	fflush(stdout);
	for (unsigned int index = 0; index < 4; index++) {
		while (read(gui, &event, sizeof(event)) == -1) {
			assert(errno == EAGAIN);
			pollfd.revents = 0;
			assert(poll(&pollfd, 1, -1) == 1);
			assert(pollfd.revents == POLLIN);
		}
		printf("gui-input: %x %u\n", event.scancode, event.pressed);
		fflush(stdout);
	}
	return 0;
}
