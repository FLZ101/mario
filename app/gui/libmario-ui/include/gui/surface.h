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

#endif
