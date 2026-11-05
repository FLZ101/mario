#ifndef GUI_FONT_H
#define GUI_FONT_H

#include <stddef.h>
#include <stdint.h>

#include <gui/surface.h>
#include <gui/text.h>

#define GUI_FONT_CACHE_MAX 2048

typedef struct gui_font gui_font;

typedef struct {
	uint16_t width;
	uint16_t height;
	uint16_t stride;
	const uint8_t *alpha;
	int advance;
	int x_offset;
	int y_offset;
} gui_font_glyph;

gui_font *gui_font_load(const char *path, int pixel_height);
void gui_font_destroy(gui_font *font);
int gui_font_get_metrics(const gui_font *font, gui_font_metrics *metrics);
int gui_font_get_glyph(gui_font *font, uint32_t codepoint,
		       gui_font_glyph *glyph);
int gui_font_lookup(void *context, uint32_t codepoint,
		    gui_glyph_metrics *metrics);
int gui_font_cache_count(const gui_font *font);
void gui_font_draw_glyph(gui_surface *surface, int x, int baseline,
			 const gui_font_glyph *glyph, gui_color color);
void gui_font_draw_text(gui_surface *surface, int x, int baseline,
			gui_font *font, const char *text, gui_color color);

#endif
