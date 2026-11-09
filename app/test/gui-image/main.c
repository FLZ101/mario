#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <gui/image.h>

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

int main(void)
{
	test_valid();
	test_invalid();
	puts("gui-image: BMP metadata ok");
	return 0;
}
