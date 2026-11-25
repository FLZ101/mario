#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <gui/font.h>

#define GUI_FONT_FILE_MAX (8 * 1024 * 1024)

static const uint8_t gui_cp437_fallback_rows[16] = {
	0x3c, 0x42, 0x02, 0x04, 0x08, 0x10, 0x10, 0x00,
	0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

typedef struct {
	uint32_t codepoint;
	uint8_t *alpha;
	uint16_t width;
	uint16_t height;
	uint16_t stride;
	int advance;
	int x_offset;
	int y_offset;
	unsigned long used;
	int valid;
} gui_font_cache_entry;

struct gui_font {
	FT_Library library;
	FT_Face face;
	uint8_t *file_data;
	gui_font_metrics metrics;
	gui_font_cache_entry cache[GUI_FONT_CACHE_MAX];
	unsigned long clock;
	int cache_count;
};

static int gui_ft_ceil(FT_Pos value)
{
	if (value < 0)
		return (int)(value / 64);
	return (int)((value + 63) / 64);
}

static int gui_ft_floor(FT_Pos value)
{
	if (value >= 0)
		return (int)(value / 64);
	return -(int)((-value + 63) / 64);
}

static int gui_font_read_file(const char *path, uint8_t **file_data,
			      size_t *file_size)
{
	struct stat statbuf;
	uint8_t *data;
	int fd;
	size_t offset = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	if (fstat(fd, &statbuf) || statbuf.st_size <= 0 ||
	    statbuf.st_size > GUI_FONT_FILE_MAX) {
		close(fd);
		return 0;
	}
	data = malloc((size_t)statbuf.st_size);
	if (data == NULL) {
		close(fd);
		return 0;
	}
	while (offset < (size_t)statbuf.st_size) {
		ssize_t result = read(fd, data + offset,
				      (size_t)statbuf.st_size - offset);

		if (result > 0) {
			offset += (size_t)result;
			continue;
		}
		if (result == -1 && errno == EINTR)
			continue;
		free(data);
		close(fd);
		return 0;
	}
	if (close(fd)) {
		free(data);
		return 0;
	}
	*file_data = data;
	*file_size = offset;
	return 1;
}

static void gui_font_set_metrics(gui_font *font)
{
	FT_Size_Metrics metrics = font->face->size->metrics;
	int height;

	font->metrics.ascent = gui_ft_ceil(metrics.ascender);
	font->metrics.descent = gui_ft_floor(metrics.descender);
	height = gui_ft_ceil(metrics.height);
	font->metrics.line_gap = height -
		(font->metrics.ascent - font->metrics.descent);
	if (font->metrics.line_gap < 0)
		font->metrics.line_gap = 0;
	font->metrics.underline_position = gui_ft_floor(FT_MulFix(
		font->face->underline_position, metrics.y_scale));
	font->metrics.underline_thickness = gui_ft_ceil(FT_MulFix(
		font->face->underline_thickness, metrics.y_scale));
}

gui_font *gui_font_load(const char *path, int pixel_height)
{
	gui_font *font;
	size_t file_size;

	if (path == NULL || pixel_height <= 0)
		return NULL;
	font = calloc(1, sizeof(*font));
	if (font == NULL)
		return NULL;
	if (!gui_font_read_file(path, &font->file_data, &file_size) ||
	    FT_Init_FreeType(&font->library) ||
	    FT_New_Memory_Face(font->library, font->file_data, file_size, 0,
			       &font->face) ||
	    FT_Set_Pixel_Sizes(font->face, 0, (FT_UInt)pixel_height)) {
		gui_font_destroy(font);
		return NULL;
	}
	gui_font_set_metrics(font);
	return font;
}

void gui_font_destroy(gui_font *font)
{
	if (font == NULL)
		return;
	for (int index = 0; index < GUI_FONT_CACHE_MAX; index++)
		free(font->cache[index].alpha);
	if (font->face != NULL)
		FT_Done_Face(font->face);
	if (font->library != NULL)
		FT_Done_FreeType(font->library);
	free(font->file_data);
	free(font);
}

int gui_font_get_metrics(const gui_font *font, gui_font_metrics *metrics)
{
	if (font == NULL || metrics == NULL)
		return 0;
	*metrics = font->metrics;
	return 1;
}

static gui_font_cache_entry *gui_font_find_entry(gui_font *font,
						  uint32_t codepoint)
{
	for (int index = 0; index < GUI_FONT_CACHE_MAX; index++)
		if (font->cache[index].valid &&
		    font->cache[index].codepoint == codepoint)
			return &font->cache[index];
	return NULL;
}

static gui_font_cache_entry *gui_font_new_entry(gui_font *font)
{
	gui_font_cache_entry *entry = NULL;

	if (font->cache_count < GUI_FONT_CACHE_MAX)
		entry = &font->cache[font->cache_count];
	else {
		for (int index = 0; index < GUI_FONT_CACHE_MAX; index++)
			if (entry == NULL || font->cache[index].used < entry->used)
				entry = &font->cache[index];
		free(entry->alpha);
	}
	memset(entry, 0, sizeof(*entry));
	return entry;
}

static int gui_font_rasterize(gui_font *font, uint32_t codepoint,
			      gui_font_cache_entry *entry)
{
	FT_Bitmap *bitmap;
	FT_GlyphSlot slot;
	FT_UInt glyph_index;
	size_t bytes;

	glyph_index = FT_Get_Char_Index(font->face, codepoint);
	if (glyph_index == 0 || FT_Load_Glyph(font->face, glyph_index,
					      FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) ||
	    FT_Render_Glyph(font->face->glyph, FT_RENDER_MODE_NORMAL))
		return 0;
	slot = font->face->glyph;
	bitmap = &slot->bitmap;
	if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY || bitmap->width > UINT16_MAX ||
	    bitmap->rows > UINT16_MAX || bitmap->pitch < 0 ||
	    (bitmap->rows != 0 && (size_t)bitmap->width > SIZE_MAX / bitmap->rows))
		return 0;
	bytes = (size_t)bitmap->width * bitmap->rows;
	if (bytes) {
		entry->alpha = malloc(bytes);
		if (entry->alpha == NULL)
			return 0;
		for (int row = 0; row < (int)bitmap->rows; row++)
			memcpy(entry->alpha + (size_t)row * bitmap->width,
			       bitmap->buffer + (size_t)row * bitmap->pitch,
			       bitmap->width);
	}
	entry->codepoint = codepoint;
	entry->width = (uint16_t)bitmap->width;
	entry->height = (uint16_t)bitmap->rows;
	entry->stride = (uint16_t)bitmap->width;
	entry->advance = gui_ft_ceil(slot->advance.x);
	entry->x_offset = slot->bitmap_left;
	entry->y_offset = slot->bitmap_top;
	entry->valid = 1;
	return 1;
}

