#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <mario/fb.h>

static void wait_for_state(int fb, uint32_t expected)
{
	struct pollfd pollfd = { .fd = fb, .events = POLLPRI };
	uint32_t state;

	assert(poll(&pollfd, 1, -1) == 1);
	assert(pollfd.revents & POLLPRI);
	assert(ioctl(fb, FBIOGET_CONSOLE_STATE, &state) == 0);
	assert(state == expected);
}

int main(void)
{
	struct fb_fix_screeninfo fix = { 0 };
	uint32_t descriptor_pixels[4] = {
		0x00ff0000, 0x00ff0000, 0x00ff0000, 0x00ff0000,
	};
	uint32_t positional_pixels[4] = {
		0x0000ff00, 0x0000ff00, 0x0000ff00, 0x0000ff00,
	};
	uint32_t readback[4] = { 0 };
	off_t position;
	int fb;
	int gui;

	fb = open("/dev/fb0", O_RDWR);
	assert(fb >= 0);
	gui = open("/dev/gui", O_RDONLY);
	assert(gui >= 0);
	assert(ioctl(fb, FBIOGET_FSCREENINFO, &fix) == 0);
	assert(fix.line_length >= sizeof(descriptor_pixels));

	/* Keep a shared descriptor offset distinct from the positional row. */
	assert(lseek(fb, fix.line_length, SEEK_SET) == fix.line_length);
	position = lseek(fb, 0, SEEK_CUR);
	printf("fb-positional: ready\n");
	fflush(stdout);

	wait_for_state(fb, FB_CONSOLE_GRAPHICS);
	/* A positional write must not redirect the shared descriptor's next write. */
	assert(pwrite(fb, positional_pixels, sizeof(positional_pixels),
		      2 * fix.line_length) == sizeof(positional_pixels));
	assert(lseek(fb, 0, SEEK_CUR) == position);
	assert(pread(fb, readback, sizeof(readback), 2 * fix.line_length) ==
	       sizeof(readback));
	assert(!memcmp(readback, positional_pixels, sizeof(positional_pixels)));
	assert(lseek(fb, 0, SEEK_CUR) == position);
	assert(write(fb, descriptor_pixels, sizeof(descriptor_pixels)) ==
	       sizeof(descriptor_pixels));
	position = lseek(fb, 0, SEEK_CUR);
	assert(position == fix.line_length + sizeof(descriptor_pixels));
	assert(pread(fb, readback, sizeof(readback), fix.line_length) ==
	       sizeof(readback));
	assert(!memcmp(readback, descriptor_pixels, sizeof(descriptor_pixels)));
	printf("fb-positional: graphics\n");
	fflush(stdout);

	wait_for_state(fb, FB_CONSOLE_TEXT);
	errno = 0;
	assert(pwrite(fb, positional_pixels, sizeof(positional_pixels),
		      2 * fix.line_length) == -1);
	assert(errno == EBUSY);
	assert(lseek(fb, 0, SEEK_CUR) == position);
	printf("fb-positional: text\n");
	fflush(stdout);

	assert(close(gui) == 0);
	assert(close(fb) == 0);
	return 0;
}
