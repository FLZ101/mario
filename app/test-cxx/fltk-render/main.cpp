#include <FL/Fl_Image_Surface.H>
#include <FL/fl_draw.H>

#include <cstdio>
#include <cstdint>
#include <memory>

static uint32_t pixel(const Fl_RGB_Image &image, int x, int y)
{
	const auto *bytes = image.array + (y * image.data_w() + x) * 3;
	return uint32_t(bytes[0]) << 16 | uint32_t(bytes[1]) << 8 | bytes[2];
}

static bool check(const Fl_RGB_Image &image, int x, int y, uint32_t expected)
{
	const auto actual = pixel(image, x, y);
	if (actual == expected)
		return true;
	std::printf("fltk-render: pixel (%d,%d) = %06x, expected %06x\n", x, y, actual, expected);
	return false;
}

static bool shapes_and_images()
{
	Fl_Image_Surface outer(40, 32), inner(3, 3);
	Fl_Surface_Device::push_current(&outer);
	fl_color(0, 0, 255);
	fl_rectf(0, 0, 40, 32);
	outer.origin(3, 4);
	fl_push_clip(2, 2, 10, 10);
	fl_push_clip(4, 3, 4, 5);
	fl_color(255, 0, 0);
	fl_rectf(0, 0, 20, 20);
	Fl_Surface_Device::push_current(&inner);
	fl_color(255, 255, 0);
	fl_rectf(0, 0, 3, 3);
	Fl_Surface_Device::pop_current();
	fl_copy_offscreen(5, 4, 3, 3, inner.offscreen(), 0, 0);
	fl_pop_clip();
	fl_color(0, 255, 0);
	fl_line(-100, 10, 100, 10);
	fl_pop_clip();
	outer.origin(0, 0);
	fl_color(255, 255, 255);
	fl_begin_complex_polygon();
	fl_vertex(20, 15); fl_vertex(35, 15); fl_vertex(35, 29); fl_vertex(20, 29);
	fl_gap();
	fl_vertex(24, 20); fl_vertex(30, 20); fl_vertex(30, 25); fl_vertex(24, 25);
	fl_end_complex_polygon();
	const uchar rgba[] = {255, 0, 0, 128, 0, 255, 0, 255};
	Fl_RGB_Image overlay(rgba, 2, 1, 4);
	overlay.draw(20, 3);
	const uchar rgb[] = {10, 20, 30};
	fl_draw_image(rgb, 22, 3, 1, 1, 3);
	Fl_Surface_Device::pop_current();
	std::unique_ptr<Fl_RGB_Image> image(outer.image());
	return image && check(*image, 6, 7, 0x0000ff) && check(*image, 7, 7, 0xff0000) &&
	       check(*image, 8, 8, 0xffff00) && check(*image, 10, 10, 0xffff00) &&
	       check(*image, 11, 10, 0x0000ff) && check(*image, 5, 14, 0x00ff00) &&
	       check(*image, 14, 14, 0x00ff00) && check(*image, 15, 14, 0x0000ff) &&
	       check(*image, 22, 22, 0xffffff) && check(*image, 26, 22, 0x0000ff) &&
	       check(*image, 20, 3, 0x80007f) && check(*image, 21, 3, 0x00ff00) &&
	       check(*image, 22, 3, 0x0a141e);
}

static bool text_placement()
{
	Fl_Image_Surface surface(36, 22);
	Fl_Surface_Device::push_current(&surface);
	fl_font(FL_COURIER, 16);
	const int advance = static_cast<int>(fl_width("M"));
	const int pair_width = static_cast<int>(fl_width("MM"));
	const int space_width = static_cast<int>(fl_width(" "));
	fl_color(255, 255, 255);
	fl_draw("MM", 2, 3, 17);
	Fl_Surface_Device::pop_current();
	std::unique_ptr<Fl_RGB_Image> image(surface.image());
	if (!image || advance != 10 || pair_width != 2 * advance || space_width != advance) {
		std::printf("fltk-render: unexpected font advances %d, %d, %d\n",
			    advance, pair_width, space_width);
		return false;
	}
	int ink = 0;
	for (int row = 0; row < 22; ++row) {
		for (int col = 3; col < 3 + advance; ++col) {
			const auto value = pixel(*image, col, row);
			ink += value != 0;
			if (value != pixel(*image, col + advance, row) || (value && row >= 17))
				return false;
		}
	}
	return ink > 0 && check(*image, 2, 10, 0) && check(*image, 23, 10, 0);
}

int main()
{
	Fl_Image_Surface oversized(65536, 65536);
	if (oversized.offscreen() || oversized.image() || !shapes_and_images() || !text_placement())
		return 1;
	std::puts("fltk-render: clipping, ARGB, paths and font advances ok");
	return 0;
}
