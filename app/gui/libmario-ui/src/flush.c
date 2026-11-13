#define _LARGEFILE64_SOURCE

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <gui/surface.h>

#include "surface-private.h"

static int gui_flush_offset(size_t scanout_stride, int row, size_t line_offset,
			    off_t *offset)
{
	size_t byte_offset;

	if ((size_t)row > SIZE_MAX / scanout_stride)
		return 0;
	byte_offset = (size_t)row * scanout_stride;
	if (byte_offset > SIZE_MAX - line_offset)
		return 0;
	byte_offset += line_offset;
	if ((uint64_t)byte_offset > INT64_MAX)
		return 0;
	*offset = (off_t)byte_offset;
	return 1;
}

static gui_flush_result gui_flush_span(const char *pixels, size_t remaining,
				      off_t offset, gui_pwrite64_fn writer, void *context)
{
	while (remaining > 0) {
		size_t requested = remaining > SSIZE_MAX ? SSIZE_MAX : remaining;
		ssize_t written = writer(context, pixels, requested, offset);

		if (written > 0) {
			if ((size_t)written > requested)
				return GUI_FLUSH_ERROR;
			pixels += written;
			remaining -= (size_t)written;
			if (remaining > 0) {
				if ((uint64_t)offset > INT64_MAX - (uint64_t)written)
					return GUI_FLUSH_ERROR;
				offset += written;
			}
			continue;
		}
		if (written == -1 && errno == EINTR)
			continue;
		if (written == -1 && errno == EBUSY)
			return GUI_FLUSH_BUSY;
		return GUI_FLUSH_ERROR;
	}
	return GUI_FLUSH_COMPLETE;
}

gui_flush_result gui_surface_flush_rect(const gui_surface *surface,
					gui_rect rect, size_t scanout_stride,
					gui_pwrite64_fn pwrite64_fn, void *context)
{
	int clipped_x;
	int clipped_y;
	int width;
	int height;
	size_t line_offset;
	size_t line_bytes;
	size_t source_stride;
	off_t final_offset;

	if (pwrite64_fn == NULL || scanout_stride == 0)
		return GUI_FLUSH_ERROR;
	if (!gui_surface_clip_rect(surface, rect, &clipped_x, &clipped_y,
				   &width, &height))
		return GUI_FLUSH_COMPLETE;
	if ((size_t)clipped_x > SIZE_MAX / sizeof(gui_color) ||
	    (size_t)width > SIZE_MAX / sizeof(gui_color))
		return GUI_FLUSH_ERROR;
	line_offset = (size_t)clipped_x * sizeof(gui_color);
	line_bytes = (size_t)width * sizeof(gui_color);
	if (line_offset > scanout_stride || line_bytes > scanout_stride - line_offset)
		return GUI_FLUSH_ERROR;
	if (surface->stride < surface->width || surface->stride <= 0 ||
	    (size_t)surface->stride > SIZE_MAX / sizeof(gui_color))
		return GUI_FLUSH_ERROR;
	source_stride = (size_t)surface->stride * sizeof(gui_color);
	if (line_offset > SIZE_MAX - line_bytes ||
	    (size_t)(clipped_y + height - 1) >
		(SIZE_MAX - line_offset - line_bytes) / source_stride ||
	    !gui_flush_offset(scanout_stride, clipped_y + height - 1, line_offset,
		&final_offset) ||
	    (uint64_t)final_offset > INT64_MAX - (uint64_t)(line_bytes - 1))
		return GUI_FLUSH_ERROR;
	if (clipped_x == 0 && line_bytes == source_stride && line_bytes == scanout_stride) {
		off_t offset;

		if (line_bytes > SIZE_MAX / (size_t)height ||
		    !gui_flush_offset(scanout_stride, clipped_y, 0, &offset))
			return GUI_FLUSH_ERROR;
		return gui_flush_span((const char *)surface->pixels +
			(size_t)clipped_y * source_stride, line_bytes * height,
			offset, pwrite64_fn, context);
	}

	for (int row = 0; row < height; row++) {
		const char *pixels = (const char *)surface->pixels +
			((size_t)(clipped_y + row) * surface->stride + clipped_x) *
			sizeof(*surface->pixels);
		off_t offset;
		gui_flush_result result;

		if (!gui_flush_offset(scanout_stride, clipped_y + row, line_offset,
				      &offset))
			return GUI_FLUSH_ERROR;
		result = gui_flush_span(pixels, line_bytes, offset, pwrite64_fn, context);
		if (result != GUI_FLUSH_COMPLETE)
			return result;
	}
	return GUI_FLUSH_COMPLETE;
}

static ssize_t gui_flush_pwrite64(void *context, const void *buffer,
				  size_t bytes, off_t offset)
{
	return pwrite64(*(const int *)context, buffer, bytes, offset);
}

gui_flush_result gui_surface_flush_rect_fd(const gui_surface *surface,
					   gui_rect rect, size_t scanout_stride, int fd)
{
	return gui_surface_flush_rect(surface, rect, scanout_stride,
				      gui_flush_pwrite64, &fd);
}

gui_flush_result gui_surface_flush_region(const gui_surface *surface,
					  gui_region *region, size_t scanout_stride,
					  gui_pwrite64_fn pwrite64_fn, void *context)
{
	int index = 0;

	if (region == NULL || region->rects == NULL || region->count < 0 ||
	    region->count > region->capacity)
		return GUI_FLUSH_ERROR;
	while (index < region->count) {
		gui_region_rect rect = region->rects[index];
		gui_flush_result result = gui_surface_flush_rect(surface,
			(gui_rect) {
				.x = rect.x,
				.y = rect.y,
				.w = rect.width,
				.h = rect.height,
			}, scanout_stride, pwrite64_fn, context);

		if (result != GUI_FLUSH_COMPLETE)
			return result;
		/* Shift, rather than swap, so the next retry keeps region order. */
		for (int remaining = index + 1; remaining < region->count;
		     remaining++)
			region->rects[remaining - 1] = region->rects[remaining];
		region->count--;
	}
	region->full = 0;
	return GUI_FLUSH_COMPLETE;
}

gui_flush_result gui_surface_flush_region_fd(const gui_surface *surface,
					     gui_region *region, size_t scanout_stride,
					     int fd)
{
	return gui_surface_flush_region(surface, region, scanout_stride,
					gui_flush_pwrite64, &fd);
}
