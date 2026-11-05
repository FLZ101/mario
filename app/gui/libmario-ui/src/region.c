#include <stddef.h>
#include <stdint.h>

#include <gui/surface.h>

static int gui_region_clip(const gui_region *region, gui_rect rect,
			   gui_region_rect *clipped)
{
	int64_t left = rect.x;
	int64_t top = rect.y;
	int64_t right = left + rect.w;
	int64_t bottom = top + rect.h;
	int64_t bounds_right = (int64_t)region->bounds.x + region->bounds.w;
	int64_t bounds_bottom = (int64_t)region->bounds.y + region->bounds.h;

	if (rect.w <= 0 || rect.h <= 0 || region->bounds.w <= 0 ||
	    region->bounds.h <= 0)
		return 0;
	if (left < region->bounds.x)
		left = region->bounds.x;
	if (top < region->bounds.y)
		top = region->bounds.y;
	if (right > bounds_right)
		right = bounds_right;
	if (bottom > bounds_bottom)
		bottom = bounds_bottom;
	if (left >= right || top >= bottom)
		return 0;
	clipped->x = (int)left;
	clipped->y = (int)top;
	clipped->width = (int)(right - left);
	clipped->height = (int)(bottom - top);
	return 1;
}

static int gui_region_touches(gui_region_rect first, gui_region_rect second)
{
	int64_t first_right = (int64_t)first.x + first.width;
	int64_t first_bottom = (int64_t)first.y + first.height;
	int64_t second_right = (int64_t)second.x + second.width;
	int64_t second_bottom = (int64_t)second.y + second.height;

	return first_right >= second.x && second_right >= first.x &&
		first_bottom >= second.y && second_bottom >= first.y;
}

static gui_region_rect gui_region_union(gui_region_rect first,
					 gui_region_rect second)
{
	int64_t left = first.x < second.x ? first.x : second.x;
	int64_t top = first.y < second.y ? first.y : second.y;
	int64_t first_right = (int64_t)first.x + first.width;
	int64_t first_bottom = (int64_t)first.y + first.height;
	int64_t second_right = (int64_t)second.x + second.width;
	int64_t second_bottom = (int64_t)second.y + second.height;
	int64_t right = first_right > second_right ? first_right : second_right;
	int64_t bottom = first_bottom > second_bottom ? first_bottom : second_bottom;

	return (gui_region_rect) {
		.x = (int)left,
		.y = (int)top,
		.width = (int)(right - left),
		.height = (int)(bottom - top),
	};
}

void gui_region_init(gui_region *region, gui_region_rect *rects, int capacity,
		     gui_rect bounds)
{
	region->count = 0;
	region->capacity = capacity > GUI_REGION_MAX_RECTS ? GUI_REGION_MAX_RECTS :
		capacity;
	region->rects = rects;
	region->bounds = bounds;
	region->full = 0;
}

void gui_region_clear(gui_region *region)
{
	region->count = 0;
	region->full = 0;
}

void gui_region_mark_full(gui_region *region)
{
	if (region == NULL || region->rects == NULL || region->capacity <= 0 ||
	    region->bounds.w <= 0 || region->bounds.h <= 0)
		return;
	/* A foreground transition invalidates every previously completed row. */
	region->rects[0] = (gui_region_rect) {
		.x = region->bounds.x,
		.y = region->bounds.y,
		.width = region->bounds.w,
		.height = region->bounds.h,
	};
	region->count = 1;
	region->full = 1;
}

void gui_region_add(gui_region *region, gui_rect rect)
{
	gui_region_rect candidate;
	int index;

	if (region == NULL || region->rects == NULL || region->capacity <= 0 ||
	    region->full || !gui_region_clip(region, rect, &candidate))
		return;
	for (index = 0; index < region->count;) {
		if (!gui_region_touches(candidate, region->rects[index])) {
			index++;
			continue;
		}
		candidate = gui_region_union(candidate, region->rects[index]);
		region->count--;
		region->rects[index] = region->rects[region->count];
	}
	if (region->count == region->capacity) {
		gui_region_mark_full(region);
		return;
	}
	region->rects[region->count++] = candidate;
}
