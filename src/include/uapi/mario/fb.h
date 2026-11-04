#ifndef _UAPI_MARIO_FB_H
#define _UAPI_MARIO_FB_H

/*
 * Stable userspace ABI for /dev/fb0.  These definitions intentionally describe
 * Mario's fixed-width ioctl payloads; they are not declared Linux-compatible.
 */
#ifdef __KERNEL__
#include <types.h>
#else
#include <stdint.h>
#endif

struct fb_bitfield {
	uint32_t offset;
	uint32_t length;
};

/* Returned by FBIOGET_VSCREENINFO. */
struct fb_var_screeninfo {
	uint32_t xres;
	uint32_t yres;
	uint32_t xres_virtual;
	uint32_t yres_virtual;
	uint32_t xoffset;
	uint32_t yoffset;
	uint32_t bits_per_pixel;
	uint32_t grayscale;
	struct fb_bitfield red;
	struct fb_bitfield green;
	struct fb_bitfield blue;
	struct fb_bitfield transp;
	uint32_t nonstd;
	uint32_t activate;
	uint32_t height;
	uint32_t width;
	uint32_t accel_flags;
	uint32_t pixclock;
	uint32_t left_margin;
	uint32_t right_margin;
	uint32_t upper_margin;
	uint32_t lower_margin;
	uint32_t hsync_len;
	uint32_t vsync_len;
	uint32_t sync;
	uint32_t vmode;
	uint32_t rotate;
	uint32_t colorspace;
	uint32_t reserved[4];
};

/* Returned by FBIOGET_FSCREENINFO. */
struct fb_fix_screeninfo {
	char id[16];
	uint64_t smem_start;
	uint32_t smem_len;
	uint32_t type;
	uint32_t type_aux;
	uint32_t visual;
	uint16_t xpanstep;
	uint16_t ypanstep;
	uint16_t ywrapstep;
	uint32_t line_length;
	uint32_t mmio_start;
	uint32_t mmio_len;
	uint32_t accel;
	uint16_t capabilities;
	uint16_t reserved[2];
};

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

/* Catch ABI drift in both kernel and userspace builds. */
_Static_assert(sizeof(struct fb_bitfield) == 8, "fb_bitfield ABI size");
_Static_assert(sizeof(struct fb_var_screeninfo) == 144,
	       "fb_var_screeninfo ABI size");
_Static_assert(sizeof(struct fb_fix_screeninfo) == 72,
	       "fb_fix_screeninfo ABI size");

#endif /* _UAPI_MARIO_FB_H */
