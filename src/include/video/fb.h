#ifndef _VIDEO_FB_H
#define _VIDEO_FB_H

#include <types.h>

struct multiboot_info;

#define FB_VIRT_BASE 0xf0400000UL

struct fb_bitfield {
	uint32_t offset;
	uint32_t length;
};

struct fb_info {
	uint64_t phys_addr;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint32_t bpp;
	uint32_t size;
	struct fb_bitfield red;
	struct fb_bitfield green;
	struct fb_bitfield blue;
	void *virt_addr;
	int enabled;
};

/* Linux-compatible enough for the M0 raw device ABI. */
struct fb_var_screeninfo {
	uint32_t xres, yres, xres_virtual, yres_virtual;
	uint32_t xoffset, yoffset;
	uint32_t bits_per_pixel, grayscale;
	struct fb_bitfield red, green, blue, transp;
	uint32_t nonstd, activate, height, width, accel_flags;
	uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
	uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
	uint32_t reserved[4];
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

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

void fb_early_init(struct multiboot_info *m);
void fb_map(void);
const struct fb_info *fb_get_info(void);
int fb_is_enabled(void);

#endif
