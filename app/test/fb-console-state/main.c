#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <mario/fb.h>

int main(void)
{
	struct pollfd pollfd;
	uint32_t state;
	uint8_t pixel = 0;
	int fb = open("/dev/fb0", O_RDWR);
	int gui;

	assert(fb >= 0);
	gui = open("/dev/gui", O_RDONLY);
	assert(gui >= 0);
	assert(ioctl(fb, FBIOGET_CONSOLE_STATE, &state) == 0);
	assert(state == FB_CONSOLE_TEXT);
	assert(write(fb, &pixel, sizeof(pixel)) == -1);
	assert(errno == EBUSY);
	printf("fb-state: ready\n");
	fflush(stdout);
	pollfd = (struct pollfd) { .fd = fb, .events = POLLPRI };
	for (unsigned int index = 0; index < 2; index++) {
		assert(poll(&pollfd, 1, -1) == 1);
		assert(pollfd.revents & POLLPRI);
		assert(ioctl(fb, FBIOGET_CONSOLE_STATE, &state) == 0);
		if (state == FB_CONSOLE_GRAPHICS)
			assert(write(fb, &pixel, sizeof(pixel)) == sizeof(pixel));
		else {
			assert(write(fb, &pixel, sizeof(pixel)) == -1);
			assert(errno == EBUSY);
		}
		printf("fb-state: %u\n", state);
		fflush(stdout);
		pollfd.revents = 0;
	}
	return 0;
}
