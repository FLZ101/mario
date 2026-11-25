#ifndef MARIO_FLTK_WINDOW_H
#define MARIO_FLTK_WINDOW_H

#include "Fl_Window_Driver.H"
#include "graphics.h"

namespace mario_fltk {

class Window_Peer;

class Window_Driver final : public Fl_Window_Driver {
public:
	explicit Window_Driver(Fl_Window *window);
	~Window_Driver() override;
	void show() override;
	void hide() override;
	void label(const char *name, const char *icon) override;
	void make_current() override;
	void flush() override;
	void resize(int x, int y, int width, int height) override;
	fl_uintptr_t os_id() override;
	void menu_window_area(int &x, int &y, int &width, int &height, int screen) override;

private:
	friend class Window_Peer;
	std::shared_ptr<Window_Peer> peer_;
	Window_Driver *popup_owner_ = nullptr;
	Pixels popup_pixels_;
	std::vector<Window_Driver *> popups_;
	void composite_popups(gui_surface *surface, const gui_rect &damage);
	void damage_popups();
	void cancel_menu();
	void configured(uint32_t window_id, int width, int height);
	void close_requested();
	void disconnected();
};

}

#endif
