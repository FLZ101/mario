#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

static int gui_clip_rect(const gui_surface *surface, gui_rect rect,
			 int *clipped_x, int *clipped_y, int *width, int *height)
{
	int64_t x0 = rect.x;
	int64_t y0 = rect.y;
	int64_t x1 = x0 + rect.w;
	int64_t y1 = y0 + rect.h;

	if (surface == NULL || surface->pixels == NULL || rect.w <= 0 ||
	    rect.h <= 0)
		return 0;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > surface->width)
		x1 = surface->width;
	if (y1 > surface->height)
		y1 = surface->height;
	if (x0 >= x1 || y0 >= y1)
		return 0;
	*clipped_x = (int)x0;
	*clipped_y = (int)y0;
	*width = (int)(x1 - x0);
	*height = (int)(y1 - y0);
	return 1;
}

void gui_fill_rect(gui_surface *surface, gui_rect rect, gui_color color)
{
	int height;
	int width;
	int clipped_x;
	int clipped_y;

	if (!gui_clip_rect(surface, rect, &clipped_x, &clipped_y, &width, &height))
		return;
	for (int row = 0; row < height; row++) {
		gui_color *pixels = surface->pixels +
			(clipped_y + row) * surface->stride + clipped_x;

		for (int column = 0; column < width; column++)
			pixels[column] = color;
	}
}

void gui_blit(gui_surface *destination, int destination_x, int destination_y,
		      const gui_surface *source, const gui_rect *source_rect)
{
	gui_rect rect;
	int64_t clipped_destination_x = destination_x;
	int64_t clipped_destination_y = destination_y;
	int64_t source_x;
	int64_t source_y;
	int64_t width;
	int64_t height;
	int row;
	int row_end;
	int row_step;

	if (destination == NULL || source == NULL || source_rect == NULL ||
	    destination->pixels == NULL || source->pixels == NULL)
		return;
	rect = *source_rect;
	if (rect.w <= 0 || rect.h <= 0)
		return;
	source_x = rect.x;
	source_y = rect.y;
	width = rect.w;
	height = rect.h;

	if (source_x < 0) {
		width += source_x;
		clipped_destination_x -= source_x;
		source_x = 0;
	}
	if (source_y < 0) {
		height += source_y;
		clipped_destination_y -= source_y;
		source_y = 0;
	}
	if (clipped_destination_x < 0) {
		width += clipped_destination_x;
		source_x -= clipped_destination_x;
		clipped_destination_x = 0;
	}
	if (clipped_destination_y < 0) {
		height += clipped_destination_y;
		source_y -= clipped_destination_y;
		clipped_destination_y = 0;
	}
	if (source_x + width > source->width)
		width = source->width - source_x;
	if (source_y + height > source->height)
		height = source->height - source_y;
	if (clipped_destination_x + width > destination->width)
		width = destination->width - clipped_destination_x;
	if (clipped_destination_y + height > destination->height)
		height = destination->height - clipped_destination_y;
	if (width <= 0 || height <= 0)
		return;

	row = 0;
	row_end = (int)height;
	row_step = 1;
	/* Copy bottom-up when one surface is shifted downward onto itself. */
	if (source == destination && clipped_destination_y > source_y) {
		row = (int)height - 1;
		row_end = -1;
		row_step = -1;
	}
	for (; row != row_end; row += row_step) {
		gui_color *destination_pixels = destination->pixels +
			(clipped_destination_y + row) * destination->stride +
			clipped_destination_x;
		const gui_color *source_pixels = source->pixels +
			(source_y + row) * source->stride + source_x;

		memmove(destination_pixels, source_pixels,
			(size_t)width * sizeof(*destination_pixels));
	}
}
