#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <gui/surface.h>

static int gui_surface_bytes(int width, int height, size_t *bytes)
{
	size_t pixels;

	if (width <= 0 || height <= 0)
		return 0;
	pixels = (size_t)width;
	if (pixels > SIZE_MAX / (size_t)height)
		return 0;
	pixels *= (size_t)height;
	if (pixels > SIZE_MAX / sizeof(gui_color))
		return 0;
	*bytes = pixels * sizeof(gui_color);
	return 1;
}

gui_surface *gui_surface_create(int width, int height)
{
	gui_surface *surface;
	size_t bytes;

	if (!gui_surface_bytes(width, height, &bytes))
		return NULL;
	surface = malloc(sizeof(*surface));
	if (surface == NULL)
		return NULL;
	/* New XRGB8888 pixels start black and are owned by this surface. */
	surface->pixels = calloc(1, bytes);
	if (surface->pixels == NULL) {
		free(surface);
		return NULL;
	}
	surface->width = width;
	surface->height = height;
	surface->stride = width;
	return surface;
}

void gui_surface_destroy(gui_surface *surface)
{
	if (surface == NULL)
		return;
	free(surface->pixels);
	free(surface);
}
