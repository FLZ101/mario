#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gui/surface.h>

static void test_fill_clipping(gui_surface *surface)
{
	gui_fill_rect(surface, (gui_rect) { -1, -1, 3, 3 }, 1);
	gui_fill_rect(surface, (gui_rect) { 3, 1, 3, 2 }, 2);
	gui_fill_rect(surface, (gui_rect) { 1, 3, 2, 3 }, 3);
	gui_fill_rect(surface, (gui_rect) { -2, 2, 3, 1 }, 4);
	gui_fill_rect(surface, (gui_rect) { 1, 1, 0, 1 }, 5);

	assert(surface->pixels[0] == 1);
	assert(surface->pixels[1] == 1);
	assert(surface->pixels[4] == 1);
	assert(surface->pixels[5] == 1);
	assert(surface->pixels[6] == 0);
	assert(surface->pixels[7] == 2);
	assert(surface->pixels[11] == 2);
	assert(surface->pixels[8] == 4);
	assert(surface->pixels[13] == 3);
	assert(surface->pixels[14] == 3);
}

static void test_blit_overlap(gui_surface *surface)
{
	for (int column = 0; column < 4; column++)
		surface->pixels[column] = (gui_color)(column + 1);
	gui_blit(surface, 1, 0, surface, &(gui_rect) { 0, 0, 3, 1 });
	assert(surface->pixels[0] == 1);
	assert(surface->pixels[1] == 1);
	assert(surface->pixels[2] == 2);
	assert(surface->pixels[3] == 3);

	for (int row = 0; row < 4; row++)
		for (int column = 0; column < 4; column++)
			surface->pixels[row * surface->stride + column] =
				(gui_color)(row * 10 + column);
	gui_blit(surface, 0, 1, surface, &(gui_rect) { 0, 0, 4, 3 });
	for (int column = 0; column < 4; column++) {
		assert(surface->pixels[4 + column] == (gui_color)column);
		assert(surface->pixels[8 + column] == (gui_color)(10 + column));
		assert(surface->pixels[12 + column] == (gui_color)(20 + column));
	}
}

static void test_damage_region(void)
{
	gui_region_rect rects[GUI_REGION_MAX_RECTS];
	gui_region region;

	gui_region_init(&region, rects, GUI_REGION_MAX_RECTS,
			(gui_rect) { 0, 0, 1000, 10 });
	gui_region_add(&region, (gui_rect) { 1, 1, 2, 2 });
	gui_region_add(&region, (gui_rect) { 3, 1, 2, 2 });
	gui_region_add(&region, (gui_rect) { 2, 1, 1, 1 });
	assert(region.count == 1);
	assert(region.rects[0].x == 1 && region.rects[0].y == 1);
	assert(region.rects[0].width == 4 && region.rects[0].height == 2);

	gui_region_clear(&region);
	for (int index = 0; index < GUI_REGION_MAX_RECTS; index++)
		gui_region_add(&region, (gui_rect) { index * 2, 0, 1, 1 });
	assert(region.count == GUI_REGION_MAX_RECTS);
	gui_region_add(&region, (gui_rect) { 200, 0, 1, 1 });
	assert(region.count == 1 && region.full);
	assert(region.rects[0].x == 0 && region.rects[0].y == 0);
	assert(region.rects[0].width == 1000 && region.rects[0].height == 10);
}

typedef struct {
	ssize_t results[8];
	int errors[8];
	off_t offsets[8];
	size_t counts[8];
	unsigned char bytes[128];
	int count;
} mock_sink;

static ssize_t mock_pwrite64(void *context, const void *buffer, size_t bytes,
			     off_t offset)
{
	mock_sink *sink = context;
	ssize_t result;
	int index = sink->count++;

	assert(index < (int)(sizeof(sink->results) / sizeof(sink->results[0])));
	sink->offsets[index] = offset;
	sink->counts[index] = bytes;
	result = sink->results[index];
	if (result == -1) {
		errno = sink->errors[index];
		return -1;
	}
	assert(result >= 0 && (size_t)result <= bytes);
	assert(offset >= 0 && (size_t)offset + (size_t)result <= sizeof(sink->bytes));
	memcpy(sink->bytes + offset, buffer, (size_t)result);
	return result;
}

