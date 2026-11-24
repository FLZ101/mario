#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <gui/font.h>

#define FONT_PATH "/usr/share/fonts/DejaVuSansMono.ttf"
#define TERMINAL_FONT_PATH "/usr/share/fonts/DejaVuSansMNerdFontMono-Regular.ttf"

static uint32_t hash_bytes(const uint8_t *bytes, size_t count)
{
	uint32_t hash = 2166136261u;

	for (size_t index = 0; index < count; index++)
		hash = (hash ^ bytes[index]) * 16777619u;
	return hash;
}

static void test_glyph_cache(gui_font *font)
{
	FT_Library library;
	FT_Face face;
	FT_ULong codepoint;
	FT_UInt glyph_index;
	gui_font_glyph glyph;
	const uint8_t *cached_alpha;
	int inserted = 0;

	assert(gui_font_get_glyph(font, 'A', &glyph));
	assert(glyph.width > 0 && glyph.height > 0 && glyph.alpha != NULL);
	cached_alpha = glyph.alpha;
	assert(gui_font_get_glyph(font, 'A', &glyph));
	assert(glyph.alpha == cached_alpha);
	assert(gui_font_cache_count(font) == 1);

	assert(FT_Init_FreeType(&library) == 0);
	assert(FT_New_Face(library, FONT_PATH, 0, &face) == 0);
	codepoint = FT_Get_First_Char(face, &glyph_index);
	while (glyph_index && inserted <= GUI_FONT_CACHE_MAX) {
		assert(gui_font_get_glyph(font, codepoint, &glyph));
		inserted++;
		codepoint = FT_Get_Next_Char(face, codepoint, &glyph_index);
	}
	assert(inserted > GUI_FONT_CACHE_MAX);
	assert(gui_font_cache_count(font) == GUI_FONT_CACHE_MAX);
	assert(FT_Done_Face(face) == 0);
	assert(FT_Done_FreeType(library) == 0);
}

static void test_glyph_golden(gui_font *font)
{
	gui_font_glyph glyph;
	gui_surface *surface;
	uint32_t alpha_hash;
	int index;

	assert(gui_font_get_glyph(font, 'A', &glyph));
	alpha_hash = hash_bytes(glyph.alpha, (size_t)glyph.stride * glyph.height);
	assert(glyph.width == 10 && glyph.height == 12 && glyph.stride == 10);
	assert(glyph.advance == 10 && glyph.x_offset == 0 && glyph.y_offset == 12);
	assert(alpha_hash == 82655601u);

	surface = gui_surface_create(16, 16);
	assert(surface != NULL);
	gui_font_draw_glyph(surface, 1, 13, &glyph, 0x00ffffff);
	for (index = 0; index < surface->width * surface->height; index++)
		if (surface->pixels[index] != 0)
			break;
	assert(index < surface->width * surface->height);
	gui_surface_destroy(surface);
}

static void test_terminal_braille(void)
{
	gui_font *font = gui_font_load(TERMINAL_FONT_PATH, 14);
	gui_font_glyph letter, braille;

	assert(font != NULL);
	assert(gui_font_get_glyph(font, 'M', &letter));
	assert(gui_font_get_glyph(font, 0x28ff, &braille));
	assert(braille.width > 0 && braille.height > 0 && braille.alpha != NULL);
	assert(braille.advance == letter.advance);
	gui_font_destroy(font);
}

int main(void)
{
	gui_font *font = gui_font_load(FONT_PATH, 16);
	gui_font_metrics metrics;

	assert(font != NULL);
	assert(gui_font_get_metrics(font, &metrics));
	assert(metrics.ascent > 0 && metrics.descent <= 0);
	test_glyph_cache(font);
	test_glyph_golden(font);
	gui_font_destroy(font);
	puts("gui-font: cache ok");
	puts("gui-font: glyph ok");
	test_terminal_braille();
	puts("gui-font: terminal braille ok");
	return 0;
}
