#ifndef GUI_IMAGE_H
#define GUI_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#include <gui/surface.h>

#define GUI_IMAGE_MAX_DIMENSION 4096U
#define GUI_IMAGE_MAX_PIXELS (16U * 1024U * 1024U)
/* A BMP header may contain an arbitrary gap before pixels, so bound input too. */
#define GUI_IMAGE_MAX_FILE_BYTES (64U * 1024U * 1024U)

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t row_bytes;
	uint32_t pixel_offset;
	uint32_t pixel_bytes;
	int top_down;
} gui_bmp_metadata;

/* Validate a 24-bit BI_RGB BMP without allocating or decoding its pixels. */
int gui_bmp_metadata_parse(const void *bytes, size_t byte_count,
			   gui_bmp_metadata *metadata);

/* Decode a validated 24-bit BI_RGB BMP into a newly allocated XRGB8888 surface. */
int gui_bmp_decode(const void *bytes, size_t byte_count, gui_surface **surface);

/* Decode one bounded, non-interlaced 8-bit RGB or RGBA PNG frame. */
int gui_png_decode(const void *bytes, size_t byte_count, gui_surface **surface);

#endif
