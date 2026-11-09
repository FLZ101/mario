#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <gui/image.h>

#include <png.h>

#define BMP_FILE_HEADER_BYTES 14U
#define BMP_INFO_HEADER_BYTES 40U
#define BMP_PIXEL_BYTES 3U
#define PNG_SIGNATURE_BYTES 8U

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

typedef struct {
	const uint8_t *bytes;
	size_t byte_count;
	size_t offset;
} gui_png_reader;

static void gui_png_read(png_structp png, png_bytep destination, png_size_t byte_count)
{
	gui_png_reader *reader = png_get_io_ptr(png);

	if (reader == NULL || byte_count > reader->byte_count - reader->offset)
		png_error(png, "truncated PNG input");
	memcpy(destination, reader->bytes + reader->offset, byte_count);
	reader->offset += byte_count;
}

static gui_color gui_png_color(const uint8_t pixel[4])
{
	uint32_t alpha = pixel[3];

	/* GUI surfaces are opaque; premultiply straight PNG alpha against black. */
	return ((uint32_t)pixel[0] * alpha + 127) / 255 << 16 |
		((uint32_t)pixel[1] * alpha + 127) / 255 << 8 |
		((uint32_t)pixel[2] * alpha + 127) / 255;
}

int gui_png_decode(const void *data, size_t byte_count, gui_surface **surface)
{
	const uint8_t *bytes = data;
	gui_png_reader reader;
	png_structp png = NULL;
	png_infop info = NULL;
	gui_surface *decoded = NULL;
	png_bytep row = NULL;
	png_uint_32 width;
	png_uint_32 height;
	int bit_depth;
	int color_type;
	int interlace;
	int compression;
	int filter;

	if (bytes == NULL || surface == NULL || byte_count < PNG_SIGNATURE_BYTES ||
	    byte_count > GUI_IMAGE_MAX_FILE_BYTES ||
	    png_sig_cmp((png_bytep)bytes, 0, PNG_SIGNATURE_BYTES) != 0)
		goto invalid;
	reader = (gui_png_reader) { .bytes = bytes, .byte_count = byte_count,
		.offset = PNG_SIGNATURE_BYTES };
	png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (png == NULL)
		goto no_memory;
	info = png_create_info_struct(png);
	if (info == NULL)
		goto no_memory;
	if (setjmp(png_jmpbuf(png)) != 0)
		goto invalid;
	/* Reject oversized IHDR dimensions before libpng allocates image rows. */
	png_set_user_limits(png, GUI_IMAGE_MAX_DIMENSION, GUI_IMAGE_MAX_DIMENSION);
	png_set_read_fn(png, &reader, gui_png_read);
	png_set_sig_bytes(png, PNG_SIGNATURE_BYTES);
	png_read_info(png, info);
	png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type,
		&interlace, &compression, &filter);
	if (width == 0 || height == 0 || (uint64_t)width * height > GUI_IMAGE_MAX_PIXELS ||
	    bit_depth != 8 || (color_type != PNG_COLOR_TYPE_RGB &&
		color_type != PNG_COLOR_TYPE_RGBA) || interlace != PNG_INTERLACE_NONE)
		goto invalid;
	if (color_type == PNG_COLOR_TYPE_RGB)
		png_set_filler(png, 0xff, PNG_FILLER_AFTER);
	png_read_update_info(png, info);
	if (png_get_rowbytes(png, info) != (size_t)width * 4)
		goto invalid;
	decoded = gui_surface_create((int)width, (int)height);
	if (decoded == NULL)
		goto no_memory;
	row = malloc((size_t)width * 4);
	if (row == NULL)
		goto no_memory;
	for (png_uint_32 y = 0; y < height; y++) {
		gui_color *destination = decoded->pixels + (size_t)y * decoded->stride;

		png_read_row(png, row, NULL);
		for (png_uint_32 x = 0; x < width; x++)
			destination[x] = gui_png_color(row + x * 4);
	}
	png_read_end(png, info);
	free(row);
	png_destroy_read_struct(&png, &info, NULL);
	*surface = decoded;
	return 0;
no_memory:
	errno = ENOMEM;
	goto out;
invalid:
	errno = EINVAL;
out:
	free(row);
	gui_surface_destroy(decoded);
	png_destroy_read_struct(&png, &info, NULL);
	return -1;
}
