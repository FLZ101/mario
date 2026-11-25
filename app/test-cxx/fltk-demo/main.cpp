#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scroll.H>

#include <cstdio>
#include <cstring>
#include <sys/resource.h>
#include <unistd.h>

namespace {

long long cpu_microseconds()
{
	rusage usage{};
	if (getrusage(RUSAGE_SELF, &usage) < 0)
		return -1;
	return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000LL +
	       usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
}

struct Demo {
	Fl_Double_Window window{480, 80, 300, 220, "FLTK Input Demo"};
	Fl_Button button{16, 16, 104, 30, "Click"};
	Fl_Input input{16, 56, 210, 30};
	Fl_Scroll scroll{16, 104, 210, 92};
	Fl_Box content{16, 104, 170, 450, "Scrollable content"};
	Fl_Box indicator{260, 16, 24, 24};
	int descriptors[2]{};
	int clicks = 0, ticks = 0, reads = 0, closes = 0;
	int observed_scroll = 0;
	long long idle_start = -1, idle_cpu = -1;
	bool observed_press = false, observed_cancel = false;

	Demo()
	{
		content.align(FL_ALIGN_TOP_LEFT | FL_ALIGN_INSIDE);
		scroll.end();
		scroll.type(Fl_Scroll::VERTICAL_ALWAYS);
		window.add(indicator);
		indicator.box(FL_FLAT_BOX);
		indicator.color(FL_RED);
		window.end();
		button.callback(clicked, this);
		input.when(FL_WHEN_CHANGED);
		input.callback(edited, this);
		window.callback(close, this);
	}

	void mark(Fl_Color color)
	{
		indicator.color(color);
		indicator.redraw();
	}

	static void clicked(Fl_Widget *, void *data)
	{
		auto &demo = *static_cast<Demo *>(data);
		++demo.clicks;
		demo.mark(FL_GREEN);
	}

	static void edited(Fl_Widget *, void *data)
	{
		auto &demo = *static_cast<Demo *>(data);
		if (std::strcmp(demo.input.value(), "abC") == 0)
			demo.mark(FL_YELLOW);
	}

	static void tick(void *data)
	{
		auto &demo = *static_cast<Demo *>(data);
		++demo.ticks;
		const char byte = '!';
		if (demo.ticks == 4)
			demo.idle_start = cpu_microseconds();
		if (demo.ticks == 14 && demo.idle_start >= 0) {
			const auto now = cpu_microseconds();
			if (now >= demo.idle_start)
				demo.idle_cpu = now - demo.idle_start;
		}
		if (demo.ticks == 1 || demo.ticks == 14)
			write(demo.descriptors[1], &byte, sizeof(byte));
		if (int position = demo.scroll.yposition(); demo.observed_scroll != position) {
			demo.observed_scroll = position;
			demo.mark(FL_MAGENTA);
		}
		if (demo.clicks == 1 && demo.button.value() && Fl::pushed() == &demo.button) {
			demo.observed_press = true;
			demo.mark(FL_RED);
		}
		if (!demo.observed_cancel && demo.observed_press && Fl::focus() == nullptr &&
		    demo.clicks == 1 && demo.button.value() == 0 && Fl::pushed() == nullptr) {
			demo.observed_cancel = true;
			demo.mark(FL_CYAN);
		}
		Fl::repeat_timeout(.05, tick, data);
	}

	static void available(int descriptor, void *data)
	{
		auto &demo = *static_cast<Demo *>(data);
		char byte;
		if (read(descriptor, &byte, sizeof(byte)) == 1 && byte == '!')
			++demo.reads;
		if (demo.reads == 2 && !demo.clicks)
			demo.mark(FL_BLUE);
	}

	static void close(Fl_Widget *, void *data)
	{
		auto &demo = *static_cast<Demo *>(data);
		if (++demo.closes == 2)
			demo.window.hide();
		else
			demo.mark(FL_WHITE);
	}
};

}

int main()
{
	Demo demo;
	if (pipe(demo.descriptors) < 0)
		return 1;
	Fl::add_fd(demo.descriptors[0], Demo::available, &demo);
	demo.window.show();
	Fl::add_timeout(.05, Demo::tick, &demo);
	const int result = Fl::run();
	Fl::remove_timeout(Demo::tick, &demo);
	Fl::remove_fd(demo.descriptors[0]);
	close(demo.descriptors[0]);
	close(demo.descriptors[1]);
	if (result || demo.clicks != 1 || demo.reads != 2 || demo.closes != 2 ||
	    !demo.observed_scroll || !demo.observed_cancel || demo.idle_cpu < 0 || demo.idle_cpu > 125000 ||
	    std::strcmp(demo.input.value(), "abC") != 0) {
		std::printf("fltk-demo: clicks=%d reads=%d closes=%d scroll=%d cancelled=%d idle_us=%lld input='%s'\n",
			demo.clicks, demo.reads, demo.closes, demo.observed_scroll,
			demo.observed_cancel, demo.idle_cpu, demo.input.value());
		return 2;
	}
	std::puts("fltk-demo: input, reset, timers, fd callbacks and close verified");
}
