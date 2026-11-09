#ifndef GUI_IMAGE_H
#define GUI_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#define GUI_IMAGE_MAX_DIMENSION 4096U
#define GUI_IMAGE_MAX_PIXELS (16U * 1024U * 1024U)

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

#endif
