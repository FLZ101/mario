#ifndef GUI_SURFACE_PRIVATE_H
#define GUI_SURFACE_PRIVATE_H

#include <gui/surface.h>

int gui_surface_clip_rect(const gui_surface *surface, gui_rect rect,
			  int *clipped_x, int *clipped_y, int *width, int *height);

#endif
