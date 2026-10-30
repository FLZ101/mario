#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define BLOCK_SIZE 8

struct fb_bitfield {
	uint32_t offset;
	uint32_t length;
};

struct fb_var_screeninfo {
	uint32_t xres, yres, xres_virtual, yres_virtual;
	uint32_t xoffset, yoffset;
	uint32_t bits_per_pixel, grayscale;
	struct fb_bitfield red, green, blue, transp;
	uint32_t rest[17];
};

struct fb_fix_screeninfo {
	char id[16];
	uint64_t smem_start;
	uint32_t smem_len, type, type_aux, visual;
	uint16_t xpanstep, ypanstep, ywrapstep;
	uint32_t line_length, mmio_start, mmio_len, accel;
	uint16_t capabilities;
	uint16_t reserved[2];
};

int main(void)
{
	struct fb_var_screeninfo var = { 0 };
	struct fb_fix_screeninfo fix = { 0 };
	uint32_t pixels[BLOCK_SIZE];
	uint32_t readback[BLOCK_SIZE];
	unsigned int row;
	int fd = open("/dev/fb0", O_RDWR);

	assert(fd >= 0);
	assert(ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0);
	assert(ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0);
	assert(var.xres && var.yres && var.bits_per_pixel == 32);
	assert(fix.smem_len >= fix.line_length * var.yres);
	for (row = 0; row < BLOCK_SIZE; row++)
		pixels[row] = 0x00ff0000;
	for (row = 0; row < BLOCK_SIZE; row++) {
		assert(lseek(fd, row * fix.line_length, SEEK_SET) ==
		       (off_t)(row * fix.line_length));
		assert(write(fd, pixels, sizeof(pixels)) == sizeof(pixels));
	}
	for (row = 0; row < BLOCK_SIZE; row++) {
		assert(lseek(fd, row * fix.line_length, SEEK_SET) ==
		       (off_t)(row * fix.line_length));
		assert(read(fd, readback, sizeof(readback)) == sizeof(readback));
		for (unsigned int col = 0; col < BLOCK_SIZE; col++)
			assert(readback[col] == pixels[col]);
	}
	assert(close(fd) == 0);
	printf("fb0: %ux%u bpp=%u stride=%u\n", var.xres, var.yres,
	       var.bits_per_pixel, fix.line_length);
	return 0;
}
