#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <gui/image_view.h>

static const uint32_t gui_image_view_numerators[GUI_IMAGE_VIEW_ZOOM_COUNT] =
	{ 1, 1, 1, 2, 4 };
static const uint32_t gui_image_view_denominators[GUI_IMAGE_VIEW_ZOOM_COUNT] =
	{ 4, 2, 1, 1, 1 };

void gui_image_view_init(gui_image_view *view)
{
	*view = (gui_image_view) { .mode = GUI_IMAGE_VIEW_FIT,
		.zoom = GUI_IMAGE_VIEW_ZOOM_ONE };
}

void gui_image_view_fit(gui_image_view *view)
{
	view->mode = GUI_IMAGE_VIEW_FIT;
	view->pan_x = 0;
	view->pan_y = 0;
}

int gui_image_view_set_zoom(gui_image_view *view, uint32_t zoom)
{
	if (zoom >= GUI_IMAGE_VIEW_ZOOM_COUNT) {
		errno = EINVAL;
		return -1;
	}
	view->mode = GUI_IMAGE_VIEW_ZOOM;
	view->zoom = zoom;
	view->pan_x = 0;
	view->pan_y = 0;
	return 0;
}

int gui_image_view_zoom(gui_image_view *view, int direction)
{
	uint32_t zoom = view->mode == GUI_IMAGE_VIEW_FIT ? GUI_IMAGE_VIEW_ZOOM_ONE :
		view->zoom;

	if (direction > 0) {
		if (zoom == GUI_IMAGE_VIEW_ZOOM_QUADRUPLE)
			return 0;
		zoom++;
	} else if (direction < 0) {
		if (zoom == GUI_IMAGE_VIEW_ZOOM_QUARTER)
			return 0;
		zoom--;
	} else
		return 0;
	view->mode = GUI_IMAGE_VIEW_ZOOM;
	view->zoom = zoom;
	view->pan_x = 0;
	view->pan_y = 0;
	return 1;
}

static uint32_t gui_image_view_scaled(uint32_t length, uint32_t numerator,
				      uint32_t denominator)
{
	uint32_t scaled = (uint64_t)length * numerator / denominator;

	/* A nonempty image remains visible at the smallest supported zoom level. */
	return scaled == 0 ? 1 : scaled;
}

static uint32_t gui_image_view_clamp_origin(uint32_t center, uint32_t maximum,
					     int32_t pan)
{
	int64_t origin = (int64_t)center + pan;

	if (origin < 0)
		return 0;
	if ((uint64_t)origin > maximum)
		return maximum;
	return (uint32_t)origin;
}

int gui_image_view_prepare_layout(const gui_image_view *view, uint32_t image_width,
				  uint32_t image_height, uint32_t viewport_width,
				  uint32_t viewport_height, gui_image_view_layout *layout)
{
	uint32_t numerator;
	uint32_t denominator;
	uint32_t scaled_width;
	uint32_t scaled_height;

	if (view == NULL || layout == NULL || image_width == 0 || image_height == 0 ||
	    viewport_width == 0 || viewport_height == 0 ||
	    (view->mode != GUI_IMAGE_VIEW_FIT && view->mode != GUI_IMAGE_VIEW_ZOOM) ||
	    view->zoom >= GUI_IMAGE_VIEW_ZOOM_COUNT) {
		errno = EINVAL;
		return -1;
	}
	if (view->mode == GUI_IMAGE_VIEW_FIT) {
		if ((uint64_t)viewport_width * image_height <=
		    (uint64_t)viewport_height * image_width) {
			numerator = viewport_width;
			denominator = image_width;
		} else {
			numerator = viewport_height;
			denominator = image_height;
		}
	} else {
		numerator = gui_image_view_numerators[view->zoom];
		denominator = gui_image_view_denominators[view->zoom];
	}
	scaled_width = gui_image_view_scaled(image_width, numerator, denominator);
	scaled_height = gui_image_view_scaled(image_height, numerator, denominator);
	*layout = (gui_image_view_layout) {
		.destination_x = scaled_width < viewport_width ?
			(viewport_width - scaled_width) / 2 : 0,
		.destination_y = scaled_height < viewport_height ?
			(viewport_height - scaled_height) / 2 : 0,
		.width = scaled_width < viewport_width ? scaled_width : viewport_width,
		.height = scaled_height < viewport_height ? scaled_height : viewport_height,
		.scaled_width = scaled_width,
		.scaled_height = scaled_height,
		.scaled_x = gui_image_view_clamp_origin(
			scaled_width > viewport_width ? (scaled_width - viewport_width) / 2 : 0,
			scaled_width > viewport_width ? scaled_width - viewport_width : 0,
			view->pan_x),
		.scaled_y = gui_image_view_clamp_origin(
			scaled_height > viewport_height ? (scaled_height - viewport_height) / 2 : 0,
			scaled_height > viewport_height ? scaled_height - viewport_height : 0,
			view->pan_y),
		.scale_numerator = numerator,
		.scale_denominator = denominator,
	};
	return 0;
}

int gui_image_view_pan(gui_image_view *view, uint32_t image_width,
		       uint32_t image_height, uint32_t viewport_width,
		       uint32_t viewport_height, int32_t delta_x, int32_t delta_y)
{
	gui_image_view_layout layout;
	uint32_t center_x;
	uint32_t center_y;
	uint32_t maximum_x;
	uint32_t maximum_y;
	uint32_t origin_x;
	uint32_t origin_y;

	if (gui_image_view_prepare_layout(view, image_width, image_height, viewport_width,
		viewport_height, &layout) < 0)
		return -1;
	maximum_x = layout.scaled_width > viewport_width ? layout.scaled_width - viewport_width : 0;
	maximum_y = layout.scaled_height > viewport_height ? layout.scaled_height - viewport_height : 0;
	center_x = maximum_x / 2;
	center_y = maximum_y / 2;
	origin_x = gui_image_view_clamp_origin(layout.scaled_x, maximum_x, delta_x);
	origin_y = gui_image_view_clamp_origin(layout.scaled_y, maximum_y, delta_y);
	if (origin_x == layout.scaled_x && origin_y == layout.scaled_y)
		return 0;
	view->pan_x = (int32_t)origin_x - (int32_t)center_x;
	view->pan_y = (int32_t)origin_y - (int32_t)center_y;
	return 1;
}
