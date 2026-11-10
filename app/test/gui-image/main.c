#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gui/image.h>
#include <gui/image_cache.h>
#include <gui/image_view.h>

static void put16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
}

static void put32(uint8_t *bytes, uint32_t value)
{
	for (unsigned int index = 0; index < 4; index++)
		bytes[index] = value >> (index * 8);
}

static void put_be32(uint8_t *bytes, uint32_t value)
{
	for (unsigned int index = 0; index < 4; index++)
		bytes[index] = value >> ((3 - index) * 8);
}

static void bmp_header(uint8_t bytes[70], uint32_t width, int32_t height)
{
	memset(bytes, 0, 70);
	bytes[0] = 'B';
	bytes[1] = 'M';
	put32(bytes + 2, 70);
	put32(bytes + 10, 54);
	put32(bytes + 14, 40);
	put32(bytes + 18, width);
	put32(bytes + 22, (uint32_t)height);
	put16(bytes + 26, 1);
	put16(bytes + 28, 24);
}

static void test_valid(void)
{
	uint8_t bytes[70];
	gui_bmp_metadata metadata;

	bmp_header(bytes, 2, -2);
	assert(gui_bmp_metadata_parse(bytes, sizeof(bytes), &metadata) == 0);
	assert(metadata.width == 2 && metadata.height == 2 && metadata.top_down);
	assert(metadata.row_bytes == 8 && metadata.pixel_offset == 54 &&
	       metadata.pixel_bytes == 16);
}

static void test_invalid(void)
{
	uint8_t bytes[70];
	gui_bmp_metadata metadata;

	bmp_header(bytes, 2, 2);
	bytes[0] = 'N';
	assert(gui_bmp_metadata_parse(bytes, sizeof(bytes), &metadata) == -1 && errno == EINVAL);
	bmp_header(bytes, 2, 2);
	put32(bytes + 2, 69);
	assert(gui_bmp_metadata_parse(bytes, sizeof(bytes), &metadata) == -1);
	bmp_header(bytes, 2, 2);
	put32(bytes + 10, 53);
	assert(gui_bmp_metadata_parse(bytes, sizeof(bytes), &metadata) == -1);
	bmp_header(bytes, 2, 2);
	put16(bytes + 28, 32);
	assert(gui_bmp_metadata_parse(bytes, sizeof(bytes), &metadata) == -1);
	bmp_header(bytes, GUI_IMAGE_MAX_DIMENSION + 1, 1);
	assert(gui_bmp_metadata_parse(bytes, sizeof(bytes), &metadata) == -1);
	bmp_header(bytes, 2, 2);
	put32(bytes + 10, 60);
	assert(gui_bmp_metadata_parse(bytes, sizeof(bytes), &metadata) == -1);
	assert(gui_bmp_metadata_parse(bytes, 53, &metadata) == -1);
}

static void test_decode(void)
{
	uint8_t bytes[70];
	gui_surface *surface = NULL;

	bmp_header(bytes, 2, 2);
	/* Positive-height BMP rows are stored bottom first and padded to four bytes. */
	bytes[54] = 0x11; bytes[55] = 0x22; bytes[56] = 0x33;
	bytes[57] = 0x44; bytes[58] = 0x55; bytes[59] = 0x66;
	bytes[62] = 0x77; bytes[63] = 0x88; bytes[64] = 0x99;
	bytes[65] = 0xaa; bytes[66] = 0xbb; bytes[67] = 0xcc;
	assert(gui_bmp_decode(bytes, sizeof(bytes), &surface) == 0);
	assert(surface->width == 2 && surface->height == 2);
	assert(surface->pixels[0] == 0x00998877 && surface->pixels[1] == 0x00ccbbaa);
	assert(surface->pixels[2] == 0x00332211 && surface->pixels[3] == 0x00665544);
	gui_surface_destroy(surface);

	bmp_header(bytes, 2, -2);
	bytes[54] = 0x11; bytes[55] = 0x22; bytes[56] = 0x33;
	bytes[57] = 0x44; bytes[58] = 0x55; bytes[59] = 0x66;
	bytes[62] = 0x77; bytes[63] = 0x88; bytes[64] = 0x99;
	bytes[65] = 0xaa; bytes[66] = 0xbb; bytes[67] = 0xcc;
	assert(gui_bmp_decode(bytes, sizeof(bytes), &surface) == 0);
	assert(surface->pixels[0] == 0x00332211 && surface->pixels[1] == 0x00665544);
	assert(surface->pixels[2] == 0x00998877 && surface->pixels[3] == 0x00ccbbaa);
	gui_surface_destroy(surface);
}

static uint8_t *read_fixture(const char *path, size_t *byte_count)
{
	struct stat status;
	uint8_t *bytes;
	int fd;

	fd = open(path, O_RDONLY);
	assert(fd >= 0 && fstat(fd, &status) == 0 && status.st_size > 0);
	bytes = malloc((size_t)status.st_size);
	assert(bytes != NULL);
	for (size_t offset = 0; offset < (size_t)status.st_size;) {
		ssize_t count = read(fd, bytes + offset, (size_t)status.st_size - offset);

		assert(count > 0);
		offset += (size_t)count;
	}
	assert(close(fd) == 0);
	*byte_count = (size_t)status.st_size;
	return bytes;
}

