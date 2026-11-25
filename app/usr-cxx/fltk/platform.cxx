#include <FL/Fl_Copy_Surface.H>
#include <FL/Fl_Graphics_Driver.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/platform.H>
#include "Fl_Window_Driver.H"
#include "drivers/Unix/Fl_Unix_Screen_Driver.H"
#include "drivers/Unix/Fl_Unix_System_Driver.H"

namespace {

class Mario_Graphics_Driver final : public Fl_Graphics_Driver {};

class Mario_Screen_Driver final : public Fl_Unix_Screen_Driver {};

class Mario_System_Driver final : public Fl_Unix_System_Driver {};

class Mario_Window_Driver final : public Fl_Window_Driver {
public:
	explicit Mario_Window_Driver(Fl_Window *window) : Fl_Window_Driver(window) {}
};

class Mario_Image_Surface_Driver final : public Fl_Image_Surface_Driver {
public:
	Mario_Image_Surface_Driver(int width, int height, int high_res, Fl_Offscreen offscreen)
		: Fl_Image_Surface_Driver(width, height, high_res, offscreen)
	{
		driver(new Mario_Graphics_Driver);
	}
	~Mario_Image_Surface_Driver() override { delete driver(); }
	void set_current() override { Fl_Surface_Device::set_current(); }
	void translate(int, int) override {}
	void untranslate() override {}
	Fl_RGB_Image *image() override { return nullptr; }
};

class Mario_Copy_Surface_Driver final : public Fl_Copy_Surface_Driver {
public:
	Mario_Copy_Surface_Driver(int width, int height) : Fl_Copy_Surface_Driver(width, height)
	{
		driver(new Mario_Graphics_Driver);
	}
	~Mario_Copy_Surface_Driver() override { delete driver(); }
	void set_current() override { Fl_Surface_Device::set_current(); }
	void translate(int, int) override {}
	void untranslate() override {}
};

}

Window fl_window = 0;

Fl_Graphics_Driver *Fl_Graphics_Driver::newMainGraphicsDriver()
{
	return new Mario_Graphics_Driver;
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
	return new Mario_Window_Driver(window);
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
