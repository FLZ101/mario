#ifndef GUI_SURFACE_H
#define GUI_SURFACE_H

#include <stdint.h>

typedef uint32_t gui_color;

typedef struct {
	int x;
	int y;
	int w;
	int h;
} gui_rect;

typedef struct {
	int width;
	int height;
	int stride;
	gui_color *pixels;
} gui_surface;

gui_surface *gui_surface_create(int width, int height);
void gui_surface_destroy(gui_surface *surface);
void gui_fill_rect(gui_surface *surface, gui_rect rect, gui_color color);
void gui_blit(gui_surface *destination, int destination_x, int destination_y,
	      const gui_surface *source, const gui_rect *source_rect);

#endif
