#ifndef GUI_IMAGE_VIEW_H
#define GUI_IMAGE_VIEW_H

#include <stdint.h>

typedef enum {
	GUI_IMAGE_VIEW_FIT,
	GUI_IMAGE_VIEW_ZOOM,
} gui_image_view_mode;

enum {
	GUI_IMAGE_VIEW_ZOOM_QUARTER,
	GUI_IMAGE_VIEW_ZOOM_HALF,
	GUI_IMAGE_VIEW_ZOOM_ONE,
	GUI_IMAGE_VIEW_ZOOM_DOUBLE,
	GUI_IMAGE_VIEW_ZOOM_QUADRUPLE,
	GUI_IMAGE_VIEW_ZOOM_COUNT,
};

typedef struct {
	gui_image_view_mode mode;
	uint32_t zoom;
	int32_t pan_x;
	int32_t pan_y;
} gui_image_view;

/* The visible region expressed in scaled-image and destination coordinates. */
typedef struct {
	uint32_t destination_x;
	uint32_t destination_y;
	uint32_t width;
	uint32_t height;
	uint32_t scaled_width;
	uint32_t scaled_height;
	uint32_t scaled_x;
	uint32_t scaled_y;
	uint32_t scale_numerator;
	uint32_t scale_denominator;
} gui_image_view_layout;

void gui_image_view_init(gui_image_view *view);
void gui_image_view_fit(gui_image_view *view);
int gui_image_view_set_zoom(gui_image_view *view, uint32_t zoom);
/* Return one when the bounded zoom level changed and zero at either limit. */
int gui_image_view_zoom(gui_image_view *view, int direction);
/* Move the clipped source viewport by scaled-image pixels and clamp it. */
int gui_image_view_pan(gui_image_view *view, uint32_t image_width,
		       uint32_t image_height, uint32_t viewport_width,
		       uint32_t viewport_height, int32_t delta_x, int32_t delta_y);
int gui_image_view_prepare_layout(const gui_image_view *view, uint32_t image_width,
				  uint32_t image_height, uint32_t viewport_width,
				  uint32_t viewport_height, gui_image_view_layout *layout);

#endif