static void gui_font_copy_glyph(const gui_font_cache_entry *entry,
				gui_font_glyph *glyph)
{
	glyph->width = entry->width;
	glyph->height = entry->height;
	glyph->stride = entry->stride;
	glyph->alpha = entry->alpha;
	glyph->advance = entry->advance;
	glyph->x_offset = entry->x_offset;
	glyph->y_offset = entry->y_offset;
}

int gui_font_get_glyph(gui_font *font, uint32_t codepoint,
		       gui_font_glyph *glyph)
{
	gui_font_cache_entry *entry;

	if (font == NULL || glyph == NULL || codepoint > 0x10ffff)
		return 0;
	entry = gui_font_find_entry(font, codepoint);
	if (entry == NULL) {
		int new_entry = font->cache_count < GUI_FONT_CACHE_MAX;

		entry = gui_font_new_entry(font);
		if (!gui_font_rasterize(font, codepoint, entry))
			return 0;
		if (new_entry)
			font->cache_count++;
	}
	entry->used = ++font->clock;
	gui_font_copy_glyph(entry, glyph);
	return 1;
}

int gui_font_lookup(void *context, uint32_t codepoint,
		    gui_glyph_metrics *metrics)
{
	gui_font_glyph glyph;

	if (metrics == NULL || !gui_font_get_glyph(context, codepoint, &glyph))
		return 0;
	metrics->advance = glyph.advance;
	return 1;
}

