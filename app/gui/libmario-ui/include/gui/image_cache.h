#ifndef GUI_IMAGE_CACHE_H
#define GUI_IMAGE_CACHE_H

#include <gui/surface.h>

/*
 * The viewer keeps only the frame on screen and its preloaded successor.
 * Every successful replacement transfers surface ownership to this cache.
 */
typedef struct {
	char *path;
	gui_surface *surface;
} gui_image_cache_entry;

typedef struct {
	gui_image_cache_entry current;
	gui_image_cache_entry next;
} gui_image_cache;

void gui_image_cache_init(gui_image_cache *cache);
void gui_image_cache_destroy(gui_image_cache *cache);

const char *gui_image_cache_current_path(const gui_image_cache *cache);
gui_surface *gui_image_cache_current(const gui_image_cache *cache);
const char *gui_image_cache_next_path(const gui_image_cache *cache);
gui_surface *gui_image_cache_next(const gui_image_cache *cache);

/* Replace both cache entries with the newly displayed decoded frame. */
int gui_image_cache_set_current(gui_image_cache *cache, const char *path,
				gui_surface *surface);
/* Replace only the preloaded successor; the displayed frame is retained. */
int gui_image_cache_set_next(gui_image_cache *cache, const char *path,
			     gui_surface *surface);
/* Promote the preloaded successor and evict the formerly displayed frame. */
int gui_image_cache_promote_next(gui_image_cache *cache);

#endif
