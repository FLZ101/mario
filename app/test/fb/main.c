#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <mario/fb.h>

#define BLOCK_SIZE 8

int main(void)
{
	struct fb_var_screeninfo var = { 0 };
	struct fb_fix_screeninfo fix = { 0 };
	uint32_t pixels[BLOCK_SIZE];
	unsigned int row;
	int fd = open("/dev/fb0", O_RDWR);

	assert(fd >= 0);
	assert(ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0);
	assert(ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0);
	assert(var.xres && var.yres && var.bits_per_pixel == 32);
	assert(fix.smem_len >= fix.line_length * var.yres);
	for (row = 0; row < BLOCK_SIZE; row++)
		pixels[row] = 0x00ff0000;
	assert(write(fd, pixels, sizeof(pixels)) == -1);
	assert(errno == EBUSY);
	assert(close(fd) == 0);
	printf("fb0: %ux%u bpp=%u stride=%u\n", var.xres, var.yres,
	       var.bits_per_pixel, fix.line_length);
	return 0;
}
