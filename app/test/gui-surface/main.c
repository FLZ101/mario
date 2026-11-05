#include <assert.h>
#include <limits.h>
#include <stdio.h>

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

	puts("gui-surface: ok");
	puts("gui-surface: drawing ok");
	puts("gui-surface: damage ok");
	return 0;
}
