#include <FL/Fl_Copy_Surface.H>
#include <FL/Fl_Graphics_Driver.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/platform.H>
#include "Fl_Timeout.h"
#include "Fl_Window_Driver.H"
#include "drivers/Unix/Fl_Unix_Screen_Driver.H"
#include "drivers/Unix/Fl_Unix_System_Driver.H"
#include "graphics.h"
#include "window.h"

#include <algorithm>
#include <climits>
#include <ctime>
#include <poll.h>
#include <vector>

extern void fl_fix_focus();
extern void (*fl_lock_function)();
extern void (*fl_unlock_function)();

namespace {

using mario_fltk::Graphics_Driver;
using mario_fltk::Pixels;

class Mario_Screen_Driver final : public Fl_Unix_Screen_Driver {
public:
	int compose(int &removed) override
	{
		removed = 0;
		return !(Fl::event_state() & (FL_CTRL | FL_ALT | FL_META)) && Fl::event_length();
	}
	void grab(Fl_Window *window) override
	{
		Fl::grab_ = window;
		if (!window)
			fl_fix_focus();
	}
};

class Mario_System_Driver final : public Fl_Unix_System_Driver {
	struct Watch {
		pollfd descriptor;
		Fl_FD_Handler callback;
		void *argument;
		uint64_t serial;
	};
	std::vector<Watch> watches_;
	uint64_t serial_ = 0;

	int poll_watches(double seconds, bool dispatch)
	{
		const auto snapshot = watches_;
		std::vector<pollfd> descriptors;
		descriptors.reserve(snapshot.size());
		for (const auto &watch : snapshot)
			descriptors.push_back(watch.descriptor);
		const int timeout = seconds >= INT_MAX / 1000.0 ? -1 :
			static_cast<int>(std::max(0.0, seconds) * 1000 + .5);
		if (dispatch)
			fl_unlock_function();
		const int result = poll(descriptors.data(), descriptors.size(), timeout);
		if (dispatch)
			fl_lock_function();
		if (result > 0 && dispatch) {
			for (size_t index = 0; index < snapshot.size(); ++index) {
				const auto &watch = snapshot[index];
				if (descriptors[index].revents &&
				    std::any_of(watches_.begin(), watches_.end(), [&](const Watch &current) {
					    return current.serial == watch.serial;
				    }))
					watch.callback(watch.descriptor.fd, watch.argument);
			}
		}
		return result;
	}

public:
	void add_fd(int descriptor, int events, Fl_FD_Handler callback, void *argument) override
	{
		remove_fd(descriptor, events);
		watches_.push_back({{descriptor, static_cast<short>(events), 0}, callback, argument, ++serial_});
	}
	void add_fd(int descriptor, Fl_FD_Handler callback, void *argument) override
	{
		add_fd(descriptor, FL_READ, callback, argument);
	}
	void remove_fd(int descriptor, int events) override
	{
		for (auto &watch : watches_) {
			if (watch.descriptor.fd == descriptor && (watch.descriptor.events & events)) {
				watch.descriptor.events &= ~events;
				watch.serial = ++serial_;
			}
		}
		watches_.erase(std::remove_if(watches_.begin(), watches_.end(), [](const Watch &watch) {
			return !watch.descriptor.events;
		}), watches_.end());
	}
	void remove_fd(int descriptor) override { remove_fd(descriptor, -1); }
	void gettime(time_t *seconds, int *microseconds) override
	{
		timespec now{};
		clock_gettime(CLOCK_MONOTONIC, &now);
		*seconds = now.tv_sec;
		*microseconds = now.tv_nsec / 1000;
	}
	int ready() override
	{
		Fl_Timeout::elapse_timeouts();
		return Fl_Timeout::time_to_wait(1) <= 0 ? 1 : poll_watches(0, false);
	}
	double wait(double seconds) override
	{
		const bool had_window = Fl::first_window() != nullptr;
		seconds = Fl_System_Driver::wait(seconds);
		if (had_window && !Fl::first_window())
			return 0;
		if (seconds <= 0) {
			const int result = poll_watches(0, true);
			Fl::flush();
			return result;
		}
		Fl::flush();
		Fl_Timeout::elapse_timeouts();
		return poll_watches(Fl::idle ? 0 : Fl_Timeout::time_to_wait(seconds), true);
	}
};

class Mario_Image_Surface_Driver final : public Fl_Image_Surface_Driver {
	Graphics_Driver graphics_;
	Pixels pixels_;

public:
	Mario_Image_Surface_Driver(int width, int height, int high_res, Fl_Offscreen external)
		: Fl_Image_Surface_Driver(width, height, high_res, external),
		  pixels_(external ? Pixels{} : mario_fltk::make_pixels(width, height))
	{
		auto *surface = external ? reinterpret_cast<gui_surface *>(external) : pixels_.get();
		offscreen = reinterpret_cast<Fl_Offscreen>(surface);
		graphics_.target(surface);
		driver(&graphics_);
	}
	void set_current() override { Fl_Surface_Device::set_current(); }
	using Fl_Widget_Surface::origin;
	void origin(int x, int y) override
	{
		Fl_Widget_Surface::origin(x, y);
		graphics_.set_origin(x, y);
	}
	void translate(int x, int y) override { graphics_.push_origin(x, y); }
	void untranslate() override { graphics_.pop_origin(); }
	Fl_RGB_Image *image() override { return graphics_.snapshot(); }
};

class Mario_Copy_Surface_Driver final : public Fl_Copy_Surface_Driver {
	Graphics_Driver graphics_;
	Pixels pixels_;

public:
	Mario_Copy_Surface_Driver(int width, int height) : Fl_Copy_Surface_Driver(width, height),
		pixels_(mario_fltk::make_pixels(width, height))
	{
		graphics_.target(pixels_.get());
		driver(&graphics_);
	}
	void set_current() override { Fl_Surface_Device::set_current(); }
	void translate(int x, int y) override { graphics_.push_origin(x, y); }
	void untranslate() override { graphics_.pop_origin(); }
};

}

Window fl_window = 0;

Fl_Graphics_Driver *Fl_Graphics_Driver::newMainGraphicsDriver()
{
	return new Graphics_Driver;
}

Fl_Screen_Driver *Fl_Screen_Driver::newScreenDriver()
{
	return new Mario_Screen_Driver;
}

Fl_System_Driver *Fl_System_Driver::newSystemDriver()
{
	return new Mario_System_Driver;
}

Fl_Window_Driver *Fl_Window_Driver::newWindowDriver(Fl_Window *window)
{
	return new mario_fltk::Window_Driver(window);
}

Fl_Image_Surface_Driver *Fl_Image_Surface_Driver::newImageSurfaceDriver(
	int width, int height, int high_res, Fl_Offscreen offscreen)
{
	return new Mario_Image_Surface_Driver(width, height, high_res, offscreen);
}

Fl_Copy_Surface_Driver *Fl_Copy_Surface_Driver::newCopySurfaceDriver(int width, int height)
{
	return new Mario_Copy_Surface_Driver(width, height);
}

Fl_Native_File_Chooser::Fl_Native_File_Chooser(int type)
	: platform_fnfc(new Fl_Native_File_Chooser_FLTK_Driver(type))
{
}
