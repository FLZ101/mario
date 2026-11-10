#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <gui/image_cache.h>

static void gui_image_cache_entry_destroy(gui_image_cache_entry *entry)
{
	free(entry->path);
	gui_surface_destroy(entry->surface);
	*entry = (gui_image_cache_entry) { 0 };
}

static int gui_image_cache_entry_set(gui_image_cache_entry *entry, const char *path,
				     gui_surface *surface)
{
	char *path_copy;

	if (path == NULL || surface == NULL) {
		errno = EINVAL;
		return -1;
	}
	path_copy = strdup(path);
	if (path_copy == NULL)
		return -1;
	gui_image_cache_entry_destroy(entry);
	entry->path = path_copy;
	entry->surface = surface;
	return 0;
}

void gui_image_cache_init(gui_image_cache *cache)
{
	*cache = (gui_image_cache) { 0 };
}

void gui_image_cache_destroy(gui_image_cache *cache)
{
	gui_image_cache_entry_destroy(&cache->current);
	gui_image_cache_entry_destroy(&cache->next);
}

const char *gui_image_cache_current_path(const gui_image_cache *cache)
{
	return cache->current.path;
}

gui_surface *gui_image_cache_current(const gui_image_cache *cache)
{
	return cache->current.surface;
}

const char *gui_image_cache_next_path(const gui_image_cache *cache)
{
	return cache->next.path;
}

gui_surface *gui_image_cache_next(const gui_image_cache *cache)
{
	return cache->next.surface;
}

int gui_image_cache_set_current(gui_image_cache *cache, const char *path,
				gui_surface *surface)
{
	if (surface == cache->current.surface || surface == cache->next.surface) {
		errno = EINVAL;
		return -1;
	}
	if (gui_image_cache_entry_set(&cache->current, path, surface) < 0)
		return -1;
	/* A direct image change makes an old successor unrelated and evicts it. */
	gui_image_cache_entry_destroy(&cache->next);
	return 0;
}

int gui_image_cache_set_next(gui_image_cache *cache, const char *path,
			     gui_surface *surface)
{
	if (surface == cache->current.surface || surface == cache->next.surface ||
	    (cache->current.path != NULL && path != NULL && strcmp(path, cache->current.path) == 0)) {
		errno = EINVAL;
		return -1;
	}
	return gui_image_cache_entry_set(&cache->next, path, surface);
}

int gui_image_cache_promote_next(gui_image_cache *cache)
{
	if (cache->next.surface == NULL) {
		errno = ENOENT;
		return -1;
	}
	gui_image_cache_entry_destroy(&cache->current);
	cache->current = cache->next;
	cache->next = (gui_image_cache_entry) { 0 };
	return 0;
}
