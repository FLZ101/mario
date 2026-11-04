#ifndef _VIDEO_FB_H
#define _VIDEO_FB_H

#include <types.h>
#include <uapi/mario/fb.h>

struct multiboot_info;

#define FB_VIRT_BASE 0xf0400000UL

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

void fb_early_init(struct multiboot_info *m);
void fb_map(void);
const struct fb_info *fb_get_info(void);
int fb_is_enabled(void);

#endif
