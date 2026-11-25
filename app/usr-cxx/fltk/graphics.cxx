#include "graphics.h"

#include <FL/Fl.H>
#include <FL/Fl_Bitmap.H>
#include <FL/Fl_Pixmap.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

extern int fl_convert_pixmap(const char *const *data, uchar *output, Fl_Color background);

namespace mario_fltk {
namespace {

constexpr size_t pixel_limit = 16 * 1024 * 1024;
constexpr int dimension_limit = 8192;
constexpr double radians = 3.14159265358979323846 / 180.0;
size_t surface_pixels;

int coord(int64_t value)
{
	return static_cast<int>(std::clamp<int64_t>(value, INT32_MIN, INT32_MAX));
}

int path_coord(double value)
{
	if (std::isnan(value))
		return 0;
	return static_cast<int>(std::round(std::clamp(value, double(INT32_MIN), double(INT32_MAX))));
}

int64_t right(const gui_rect &rect) { return int64_t(rect.x) + rect.w; }
int64_t bottom(const gui_rect &rect) { return int64_t(rect.y) + rect.h; }

gui_rect intersect(gui_rect left, gui_rect other)
{
	const int x = std::max(left.x, other.x);
	const int y = std::max(left.y, other.y);
	return {x, y, coord(std::max<int64_t>(0, std::min(right(left), right(other)) - x)),
		coord(std::max<int64_t>(0, std::min(bottom(left), bottom(other)) - y))};
}

unsigned multiply_alpha(unsigned channel, unsigned alpha)
{
	return (channel * alpha + 127) / 255;
}

gui_color source_over(gui_color color, gui_color destination, unsigned alpha)
{
	if (alpha == 255)
		return color | 0xff000000U;
	unsigned inverse = 255 - alpha;
	gui_color result = (alpha + multiply_alpha(destination >> 24, inverse)) << 24;
	for (unsigned shift : {0U, 8U, 16U}) {
		unsigned channel = multiply_alpha((color >> shift) & 255, alpha) +
				   multiply_alpha((destination >> shift) & 255, inverse);
		result |= channel << shift;
	}
	return result;
}

template <typename Visit>
void text_codepoints(const char *text, int count, Visit visit)
{
	if (!text || count <= 0)
		return;
	const char *end = text + count;
	while (text < end && *text) {
		int length;
		const unsigned codepoint = fl_utf8decode(text, end, &length);
		visit(codepoint);
		text += length;
	}
}

bool lookup(gui_font *font, unsigned codepoint, gui_font_glyph &glyph)
{
	return gui_font_get_glyph(font, codepoint, &glyph) ||
	       gui_font_get_glyph(font, '?', &glyph);
}

struct Font_Entry {
	std::unique_ptr<gui_font, decltype(&gui_font_destroy)> font{nullptr, gui_font_destroy};
	int family = -1;
	int size = 0;
};

}

Pixels make_pixels(int width, int height)
{
	if (width <= 0 || height <= 0 || width > dimension_limit || height > dimension_limit)
		return {};
	const auto count = size_t(width) * height;
	if (count > pixel_limit - surface_pixels)
		return {};
	Pixels surface(gui_surface_create(width, height));
	if (surface)
		surface_pixels += count;
	return surface;
}

void Surface_Deleter::operator()(gui_surface *surface) const
{
	surface_pixels -= size_t(surface->width) * surface->height;
	gui_surface_destroy(surface);
}

Graphics_Driver::~Graphics_Driver()
{
	for (int index = 0; index <= rstackptr; ++index)
		XDestroyRegion(rstack[index]);
}

void Graphics_Driver::target(gui_surface *surface)
{
	surface_ = surface;
}

void Graphics_Driver::set_origin(int x, int y)
{
	origin_ = {x, y};
}

void Graphics_Driver::push_origin(int x, int y)
{
	origins_.push_back(origin_);
	origin_ = {coord(int64_t(origin_.x) + x), coord(int64_t(origin_.y) + y)};
}

void Graphics_Driver::pop_origin()
{
	if (!origins_.empty()) {
		origin_ = origins_.back();
		origins_.pop_back();
	}
}

gui_rect Graphics_Driver::bounds() const
{
	if (!surface_ || overflow_clips_)
		return {};
	const gui_rect surface{0, 0, surface_->width, surface_->height};
	const auto *clip = static_cast<const gui_rect *>(rstack[rstackptr]);
	return clip ? intersect(surface, *clip) : surface;
}

gui_rect Graphics_Driver::absolute(int x, int y, int width, int height) const
{
	return {coord(int64_t(x) + origin_.x), coord(int64_t(y) + origin_.y), width, height};
}

gui_color Graphics_Driver::ink()
{
	return (Fl::get_color(color()) >> 8) | 0xff000000U;
}

void Graphics_Driver::color(uchar red, uchar green, uchar blue)
{
	color(fl_rgb_color(red, green, blue));
}

void Graphics_Driver::push_clip(int x, int y, int width, int height)
{
	if (rstackptr == region_stack_max) {
		++overflow_clips_;
		return;
	}
	auto clip = intersect(bounds(), absolute(x, y, width, height));
	rstack[++rstackptr] = new gui_rect(clip);
	restore_clip();
}

void Graphics_Driver::push_no_clip()
{
	if (rstackptr == region_stack_max) {
		++overflow_clips_;
		return;
	}
	Fl_Graphics_Driver::push_no_clip();
}

void Graphics_Driver::pop_clip()
{
	if (overflow_clips_)
		--overflow_clips_;
	else
		Fl_Graphics_Driver::pop_clip();
}

Fl_Region Graphics_Driver::XRectangleRegion(int x, int y, int width, int height)
{
	return new gui_rect{x, y, width, height};
}

void Graphics_Driver::add_rectangle_to_region(Fl_Region region, int x, int y, int width, int height)
{
	if (!region || width <= 0 || height <= 0)
		return;
	auto &rect = *static_cast<gui_rect *>(region);
	const int min_x = std::min(x, rect.x), min_y = std::min(y, rect.y);
	rect = {min_x, min_y, coord(std::max(right(rect), int64_t(x) + width) - min_x),
		coord(std::max(bottom(rect), int64_t(y) + height) - min_y)};
}

void Graphics_Driver::XDestroyRegion(Fl_Region region) { delete static_cast<gui_rect *>(region); }

int Graphics_Driver::clip_box(int x, int y, int width, int height, int &out_x, int &out_y,
			      int &out_width, int &out_height)
{
	const auto clip = intersect(bounds(), absolute(x, y, width, height));
	out_x = clip.w && clip.h ? coord(int64_t(clip.x) - origin_.x) : x;
	out_y = clip.w && clip.h ? coord(int64_t(clip.y) - origin_.y) : y;
	out_width = clip.w;
	out_height = clip.h;
	return out_x != x || out_y != y || out_width != width || out_height != height;
}

int Graphics_Driver::not_clipped(int x, int y, int width, int height)
{
	const auto clip = intersect(bounds(), absolute(x, y, width, height));
	return clip.w > 0 && clip.h > 0;
}

void Graphics_Driver::blend(int x, int y, gui_color color, unsigned alpha, const gui_rect &clip)
{
	if (!alpha || x < clip.x || y < clip.y || x >= right(clip) || y >= bottom(clip))
		return;
	auto &pixel = surface_->pixels[y * surface_->stride + x];
	pixel = source_over(color, pixel, alpha);
}

void Graphics_Driver::point(int x, int y)
{
	rectf(x, y, 1, 1);
}

void Graphics_Driver::rectf(int x, int y, int width, int height)
{
	gui_fill_rect(surface_, intersect(bounds(), absolute(x, y, width, height)), ink());
}

void Graphics_Driver::rect(int x, int y, int width, int height)
{
	if (width <= 0 || height <= 0)
		return;
	rectf(x, y, width, 1);
	rectf(x, coord(int64_t(y) + height - 1), width, 1);
	rectf(x, y, 1, height);
	rectf(coord(int64_t(x) + width - 1), y, 1, height);
}

void Graphics_Driver::line(int x, int y, int end_x, int end_y)
{
	const auto clip = bounds();
	if (!clip.w || !clip.h)
		return;
	const double start_x = double(x) + origin_.x, start_y = double(y) + origin_.y;
	const double dx = double(end_x) - x, dy = double(end_y) - y;
	double first = 0, last = 1;
	const auto edge = [&first, &last](double direction, double distance) {
		if (direction < 0)
			first = std::max(first, distance / direction);
		else if (direction > 0)
			last = std::min(last, distance / direction);
		return direction == 0 ? distance >= 0 : first <= last;
	};
	if (!edge(-dx, start_x - clip.x) || !edge(dx, right(clip) - 1 - start_x) ||
	    !edge(-dy, start_y - clip.y) || !edge(dy, bottom(clip) - 1 - start_y))
		return;
	x = path_coord(start_x + first * dx);
	y = path_coord(start_y + first * dy);
	end_x = path_coord(start_x + last * dx);
	end_y = path_coord(start_y + last * dy);
	const int horizontal = std::abs(end_x - x), vertical = -std::abs(end_y - y);
	const int step_x = x < end_x ? 1 : -1, step_y = y < end_y ? 1 : -1;
	const gui_color color = ink();
	for (int error = horizontal + vertical;;) {
		blend(x, y, color, 255, clip);
		if (x == end_x && y == end_y)
			break;
		const int twice_error = 2 * error;
		if (twice_error >= vertical) { error += vertical; x += step_x; }
		if (twice_error <= horizontal) { error += horizontal; y += step_y; }
	}
}

void Graphics_Driver::end_points()
{
	for (int vertex = 0; vertex < n; ++vertex)
		point(path_coord(xpoint[vertex].x), path_coord(xpoint[vertex].y));
}

void Graphics_Driver::end_line()
{
	if (n == 1)
		end_points();
	for (int vertex = 1; vertex < n; ++vertex)
		line(path_coord(xpoint[vertex - 1].x), path_coord(xpoint[vertex - 1].y),
		     path_coord(xpoint[vertex].x), path_coord(xpoint[vertex].y));
}

void Graphics_Driver::begin_complex_polygon()
{
	Fl_Graphics_Driver::begin_complex_polygon();
	contours_.clear();
}

void Graphics_Driver::gap()
{
	if (contours_.empty() || contours_.back() != n)
		contours_.push_back(n);
}

void Graphics_Driver::fill_path(const std::vector<int> &contours)
{
	const auto clip = bounds();
	if (!clip.w || !clip.h || n < 3)
		return;
	float min_y = xpoint[0].y, max_y = min_y;
	for (int vertex = 1; vertex < n; ++vertex) {
		min_y = std::min(min_y, xpoint[vertex].y);
		max_y = std::max(max_y, xpoint[vertex].y);
	}
	const int start = std::max(clip.y, path_coord(std::ceil(min_y + origin_.y - .5)));
	const int end = std::min<int>(bottom(clip), path_coord(std::ceil(max_y + origin_.y - .5)));
	const auto color = ink();
	std::vector<double> crossings;
	for (int row = start; row < end; ++row) {
		crossings.clear();
		const double scan = row + .5 - origin_.y;
		int begin = 0;
		for (int contour : contours) {
			if (contour - begin >= 3) {
				for (int next = begin, prev = contour - 1; next < contour; prev = next++) {
					const auto &from = xpoint[prev], &to = xpoint[next];
					if ((from.y > scan) != (to.y > scan))
						crossings.push_back(from.x + origin_.x +
							(scan - from.y) / (to.y - from.y) * (to.x - from.x));
				}
			}
			begin = contour;
		}
		std::sort(crossings.begin(), crossings.end());
		for (size_t edge = 1; edge < crossings.size(); edge += 2) {
			const int left = std::max(clip.x, path_coord(std::ceil(crossings[edge - 1] - .5)));
			const int right_edge = std::min<int>(right(clip),
							    path_coord(std::ceil(crossings[edge] - .5)));
			if (left < right_edge)
				gui_fill_rect(surface_, {left, row, right_edge - left, 1}, color);
		}
	}
}

void Graphics_Driver::end_polygon() { fill_path({n}); }

void Graphics_Driver::end_complex_polygon()
{
	gap();
	fill_path(contours_);
}

void Graphics_Driver::polygon(int x0, int y0, int x1, int y1, int x2, int y2)
{
	begin_polygon();
	transformed_vertex(x0, y0);
	transformed_vertex(x1, y1);
	transformed_vertex(x2, y2);
	end_polygon();
}

void Graphics_Driver::polygon(int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3)
{
	begin_polygon();
	transformed_vertex(x0, y0);
	transformed_vertex(x1, y1);
	transformed_vertex(x2, y2);
	transformed_vertex(x3, y3);
	end_polygon();
}

void Graphics_Driver::ellipse_path(int x, int y, int width, int height, double start, double end)
{
	if (width <= 0 || height <= 0 || !std::isfinite(start) || !std::isfinite(end))
		return;
	const double rx = (width - 1) / 2., ry = (height - 1) / 2.;
	const double span = std::clamp(end - start, -360., 360.);
	const int segments = std::clamp(int(std::ceil(std::abs(span) / 8)), 1, 45);
	for (int step = 0; step <= segments; ++step) {
		const double theta = (std::fmod(start, 360.) + span * step / segments) * radians;
		transformed_vertex(x + rx + rx * std::cos(theta), y + ry - ry * std::sin(theta));
	}
}

void Graphics_Driver::arc(int x, int y, int width, int height, double start, double end)
{
	begin_line();
	ellipse_path(x, y, width, height, start, end);
	end_line();
}

void Graphics_Driver::pie(int x, int y, int width, int height, double start, double end)
{
	begin_polygon();
	if (std::abs(end - start) < 360.)
		transformed_vertex(x + (width - 1.) / 2, y + (height - 1.) / 2);
	ellipse_path(x, y, width, height, start, end);
	end_polygon();
}

void Graphics_Driver::circle(double x, double y, double radius)
{
	const auto circle_shape = what;
	if (circle_shape == POLYGON || circle_shape == COMPLEX_POLYGON)
		Fl_Graphics_Driver::arc(x, y, radius, 0., 360.);
	else {
		begin_loop();
		Fl_Graphics_Driver::arc(x, y, radius, 0., 360.);
		end_loop();
	}
}

void Graphics_Driver::font(Fl_Font face, Fl_Fontsize size)
{
	font_ = face;
	size_ = size;
}

gui_font *Graphics_Driver::font_for_size()
{
	static std::array<Font_Entry, 12> entries;
	static size_t victim = 0;
	static const char *const paths[] = {
		"/usr/share/fonts/DejaVuSans.ttf",
		"/usr/share/fonts/DejaVuSans-Bold.ttf",
		"/usr/share/fonts/DejaVuSansMono.ttf",
		"/usr/share/fonts/DejaVuSansMono-Bold.ttf",
	};
	const bool mono = (font_ >= FL_COURIER && font_ <= FL_COURIER_BOLD_ITALIC) ||
			  font_ == FL_SCREEN || font_ == FL_SCREEN_BOLD;
	const bool bold = (font_ >= 0 && font_ <= FL_TIMES_BOLD_ITALIC && (font_ & 1)) ||
			  font_ == FL_SCREEN_BOLD;
	const int family = 2 * mono + bold;
	const int size = std::clamp(size_ > 0 ? int(size_) : 14, 1, 128);
	for (auto &entry : entries)
		if (entry.family == family && entry.size == size)
			return entry.font.get();
	auto &entry = entries[victim++ % entries.size()];
	entry.font.reset(gui_font_load(paths[family], size));
	entry.family = family;
	entry.size = size;
	return entry.font.get();
}

gui_font_metrics Graphics_Driver::metrics()
{
	gui_font_metrics result{11, -3, 0, 0, 0};
	gui_font_get_metrics(font_for_size(), &result);
	return result;
}

int Graphics_Driver::height()
{
	auto font_metrics = metrics();
	return font_metrics.ascent - font_metrics.descent + font_metrics.line_gap;
}

int Graphics_Driver::descent() { return -metrics().descent; }

double Graphics_Driver::width(unsigned int codepoint)
{
	gui_font_glyph glyph{};
	return lookup(font_for_size(), codepoint, glyph) ? glyph.advance : 8;
}

double Graphics_Driver::width(const char *text, int count)
{
	double result = 0;
	auto *selected = font_for_size();
	text_codepoints(text, count, [&](unsigned codepoint) {
		gui_font_glyph glyph{};
		result += lookup(selected, codepoint, glyph) ? glyph.advance : 8;
	});
	return result;
}

void Graphics_Driver::draw(const char *text, int count, int x, int y)
{
	const auto clip = bounds();
	if (!clip.w || !clip.h)
		return;
	auto *selected = font_for_size();
	const auto color = ink();
	const int baseline = coord(int64_t(y) + origin_.y);
	int64_t cursor = int64_t(x) + origin_.x;
	text_codepoints(text, count, [&](unsigned codepoint) {
		gui_font_glyph glyph{};
		if (lookup(selected, codepoint, glyph)) {
			const auto area = intersect(clip, {coord(cursor + glyph.x_offset),
				coord(int64_t(baseline) - glyph.y_offset), glyph.width, glyph.height});
			for (int row = area.y; row < bottom(area); ++row)
				for (int column = area.x; column < right(area); ++column) {
					const auto offset = (int64_t(row) - baseline + glyph.y_offset) *
						glyph.stride + column - cursor - glyph.x_offset;
					blend(column, row, color, glyph.alpha[offset], clip);
				}
			cursor += glyph.advance;
		} else {
			cursor += 8;
		}
	});
}

void Graphics_Driver::image_rows(const uchar *bytes, Fl_Draw_Image_Cb callback, void *context,
				int x, int y, int width, int height, int depth, int stride, bool mono)
{
	if (depth == INT32_MIN || (!bytes && !callback))
		return;
	const bool alpha = (std::abs(depth) & FL_IMAGE_WITH_ALPHA) != 0;
	const int channels = std::abs(depth) & ~FL_IMAGE_WITH_ALPHA;
	if (channels < 1 || channels > 4 || (alpha && channels != 2 && channels != 4))
		return;
	const int delta = depth < 0 && !callback ? -channels : channels;
	const auto image = absolute(x, y, width, height);
	const auto clip = intersect(bounds(), image);
	if (!clip.w || !clip.h)
		return;
	if (!stride) {
		const int64_t row_bytes = int64_t(width) * channels;
		if (row_bytes > INT32_MAX)
			return;
		stride = static_cast<int>(row_bytes);
	}
	std::vector<uchar> scanline(callback ? clip.w * channels : 0);
	for (int row = clip.y; row < bottom(clip); ++row) {
		const int src_x = clip.x - image.x, src_y = row - image.y;
		const uchar *pixel;
		if (callback) {
			callback(context, src_x, src_y, clip.w, scanline.data());
			pixel = scanline.data();
		} else {
			const int64_t offset = int64_t(src_x) * delta + int64_t(src_y) * stride;
			if (offset < PTRDIFF_MIN || offset > PTRDIFF_MAX)
				return;
			pixel = bytes + offset;
		}
		for (int column = clip.x; column < right(clip); ++column, pixel += delta) {
			const unsigned red = pixel[0];
			const unsigned green = mono || channels < 3 ? red : pixel[1];
			const unsigned blue = mono || channels < 3 ? red : pixel[2];
			blend(column, row, (red << 16) | (green << 8) | blue,
			      alpha ? pixel[channels - 1] : 255, clip);
		}
	}
}

void Graphics_Driver::draw_image(const uchar *bytes, int x, int y, int width, int height,
				int depth, int stride)
{
	image_rows(bytes, nullptr, nullptr, x, y, width, height, depth, stride, false);
}

void Graphics_Driver::draw_image(Fl_Draw_Image_Cb callback, void *context, int x, int y,
				int width, int height, int depth)
{
	image_rows(nullptr, callback, context, x, y, width, height, depth, 0, false);
}

void Graphics_Driver::draw_image_mono(const uchar *bytes, int x, int y, int width, int height,
				     int depth, int stride)
{
	image_rows(bytes, nullptr, nullptr, x, y, width, height, depth, stride, true);
}

void Graphics_Driver::draw_image_mono(Fl_Draw_Image_Cb callback, void *context, int x, int y,
				     int width, int height, int depth)
{
	image_rows(nullptr, callback, context, x, y, width, height, depth, 0, true);
}

void Graphics_Driver::draw_rgb(Fl_RGB_Image *image, int x, int y, int width, int height,
			      int source_x, int source_y)
{
	if (!image->array || image->d() < 1 || image->d() > 4) {
		draw_empty(image, x, y);
		return;
	}
	if (image->w() != image->data_w() || image->h() != image->data_h()) {
		if (image->w() <= 0 || image->h() <= 0 ||
		    int64_t(image->w()) * image->h() > pixel_limit)
			return;
		std::unique_ptr<Fl_RGB_Image> scaled(static_cast<Fl_RGB_Image *>(
			image->Fl_RGB_Image::copy(image->w(), image->h())));
		if (scaled)
			draw_rgb(scaled.get(), x, y, width, height, source_x, source_y);
		return;
	}
	int draw_x, draw_y, draw_width, draw_height;
	if (start_image(image, x, y, width, height, source_x, source_y,
			draw_x, draw_y, draw_width, draw_height))
		return;
	const int depth = image->d(), stride = image->ld() ? image->ld() : image->data_w() * depth;
	draw_image(image->array + size_t(source_y) * stride + source_x * depth,
		   draw_x, draw_y, draw_width, draw_height,
		   depth % 2 ? depth : depth | FL_IMAGE_WITH_ALPHA, stride);
}

void Graphics_Driver::draw_bitmap(Fl_Bitmap *image, int x, int y, int width, int height,
				 int source_x, int source_y)
{
	int draw_x, draw_y, draw_width, draw_height;
	if (!image->array || !image->data_w() || !image->data_h() ||
	    start_image(image, x, y, width, height, source_x, source_y,
			draw_x, draw_y, draw_width, draw_height))
		return;
	const auto draw_area = absolute(draw_x, draw_y, draw_width, draw_height);
	const auto clip = bounds();
	const auto color = ink();
	const int stride = (image->data_w() + 7) / 8;
	for (int row = 0; row < draw_height; ++row) {
		const auto src_y = int64_t(source_y + row) * image->data_h() / image->h();
		for (int col = 0; col < draw_width; ++col) {
			const auto src_x = int64_t(source_x + col) * image->data_w() / image->w();
			if (image->array[src_y * stride + src_x / 8] & (1 << (src_x % 8)))
				blend(draw_area.x + col, draw_area.y + row, color, 255, clip);
		}
	}
}

void Graphics_Driver::draw_pixmap(Fl_Pixmap *image, int x, int y, int width, int height,
				 int source_x, int source_y)
{
	const int64_t pixels = int64_t(image->data_w()) * image->data_h();
	if (pixels <= 0 || pixels > pixel_limit)
		return;
	std::vector<uchar> rgba(pixels * 4);
	if (fl_convert_pixmap(image->data(), rgba.data(), FL_BLACK)) {
		Fl_RGB_Image rgb(rgba.data(), image->data_w(), image->data_h(), 4);
		rgb.scale(image->w(), image->h(), 0, 1);
		draw_rgb(&rgb, x, y, width, height, source_x, source_y);
	}
}

void Graphics_Driver::copy_offscreen(int x, int y, int width, int height, Fl_Offscreen offscreen,
				    int source_x, int source_y)
{
	const auto *source = reinterpret_cast<const gui_surface *>(offscreen);
	if (!source || !surface_)
		return;
	const auto src = intersect({0, 0, source->width, source->height},
				   {source_x, source_y, width, height});
	const auto dst = absolute(coord(int64_t(x) + src.x - source_x),
				  coord(int64_t(y) + src.y - source_y), src.w, src.h);
	const auto clip = intersect(bounds(), dst);
	if (!clip.w || !clip.h)
		return;
	const gui_rect from{src.x + (clip.x - dst.x), src.y + (clip.y - dst.y), clip.w, clip.h};
	gui_blit(surface_, clip.x, clip.y, source, &from);
}

Fl_RGB_Image *Graphics_Driver::snapshot() const
{
	if (!surface_)
		return nullptr;
	const int width = surface_->width, height = surface_->height;
	auto bytes = std::make_unique<uchar[]>(size_t(width) * height * 3);
	auto *output = bytes.get();
	for (int row = 0; row < height; ++row) {
		const auto *pixels = surface_->pixels + row * surface_->stride;
		for (int column = 0; column < width; ++column) {
			const auto color = pixels[column];
			*output++ = (color >> 16) & 255;
			*output++ = (color >> 8) & 255;
			*output++ = color & 255;
		}
	}
	auto *image = new Fl_RGB_Image(bytes.release(), width, height);
	image->alloc_array = 1;
	return image;
}

}
