#ifndef MARIO_FILEMANAGER_ICONS_H
#define MARIO_FILEMANAGER_ICONS_H

class Fl_RGB_Image;

namespace files {

struct Entry;

enum class Icon {
	folder, file, text, image, archive, audio, video, code, executable, link,
	broken_link, unavailable, special, home, bookmark, drive, back, forward, up, split, count
};

Fl_RGB_Image *icon(Icon id);
Icon entry_icon(const Entry &entry);

}

#endif
