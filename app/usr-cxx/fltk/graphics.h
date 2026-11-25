#ifndef MARIO_FLTK_GRAPHICS_H
#define MARIO_FLTK_GRAPHICS_H

#include <FL/Fl_Graphics_Driver.H>

#include <memory>
#include <vector>

extern "C" {
#include <gui/font.h>
#include <gui/surface.h>
}

namespace mario_fltk {

struct Surface_Deleter {
	void operator()(gui_surface *surface) const;
};

using Pixels = std::unique_ptr<gui_surface, Surface_Deleter>;

Pixels make_pixels(int width, int height);

class Graphics_Driver final : public Fl_Graphics_Driver {
public:
	~Graphics_Driver() override;
	void target(gui_surface *surface);
	void set_origin(int x, int y);
	void push_origin(int x, int y);
	void pop_origin();
	Fl_RGB_Image *snapshot() const;
	char can_do_alpha_blending() override { return 1; }
	int has_feature(driver_feature feature) override { return feature == NATIVE; }

	using Fl_Graphics_Driver::color;
	using Fl_Graphics_Driver::line;
	using Fl_Graphics_Driver::arc;
	void color(uchar red, uchar green, uchar blue) override;
	void point(int x, int y) override;
	void rect(int x, int y, int width, int height) override;
	void rectf(int x, int y, int width, int height) override;
	void line(int x, int y, int end_x, int end_y) override;
	void polygon(int x0, int y0, int x1, int y1, int x2, int y2) override;
	void polygon(int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3) override;
	void arc(int x, int y, int width, int height, double start, double end) override;
	void pie(int x, int y, int width, int height, double start, double end) override;
	void circle(double x, double y, double radius) override;

	void push_clip(int x, int y, int width, int height) override;
	void push_no_clip() override;
	void pop_clip() override;
	Fl_Region XRectangleRegion(int x, int y, int width, int height) override;
	void add_rectangle_to_region(Fl_Region region, int x, int y, int width, int height) override;
	void XDestroyRegion(Fl_Region region) override;
	int clip_box(int x, int y, int width, int height, int &out_x, int &out_y,
		     int &out_width, int &out_height) override;
	int not_clipped(int x, int y, int width, int height) override;

	void begin_complex_polygon() override;
	void gap() override;
	void end_points() override;
	void end_line() override;
	void end_polygon() override;
	void end_complex_polygon() override;

	void font(Fl_Font face, Fl_Fontsize size) override;
	double width(unsigned int codepoint) override;
	double width(const char *text, int count) override;
	int height() override;
	int descent() override;
	void draw(const char *text, int count, int x, int y) override;

	void draw_image(const uchar *bytes, int x, int y, int width, int height,
			int depth, int stride) override;
	void draw_image(Fl_Draw_Image_Cb callback, void *context, int x, int y,
			int width, int height, int depth) override;
	void draw_image_mono(const uchar *bytes, int x, int y, int width, int height,
			     int depth, int stride) override;
	void draw_image_mono(Fl_Draw_Image_Cb callback, void *context, int x, int y,
			     int width, int height, int depth) override;
	void draw_rgb(Fl_RGB_Image *image, int x, int y, int width, int height,
		      int source_x, int source_y) override;
	void draw_bitmap(Fl_Bitmap *image, int x, int y, int width, int height,
			 int source_x, int source_y) override;
	void draw_pixmap(Fl_Pixmap *image, int x, int y, int width, int height,
			 int source_x, int source_y) override;
	void copy_offscreen(int x, int y, int width, int height, Fl_Offscreen source,
			    int source_x, int source_y) override;

private:
	struct Origin { int x; int y; };
	gui_surface *surface_ = nullptr;
	Origin origin_{};
	std::vector<Origin> origins_;
	std::vector<int> contours_;
	int overflow_clips_ = 0;

	gui_rect bounds() const;
	gui_rect absolute(int x, int y, int width, int height) const;
	gui_color ink();
	void blend(int x, int y, gui_color color, unsigned alpha, const gui_rect &clip);
	void ellipse_path(int x, int y, int width, int height, double start, double end);
	void fill_path(const std::vector<int> &contours);
	gui_font *font_for_size();
	gui_font_metrics metrics();
	void image_rows(const uchar *bytes, Fl_Draw_Image_Cb callback, void *context,
			int x, int y, int width, int height, int depth, int stride, bool mono);
};

}

#endif