int gui_font_cache_count(const gui_font *font)
{
	return font == NULL ? 0 : font->cache_count;
}

void gui_font_draw_glyph(gui_surface *surface, int x, int baseline,
			 const gui_font_glyph *glyph, gui_color color)
{
	int64_t left;
	int64_t top;
	int alpha = color >> 24;

	if (surface == NULL || surface->pixels == NULL || glyph == NULL ||
	    glyph->alpha == NULL || glyph->stride < glyph->width)
		return;
	if (alpha == 0)
		alpha = 255;
	left = (int64_t)x + glyph->x_offset;
	top = (int64_t)baseline - glyph->y_offset;
	for (int row = 0; row < glyph->height; row++)
		for (int column = 0; column < glyph->width; column++) {
			int64_t destination_x = left + column;
			int64_t destination_y = top + row;
			int coverage;
			int combined_alpha;
			int red;
			int green;
			int blue;
			gui_color destination;
			int inverse_alpha;

			if (destination_x < 0 || destination_x >= surface->width ||
			    destination_y < 0 || destination_y >= surface->height)
				continue;
			coverage = glyph->alpha[row * glyph->stride + column];
			combined_alpha = coverage * alpha / 255;
			red = ((color >> 16) & 0xff) * combined_alpha / 255;
			green = ((color >> 8) & 0xff) * combined_alpha / 255;
			blue = (color & 0xff) * combined_alpha / 255;
			destination = surface->pixels[destination_y * surface->stride +
				destination_x];
			inverse_alpha = 255 - combined_alpha;
			surface->pixels[destination_y * surface->stride + destination_x] =
				((gui_color)(combined_alpha +
				 ((destination >> 24) & 0xff) * inverse_alpha / 255) << 24) |
				((gui_color)(red + ((destination >> 16) & 0xff) *
				 inverse_alpha / 255) << 16) |
				((gui_color)(green + ((destination >> 8) & 0xff) *
				 inverse_alpha / 255) << 8) |
				(gui_color)(blue + (destination & 0xff) * inverse_alpha / 255);
		}
}

static void gui_font_draw_cp437_fallback(gui_surface *surface, int x,
					 int baseline, gui_color color)
{
	uint8_t alpha[8 * 16];
	gui_font_glyph glyph = {
		.width = 8,
		.height = 16,
		.stride = 8,
		.alpha = alpha,
		.advance = GUI_CP437_FALLBACK_ADVANCE,
		.y_offset = 14,
	};

	for (int row = 0; row < glyph.height; row++)
		for (int column = 0; column < glyph.width; column++)
			alpha[row * glyph.stride + column] =
				gui_cp437_fallback_rows[row] & (0x80 >> column) ? 255 : 0;
	gui_font_draw_glyph(surface, x, baseline, &glyph, color);
}

void gui_font_draw_text(gui_surface *surface, int x, int baseline,
			gui_font *font, const char *text, gui_color color)
{
	const char *cursor = text;
	gui_text_glyph decoded;

	if (surface == NULL || font == NULL || text == NULL)
		return;
	while (gui_text_next(&cursor, &decoded)) {
		gui_font_glyph glyph;

		if (decoded.source == GUI_TEXT_GLYPH_FONT &&
		    gui_font_get_glyph(font, decoded.codepoint, &glyph)) {
			gui_font_draw_glyph(surface, x, baseline, &glyph, color);
			x += glyph.advance;
		} else {
			/* Keep malformed and missing text visible without a font glyph. */
			gui_font_draw_cp437_fallback(surface, x, baseline, color);
			x += GUI_CP437_FALLBACK_ADVANCE;
		}
	}
}
