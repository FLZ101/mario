#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <mario/fb.h>

#define TEST_BUFFER_SIZE 8192

static void wait_for_graphics(int fb)
{
	struct pollfd pollfd = { .fd = fb, .events = POLLPRI };
	uint32_t state;

	assert(poll(&pollfd, 1, -1) == 1);
	assert(pollfd.revents & POLLPRI);
	assert(ioctl(fb, FBIOGET_CONSOLE_STATE, &state) == 0);
	assert(state == FB_CONSOLE_GRAPHICS);
}

int main(void)
{
	struct fb_fix_screeninfo fix = { 0 };
	char pixels[TEST_BUFFER_SIZE];
	int fb;
	int gui;
	int result;

	memset(pixels, 0xa5, sizeof(pixels));
	fb = open("/dev/fb0", O_RDWR);
	assert(fb >= 0);
	gui = open("/dev/gui", O_RDONLY);
	assert(gui >= 0);
	assert(ioctl(fb, FBIOGET_FSCREENINFO, &fix) == 0);
	assert(fix.line_length * 2 <= sizeof(pixels));
	printf("fb-write-chunks: ready\n");
	fflush(stdout);

	wait_for_graphics(fb);
	/* The final partial scanline remains a short successful write. */
	assert(pwrite(fb, pixels, 8, fix.smem_len - 2) == 2);
	result = write(fb, pixels, fix.line_length * 2);
	/* The test kernel invalidates the generation after the first scanline. */
	assert(result == (int)fix.line_length);
	printf("fb-write-chunks: completed=%d\n", result);
	fflush(stdout);

	assert(close(gui) == 0);
	assert(close(fb) == 0);
	return 0;
}
