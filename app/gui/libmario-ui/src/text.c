#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include <gui/text.h>

static int gui_utf8_continuation(unsigned char byte)
{
	return (byte & 0xc0) == 0x80;
}

static int gui_text_decode(const unsigned char *text, uint32_t *codepoint,
			   int *length)
{
	unsigned char first = text[0];
	unsigned char second;
	unsigned char third;
	unsigned char fourth;

	if (first < 0x80) {
		*codepoint = first;
		*length = 1;
		return 1;
	}
	if (first >= 0xc2 && first <= 0xdf) {
		second = text[1];
		if (second && gui_utf8_continuation(second)) {
			*codepoint = ((uint32_t)(first & 0x1f) << 6) |
				(second & 0x3f);
			*length = 2;
			return 1;
		}
	}
	if (first >= 0xe0 && first <= 0xef) {
		second = text[1];
		if (!second || !gui_utf8_continuation(second))
			return 0;
		third = text[2];
		if (third && gui_utf8_continuation(third) &&
		    !(first == 0xe0 && second < 0xa0) &&
		    !(first == 0xed && second > 0x9f)) {
			*codepoint = ((uint32_t)(first & 0x0f) << 12) |
				((uint32_t)(second & 0x3f) << 6) | (third & 0x3f);
			*length = 3;
			return 1;
		}
	}
	if (first >= 0xf0 && first <= 0xf4) {
		second = text[1];
		if (!second || !gui_utf8_continuation(second))
			return 0;
		third = text[2];
		if (!third || !gui_utf8_continuation(third))
			return 0;
		fourth = text[3];
		if (fourth && gui_utf8_continuation(fourth) &&
		    !(first == 0xf0 && second < 0x90) &&
		    !(first == 0xf4 && second > 0x8f)) {
			*codepoint = ((uint32_t)(first & 0x07) << 18) |
				((uint32_t)(second & 0x3f) << 12) |
				((uint32_t)(third & 0x3f) << 6) | (fourth & 0x3f);
			*length = 4;
			return 1;
		}
	}
	return 0;
}

int gui_text_next(const char **text, gui_text_glyph *glyph)
{
	const unsigned char *bytes;
	int length;

	if (text == NULL || *text == NULL || glyph == NULL)
		return 0;
	bytes = (const unsigned char *)*text;
	if (bytes[0] == '\0')
		return 0;
	if (!gui_text_decode(bytes, &glyph->codepoint, &length)) {
		/* Malformed UTF-8 consumes one byte and never reaches the font. */
		glyph->codepoint = GUI_CP437_FALLBACK_CODEPOINT;
		glyph->source = GUI_TEXT_GLYPH_CP437;
		*text += 1;
		return 1;
	}
	glyph->source = GUI_TEXT_GLYPH_FONT;
	*text += length;
	return 1;
}

static int gui_text_line_height(const gui_font_metrics *metrics, int *height)
{
	int64_t line_height;

	if (metrics->ascent < 0 || metrics->descent > 0 || metrics->line_gap < 0)
		return 0;
	line_height = (int64_t)metrics->ascent - metrics->descent +
		metrics->line_gap;
	if (line_height > INT_MAX)
		return 0;
	*height = (int)line_height;
	return 1;
}

int gui_text_measure(const char *text, const gui_font_metrics *font_metrics,
		     gui_text_lookup_fn lookup, void *context, gui_text_layout *layout)
{
	const char *cursor = text;
	gui_text_glyph glyph;
	gui_text_layout result = { 0 };

	if (text == NULL || font_metrics == NULL || layout == NULL ||
	    !gui_text_line_height(font_metrics, &result.height))
		return 0;
	result.baseline = font_metrics->ascent;
	while (gui_text_next(&cursor, &glyph)) {
		gui_glyph_metrics metrics;
		int advance = GUI_CP437_FALLBACK_ADVANCE;

		if (glyph.source == GUI_TEXT_GLYPH_FONT &&
		    lookup != NULL && lookup(context, glyph.codepoint, &metrics)) {
			if (metrics.advance < 0)
				return 0;
			advance = metrics.advance;
		} else {
			glyph.source = GUI_TEXT_GLYPH_CP437;
			if (result.fallback_count == INT_MAX)
				return 0;
			result.fallback_count++;
		}
		if (advance > INT_MAX - result.width ||
		    result.glyph_count == INT_MAX)
			return 0;
		result.width += advance;
		result.glyph_count++;
	}
	*layout = result;
	return 1;
}
