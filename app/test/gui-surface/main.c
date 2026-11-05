#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include <gui/surface.h>

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

	puts("gui-surface: ok");
	return 0;
}