static void test_flush_rect(void)
{
	gui_surface *surface = gui_surface_create(4, 3);
	mock_sink sink = {
		.results = { 3, 9, 5, 7 },
	};
	gui_rect rect = { -1, 1, 4, 2 };
	int fd;

	assert(surface != NULL);
	for (int index = 0; index < surface->width * surface->height; index++)
		surface->pixels[index] = (gui_color)(0x01020300 + index);
	assert(gui_surface_flush_rect(surface, rect, 24, mock_pwrite64, &sink) ==
	       GUI_FLUSH_COMPLETE);
	assert(sink.count == 4);
	assert(sink.offsets[0] == 24 && sink.counts[0] == 12);
	assert(sink.offsets[1] == 27 && sink.counts[1] == 9);
	assert(sink.offsets[2] == 48 && sink.counts[2] == 12);
	assert(sink.offsets[3] == 53 && sink.counts[3] == 7);
	assert(!memcmp(sink.bytes + 24, surface->pixels + 4, 12));
	assert(!memcmp(sink.bytes + 48, surface->pixels + 8, 12));

	memset(&sink, 0, sizeof(sink));
	sink.results[0] = -1;
	sink.errors[0] = EINTR;
	sink.results[1] = 8;
	assert(gui_surface_flush_rect(surface, (gui_rect) { 0, 0, 2, 1 }, 16,
				      mock_pwrite64, &sink) == GUI_FLUSH_COMPLETE);
	assert(sink.count == 2);
	assert(sink.offsets[0] == 0 && sink.counts[0] == 8);
	assert(sink.offsets[1] == 0 && sink.counts[1] == 8);

	memset(&sink, 0, sizeof(sink));
	sink.results[0] = 4;
	sink.results[1] = -1;
	sink.errors[1] = EBUSY;
	assert(gui_surface_flush_rect(surface, rect, 24, mock_pwrite64, &sink) ==
	       GUI_FLUSH_BUSY);
	assert(sink.count == 2);

	fd = open("/tmp/gui-surface-flush", O_CREAT | O_TRUNC | O_RDWR, 0600);
	assert(fd >= 0);
	assert(gui_surface_flush_rect_fd(surface, (gui_rect) { 0, 0, 2, 1 }, 16,
					 fd) == GUI_FLUSH_COMPLETE);
	assert(pread(fd, sink.bytes, 8, 0) == 8);
	assert(!memcmp(sink.bytes, surface->pixels, 8));
	assert(close(fd) == 0);
	assert(unlink("/tmp/gui-surface-flush") == 0);
	gui_surface_destroy(surface);
}

int main(void)
{
	gui_surface *surface;

	assert(gui_surface_create(0, 1) == NULL);
	assert(gui_surface_create(1, 0) == NULL);
	assert(gui_surface_create(-1, 1) == NULL);
	assert(gui_surface_create(INT_MAX, INT_MAX) == NULL);
	assert(gui_surface_create(65536, 65536) == NULL);

	surface = gui_surface_create(3, 2);
	assert(surface != NULL);
	assert(surface->width == 3);
	assert(surface->height == 2);
	assert(surface->stride == 3);
	for (int index = 0; index < surface->stride * surface->height; index++)
		assert(surface->pixels[index] == 0);
	gui_surface_destroy(surface);
	gui_surface_destroy(NULL);

	surface = gui_surface_create(4, 4);
	assert(surface != NULL);
	test_fill_clipping(surface);
	test_blit_overlap(surface);
	gui_surface_destroy(surface);
	test_damage_region();
	test_flush_rect();

	puts("gui-surface: ok");
	puts("gui-surface: drawing ok");
	puts("gui-surface: damage ok");
	puts("gui-surface: flush ok");
	return 0;
}
