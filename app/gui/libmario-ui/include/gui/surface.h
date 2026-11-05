#ifndef GUI_SURFACE_H
#define GUI_SURFACE_H

#include <stddef.h>
#include <sys/types.h>
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

#define GUI_REGION_MAX_RECTS 64

typedef struct {
	int x;
	int y;
	int width;
	int height;
} gui_region_rect;

typedef struct {
	int count;
	int capacity;
	gui_region_rect *rects;
	gui_rect bounds;
	int full;
} gui_region;

typedef ssize_t (*gui_pwrite64_fn)(void *context, const void *buffer,
				   size_t bytes, off_t offset);

typedef enum {
	GUI_FLUSH_COMPLETE,
	GUI_FLUSH_BUSY,
	GUI_FLUSH_ERROR,
} gui_flush_result;

gui_surface *gui_surface_create(int width, int height);
void gui_surface_destroy(gui_surface *surface);
void gui_fill_rect(gui_surface *surface, gui_rect rect, gui_color color);
void gui_blit(gui_surface *destination, int destination_x, int destination_y,
	      const gui_surface *source, const gui_rect *source_rect);
void gui_region_init(gui_region *region, gui_region_rect *rects, int capacity,
		     gui_rect bounds);
void gui_region_add(gui_region *region, gui_rect rect);
void gui_region_clear(gui_region *region);
void gui_region_mark_full(gui_region *region);
gui_flush_result gui_surface_flush_rect(const gui_surface *surface,
					gui_rect rect, size_t scanout_stride,
					gui_pwrite64_fn pwrite64_fn, void *context);
gui_flush_result gui_surface_flush_rect_fd(const gui_surface *surface,
					   gui_rect rect, size_t scanout_stride, int fd);
gui_flush_result gui_surface_flush_region(const gui_surface *surface,
					  gui_region *region, size_t scanout_stride,
					  gui_pwrite64_fn pwrite64_fn, void *context);
gui_flush_result gui_surface_flush_region_fd(const gui_surface *surface,
					     gui_region *region, size_t scanout_stride,
					     int fd);

#endif