static void test_png_decode(void)
{
	uint8_t *bytes;
	size_t byte_count;
	gui_surface *surface = NULL;
	gui_surface *previous;

	bytes = read_fixture("/usr/share/mario-tests/gui/alpha-128.png", &byte_count);
	assert(gui_png_decode(bytes, byte_count, &surface) == 0);
	assert(surface->width == 128 && surface->height == 128);
	previous = surface;
	assert(gui_png_decode(bytes, 7, &surface) == -1 && errno == EINVAL);
	assert(surface == previous);
	/* libpng rejects the IHDR limit before a malformed replacement is allocated. */
	put_be32(bytes + 16, GUI_IMAGE_MAX_DIMENSION + 1);
	assert(gui_png_decode(bytes, byte_count, &surface) == -1 && errno == EINVAL);
	assert(surface == previous);
	bytes[0] = 0;
	assert(gui_png_decode(bytes, byte_count, &surface) == -1 && errno == EINVAL);
	assert(surface == previous);
	gui_surface_destroy(surface);
	free(bytes);
}

static gui_surface *cache_surface(gui_color color)
{
	gui_surface *surface = gui_surface_create(1, 1);

	assert(surface != NULL);
	surface->pixels[0] = color;
	return surface;
}

static void test_cache(void)
{
	gui_image_cache cache;
	gui_surface *first = cache_surface(0x00112233);
	gui_surface *second = cache_surface(0x00445566);
	gui_surface *replacement = cache_surface(0x00778899);

	gui_image_cache_init(&cache);
	assert(gui_image_cache_set_current(&cache, "first.bmp", first) == 0);
	assert(gui_image_cache_current(&cache) == first);
	assert(strcmp(gui_image_cache_current_path(&cache), "first.bmp") == 0);
	assert(gui_image_cache_set_next(&cache, "second.png", second) == 0);
	assert(gui_image_cache_next(&cache) == second);
	/* Replacing next deterministically evicts the old preloaded frame. */
	assert(gui_image_cache_set_next(&cache, "replacement.png", replacement) == 0);
	assert(gui_image_cache_next(&cache) == replacement);
	assert(strcmp(gui_image_cache_next_path(&cache), "replacement.png") == 0);
	assert(gui_image_cache_promote_next(&cache) == 0);
	assert(gui_image_cache_current(&cache) == replacement);
	assert(gui_image_cache_next(&cache) == NULL);
	assert(gui_image_cache_set_next(&cache, "replacement.png", replacement) == -1 &&
		errno == EINVAL);
	gui_image_cache_destroy(&cache);
}

static void test_view(void)
{
	gui_image_view view;
	gui_image_view_layout layout;

	gui_image_view_init(&view);
	assert(gui_image_view_prepare_layout(&view, 1600, 1200, 320, 256, &layout) == 0);
	assert(layout.destination_x == 0 && layout.destination_y == 8);
	assert(layout.width == 320 && layout.height == 240);
	assert(layout.scaled_x == 0 && layout.scaled_y == 0);
	assert(layout.scale_numerator == 320 && layout.scale_denominator == 1600);
	assert(gui_image_view_set_zoom(&view, GUI_IMAGE_VIEW_ZOOM_ONE) == 0);
	assert(gui_image_view_prepare_layout(&view, 1600, 1200, 320, 256, &layout) == 0);
	assert(layout.destination_x == 0 && layout.destination_y == 0);
	assert(layout.width == 320 && layout.height == 256);
	assert(layout.scaled_x == 640 && layout.scaled_y == 472);
	assert(layout.scale_numerator == 1 && layout.scale_denominator == 1);
	assert(gui_image_view_pan(&view, 1600, 1200, 320, 256, -1000, 1000) == 1);
	assert(gui_image_view_prepare_layout(&view, 1600, 1200, 320, 256, &layout) == 0);
	assert(layout.scaled_x == 0 && layout.scaled_y == 944);
	assert(gui_image_view_pan(&view, 1600, 1200, 320, 256, 2000, -2000) == 1);
	assert(gui_image_view_prepare_layout(&view, 1600, 1200, 320, 256, &layout) == 0);
	assert(layout.scaled_x == 1280 && layout.scaled_y == 0);
	assert(gui_image_view_pan(&view, 1600, 1200, 320, 256, 1, -1) == 0);
	assert(gui_image_view_zoom(&view, 1) == 1);
	assert(gui_image_view_zoom(&view, 1) == 1);
	assert(gui_image_view_zoom(&view, 1) == 0);
	assert(gui_image_view_prepare_layout(&view, 64, 64, 320, 256, &layout) == 0);
	assert(layout.destination_x == 32 && layout.destination_y == 0);
	assert(layout.width == 256 && layout.height == 256);
	assert(gui_image_view_set_zoom(&view, GUI_IMAGE_VIEW_ZOOM_COUNT) == -1 && errno == EINVAL);
}

int main(void)
{
	test_valid();
	test_invalid();
	test_decode();
	test_png_decode();
	test_cache();
	test_view();
	puts("gui-image: BMP and PNG decode ok");
	return 0;
}
