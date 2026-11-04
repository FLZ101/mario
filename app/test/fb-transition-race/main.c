#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <mario/fb.h>

static void wait_for_state(int fb, uint32_t expected)
{
	struct pollfd pollfd = { .fd = fb, .events = POLLPRI };
	uint32_t state;

	for (;;) {
		assert(poll(&pollfd, 1, -1) == 1);
		assert(pollfd.revents & POLLPRI);
		assert(ioctl(fb, FBIOGET_CONSOLE_STATE, &state) == 0);
		if (state == expected)
			return;
	}
}

static void fill_pixels(uint32_t *pixels, unsigned int count, uint32_t color)
{
	unsigned int index;

	for (index = 0; index < count; index++)
		pixels[index] = color;
}

int main(void)
{
	struct fb_fix_screeninfo fix = { 0 };
	uint32_t *pixels;
	int fb;
	int gui;
	int result;

	fb = open("/dev/fb0", O_RDWR);
	assert(fb >= 0);
	gui = open("/dev/gui", O_RDONLY);
	assert(gui >= 0);
	assert(ioctl(fb, FBIOGET_FSCREENINFO, &fix) == 0);
	assert(!(fix.smem_len % sizeof(*pixels)));
	pixels = malloc(fix.smem_len);
	assert(pixels);
	printf("fb-transition-race: ready\n");
	fflush(stdout);

	wait_for_state(fb, FB_CONSOLE_GRAPHICS);
	fill_pixels(pixels, fix.smem_len / sizeof(*pixels), 0x00ff0000);
	result = write(fb, pixels, fix.smem_len);
	assert(result == (int)fix.line_length);
	printf("fb-transition-race: stale=%d\n", result);
	fflush(stdout);

	/* Ignore the F1 completion and redraw only after the following F8 frame. */
	wait_for_state(fb, FB_CONSOLE_GRAPHICS);
	/* The interrupted ordinary write advanced f_pos by its completed scanline. */
	assert(lseek(fb, 0, SEEK_SET) == 0);
	fill_pixels(pixels, fix.smem_len / sizeof(*pixels), 0x0000ff00);
	assert(write(fb, pixels, fix.smem_len) == (int)fix.smem_len);
	printf("fb-transition-race: fresh\n");
	fflush(stdout);

	/* Keep the graphics seat alive until the fixture has sampled the frame. */
	for (;;)
		sleep(1);

	free(pixels);
	assert(close(gui) == 0);
	assert(close(fb) == 0);
	return 0;
}
