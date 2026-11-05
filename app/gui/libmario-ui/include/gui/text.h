#ifndef GUI_TEXT_H
#define GUI_TEXT_H

#include <stdint.h>

#define GUI_CP437_FALLBACK_CODEPOINT 0x003fu
#define GUI_CP437_FALLBACK_ADVANCE 8

typedef enum {
	GUI_TEXT_GLYPH_FONT,
	GUI_TEXT_GLYPH_CP437,
} gui_text_glyph_source;

typedef struct {
	uint32_t codepoint;
	gui_text_glyph_source source;
} gui_text_glyph;

typedef struct {
	int advance;
} gui_glyph_metrics;

typedef struct {
	int ascent;
	int descent;
	int line_gap;
	int underline_position;
	int underline_thickness;
} gui_font_metrics;

typedef int (*gui_text_lookup_fn)(void *context, uint32_t codepoint,
				  gui_glyph_metrics *metrics);

typedef struct {
	int width;
	int height;
	int baseline;
	int glyph_count;
	int fallback_count;
} gui_text_layout;

int gui_text_next(const char **text, gui_text_glyph *glyph);
int gui_text_measure(const char *text, const gui_font_metrics *font_metrics,
		     gui_text_lookup_fn lookup, void *context, gui_text_layout *layout);

#endif
