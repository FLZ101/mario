#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <gui/text.h>

static int lookup_ascii(void *context, uint32_t codepoint,
			gui_glyph_metrics *metrics)
{
	int *lookups = context;

	(*lookups)++;
	if (codepoint != 'A' && codepoint != 0x00e9)
		return 0;
	metrics->advance = codepoint == 'A' ? 9 : 11;
	return 1;
}

static void test_utf8_decode(void)
{
	const char *text = "A\xc3\xa9\xf0\x9f\x98\x80";
	gui_text_glyph glyph;

	assert(gui_text_next(&text, &glyph));
	assert(glyph.codepoint == 'A' && glyph.source == GUI_TEXT_GLYPH_FONT);
	assert(gui_text_next(&text, &glyph));
	assert(glyph.codepoint == 0x00e9 && glyph.source == GUI_TEXT_GLYPH_FONT);
	assert(gui_text_next(&text, &glyph));
	assert(glyph.codepoint == 0x1f600 && glyph.source == GUI_TEXT_GLYPH_FONT);
	assert(!gui_text_next(&text, &glyph));
}

static void test_malformed_utf8(void)
{
	const char *text = "\xc0\xaf" "A" "\xe2\x82";
	gui_text_glyph glyph;

	assert(gui_text_next(&text, &glyph));
	assert(glyph.codepoint == GUI_CP437_FALLBACK_CODEPOINT);
	assert(glyph.source == GUI_TEXT_GLYPH_CP437);
	assert(gui_text_next(&text, &glyph));
	assert(glyph.source == GUI_TEXT_GLYPH_CP437);
	assert(gui_text_next(&text, &glyph));
	assert(glyph.codepoint == 'A' && glyph.source == GUI_TEXT_GLYPH_FONT);
	assert(gui_text_next(&text, &glyph));
	assert(glyph.source == GUI_TEXT_GLYPH_CP437);
	assert(gui_text_next(&text, &glyph));
	assert(glyph.source == GUI_TEXT_GLYPH_CP437);
	assert(!gui_text_next(&text, &glyph));
}

static void test_layout(void)
{
	gui_font_metrics font_metrics = {
		.ascent = 12,
		.descent = -4,
		.line_gap = 3,
		.underline_position = -2,
		.underline_thickness = 1,
	};
	gui_text_layout layout;
	int lookups = 0;

	assert(gui_text_measure("A\xc3\xa9\xe2\x98\x83", &font_metrics,
				lookup_ascii, &lookups, &layout));
	assert(layout.width == 28);
	assert(layout.height == 19 && layout.baseline == 12);
	assert(layout.glyph_count == 3 && layout.fallback_count == 1);
	assert(lookups == 3);
	lookups = 0;
	assert(gui_text_measure("\xc0" "A", &font_metrics, lookup_ascii, &lookups,
				&layout));
	assert(layout.width == 17 && layout.fallback_count == 1);
	/* The malformed byte selected CP437 and never reached the font lookup. */
	assert(lookups == 1);
}

int main(void)
{
	test_utf8_decode();
	test_malformed_utf8();
	test_layout();
	puts("gui-text: decode ok");
	puts("gui-text: layout ok");
	return 0;
}
