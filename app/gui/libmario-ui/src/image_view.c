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
}

int gui_image_view_set_zoom(gui_image_view *view, uint32_t zoom)
{
	if (zoom >= GUI_IMAGE_VIEW_ZOOM_COUNT) {
		errno = EINVAL;
		return -1;
	}
	view->mode = GUI_IMAGE_VIEW_ZOOM;
	view->zoom = zoom;
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
	return 1;
}

static uint32_t gui_image_view_scaled(uint32_t length, uint32_t numerator,
				      uint32_t denominator)
{
	uint32_t scaled = (uint64_t)length * numerator / denominator;

	/* A nonempty image remains visible at the smallest supported zoom level. */
	return scaled == 0 ? 1 : scaled;
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
		/* Center clipping until the later pan controls adjust this origin. */
		.scaled_x = scaled_width > viewport_width ?
			(scaled_width - viewport_width) / 2 : 0,
		.scaled_y = scaled_height > viewport_height ?
			(scaled_height - viewport_height) / 2 : 0,
		.scale_numerator = numerator,
		.scale_denominator = denominator,
	};
	return 0;
}
