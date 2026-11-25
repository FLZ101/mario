#ifndef MARIO_FONT_ICONS_H
#define MARIO_FONT_ICONS_H

#include <FL/Fl_RGB_Image.H>

#include <cstdint>
#include <memory>

class Fl_Widget;
struct gui_font;

namespace mario {

inline constexpr char nerd_icon_font[] = "/usr/share/fonts/DejaVuSansMNerdFontMono-Regular.ttf";

class IconFont {
public:
	IconFont(const char *path = nerd_icon_font, int pixel_height = 17, int cell_size = 14);
	std::unique_ptr<Fl_RGB_Image> render(std::uint32_t codepoint, std::uint32_t rgb) const;

private:
	std::unique_ptr<gui_font, void (*)(gui_font *)> font_;
	int cell_size_;
};

void set_label_icon(Fl_Widget &widget, Fl_Image *image);

}

#endif
