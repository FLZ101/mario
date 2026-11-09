#include <errno.h>
#include <limits.h>
#include <stdint.h>

#include <gui/image.h>

#define BMP_FILE_HEADER_BYTES 14U
#define BMP_INFO_HEADER_BYTES 40U
#define BMP_PIXEL_BYTES 3U

static uint16_t gui_image_get16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static uint32_t gui_image_get32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
		(uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

int gui_bmp_metadata_parse(const void *data, size_t byte_count,
			   gui_bmp_metadata *metadata)
{
	const uint8_t *bytes = data;
	uint32_t file_bytes;
	uint32_t pixel_offset;
	uint32_t dib_bytes;
	int32_t signed_width;
	int32_t signed_height;
	uint32_t width;
	uint32_t height;
	uint64_t row_bytes;
	uint64_t pixel_bytes;

	if (bytes == NULL || metadata == NULL ||
	    byte_count < BMP_FILE_HEADER_BYTES + BMP_INFO_HEADER_BYTES ||
	    byte_count > GUI_IMAGE_MAX_FILE_BYTES ||
	    bytes[0] != 'B' || bytes[1] != 'M')
		goto invalid;
	file_bytes = gui_image_get32(bytes + 2);
	pixel_offset = gui_image_get32(bytes + 10);
	dib_bytes = gui_image_get32(bytes + BMP_FILE_HEADER_BYTES);
	signed_width = (int32_t)gui_image_get32(bytes + BMP_FILE_HEADER_BYTES + 4);
	signed_height = (int32_t)gui_image_get32(bytes + BMP_FILE_HEADER_BYTES + 8);
	if (file_bytes != byte_count || dib_bytes < BMP_INFO_HEADER_BYTES ||
	    pixel_offset < BMP_FILE_HEADER_BYTES + dib_bytes || pixel_offset > file_bytes ||
	    signed_width <= 0 || signed_height == 0 || signed_height == INT32_MIN ||
	    gui_image_get16(bytes + BMP_FILE_HEADER_BYTES + 12) != 1 ||
	    gui_image_get16(bytes + BMP_FILE_HEADER_BYTES + 14) != 24 ||
	    gui_image_get32(bytes + BMP_FILE_HEADER_BYTES + 16) != 0)
		goto invalid;
	width = (uint32_t)signed_width;
	height = signed_height < 0 ? (uint32_t)-signed_height : (uint32_t)signed_height;
	if (width > GUI_IMAGE_MAX_DIMENSION || height > GUI_IMAGE_MAX_DIMENSION ||
	    (uint64_t)width * height > GUI_IMAGE_MAX_PIXELS)
		goto invalid;
	/* BMP rows are padded to a four-byte boundary independently of pixel width. */
	row_bytes = ((uint64_t)width * BMP_PIXEL_BYTES + 3) & ~3U;
	pixel_bytes = row_bytes * height;
	if (row_bytes > UINT32_MAX || pixel_bytes > UINT32_MAX ||
	    pixel_bytes > file_bytes - pixel_offset)
		goto invalid;
	*metadata = (gui_bmp_metadata) {
		.width = width,
		.height = height,
		.row_bytes = (uint32_t)row_bytes,
		.pixel_offset = pixel_offset,
		.pixel_bytes = (uint32_t)pixel_bytes,
		.top_down = signed_height < 0,
	};
	return 0;
invalid:
	errno = EINVAL;
	return -1;
}

int gui_bmp_decode(const void *data, size_t byte_count, gui_surface **surface)
{
	const uint8_t *bytes = data;
	gui_bmp_metadata metadata;
	gui_surface *decoded;

	if (surface == NULL || gui_bmp_metadata_parse(data, byte_count, &metadata) < 0)
		return -1;
	decoded = gui_surface_create((int)metadata.width, (int)metadata.height);
	if (decoded == NULL)
		return -1;
	for (uint32_t y = 0; y < metadata.height; y++) {
		uint32_t source_y = metadata.top_down ? y : metadata.height - 1 - y;
		const uint8_t *source = bytes + metadata.pixel_offset +
			(size_t)source_y * metadata.row_bytes;
		gui_color *destination = decoded->pixels + (size_t)y * decoded->stride;

		for (uint32_t x = 0; x < metadata.width; x++) {
			const uint8_t *pixel = source + x * BMP_PIXEL_BYTES;

			/* BMP stores BGR triples; GUI surfaces use XRGB8888. */
			destination[x] = (gui_color)pixel[2] << 16 |
				(gui_color)pixel[1] << 8 | pixel[0];
		}
	}
	*surface = decoded;
	return 0;
}
