#include <assert.h>
#include <stdio.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#define FONT_PATH "/usr/share/fonts/DejaVuSansMono.ttf"

int main(void)
{
	FT_Face face;
	FT_Library library;

	assert(FT_Init_FreeType(&library) == 0);
	/* Opening this path verifies that the bundled TTF reached the ramdisk. */
	assert(FT_New_Face(library, FONT_PATH, 0, &face) == 0);
	assert(FT_Set_Pixel_Sizes(face, 0, 16) == 0);

	printf("font: path=%s\n", FONT_PATH);
	printf("font: family=%s style=%s glyphs=%ld units-per-em=%u "
	       "ascender=%d descender=%d height=%d max-advance=%d\n",
	       face->family_name, face->style_name, (long)face->num_glyphs,
	       face->units_per_EM, face->ascender, face->descender, face->height,
	       face->max_advance_width);
	printf("font: pixels=%u,%u ascender=%ld descender=%ld height=%ld "
	       "max-advance=%ld\n",
	       face->size->metrics.x_ppem, face->size->metrics.y_ppem,
	       face->size->metrics.ascender, face->size->metrics.descender,
	       face->size->metrics.height, face->size->metrics.max_advance);

	FT_Done_Face(face);
	FT_Done_FreeType(library);
	return 0;
}
