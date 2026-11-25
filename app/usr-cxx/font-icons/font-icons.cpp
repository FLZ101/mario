#include "font-icons.h"

#include <FL/Fl_Widget.H>

extern "C" {
#include <gui/font.h>
}

#include <climits>

namespace mario {

IconFont::IconFont(const char *path, int pixel_height, int cell_size)
	: font_(gui_font_load(path, pixel_height), gui_font_destroy), cell_size_(cell_size)
{
}

std::unique_ptr<Fl_RGB_Image> IconFont::render(std::uint32_t codepoint, std::uint32_t rgb) const
{
	if (cell_size_ <= 0 || cell_size_ > INT_MAX / 4 / cell_size_)
		return {};
	gui_font_glyph glyph{};
	if (!gui_font_get_glyph(font_.get(), codepoint, &glyph) || !glyph.width || !glyph.height ||
	    glyph.width > cell_size_ || glyph.height > cell_size_)
		return {};
	auto rgba = std::make_unique<uchar[]>(cell_size_ * cell_size_ * 4);
	const int left = (cell_size_ - glyph.width) / 2;
	const int top = (cell_size_ - glyph.height) / 2;
	for (int row = 0; row < glyph.height; ++row)
		for (int column = 0; column < glyph.width; ++column) {
			auto *pixel = rgba.get() + ((top + row) * cell_size_ + left + column) * 4;
			pixel[0] = rgb >> 16;
			pixel[1] = rgb >> 8;
			pixel[2] = rgb;
			pixel[3] = glyph.alpha[row * glyph.stride + column];
		}
	auto image = std::make_unique<Fl_RGB_Image>(rgba.get(), cell_size_, cell_size_, 4);
	image->alloc_array = 1;
	rgba.release();
	return image;
}

void set_label_icon(Fl_Widget &widget, Fl_Image *image)
{
	widget.image(image);
	auto *disabled = image ? image->copy() : nullptr;
	if (disabled)
		disabled->inactive();
	widget.bind_deimage(disabled);
	if (image) {
		widget.align(FL_ALIGN_INSIDE | FL_ALIGN_IMAGE_NEXT_TO_TEXT);
		widget.label_image_spacing(4);
	}
	widget.redraw();
}

}
