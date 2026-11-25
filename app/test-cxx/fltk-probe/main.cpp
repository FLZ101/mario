#include <FL/Fl_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Window.H>

#include <cstdio>
#include <cstring>

int main()
{
	Fl_Window window(240, 120, "FLTK build probe");
	Fl_Button button(10, 10, 100, 30, "Probe");
	Fl_Input input(10, 50, 100, 30);
	window.end();
	input.value("Mario");
	Fl_Native_File_Chooser chooser(Fl_Native_File_Chooser::BROWSE_FILE);
	if (window.shown() || window.children() != 2 ||
	    std::strcmp(button.label(), "Probe") || std::strcmp(input.value(), "Mario") ||
	    chooser.type() != Fl_Native_File_Chooser::BROWSE_FILE)
		return 1;
	std::puts("fltk-build: constructed stock widgets without a WM");
	return 0;
}
