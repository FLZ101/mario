#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Widget.H>
#include <FL/fl_draw.H>

#include <cstdio>
#include <cstring>
#include <unistd.h>

int run_upload_test();

class Patch : public Fl_Widget {
	bool changed_ = false;

public:
	Patch() : Fl_Widget(16, 16, 32, 32) {}
	void update()
	{
		changed_ = true;
		redraw();
	}
	void draw() override
	{
		fl_color(changed_ ? FL_GREEN : FL_RED);
		fl_rectf(x(), y(), w(), h());
	}
};

struct Fixture {
	Fl_Double_Window window{64, 80, 224, 160, "FLTK Window"};
	Patch patch;
	bool resized = false;
};

static void fixture_commands(void *context)
{
	auto &fixture = *static_cast<Fixture *>(context);
	if (!fixture.resized && !access("/tmp/fltk-window.resize", F_OK)) {
		fixture.resized = true;
		fixture.window.resize(64, 80, 256, 180);
		fixture.window.label("FLTK Window resized");
	}
	if (!access("/tmp/fltk-window.patch", F_OK)) {
		unlink("/tmp/fltk-window.patch");
		fixture.patch.update();
	}
	if (!access("/tmp/fltk-window.close", F_OK)) {
		fixture.window.hide();
		return;
	}
	Fl::repeat_timeout(.03, fixture_commands, context);
}

int main(int argc, char **argv)
{
	if (argc > 1 && std::strcmp(argv[1], "--fake") == 0)
		return run_upload_test();
	Fixture fixture;
	fixture.window.color(fl_rgb_color(32, 64, 96));
	fixture.window.end();
	fixture.window.show();
	if (!fixture.window.shown())
		return 1;
	fixture.window.clear_damage();
	fixture.patch.redraw();
	fixture.window.redraw();
	Fl::add_timeout(.03, fixture_commands, &fixture);
	Fl::run();
	Fl::remove_timeout(fixture_commands, &fixture);
	if (fixture.window.w() != 256 || fixture.window.h() != 180)
		return 2;
	std::puts("fltk-window: configured and closed");
	return 0;
}
