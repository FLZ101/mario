#ifndef MARIO_FILEMANAGER_PREVIEW_H
#define MARIO_FILEMANAGER_PREVIEW_H

#include "browser.h"

#include <list>

namespace files {

struct Preview {
	std::string text;
	std::vector<unsigned char> rgb;
	int width = 0, height = 0;
};

bool image_name(std::string_view name);

class PreviewCache {
public:
	Preview read(const std::string &path);
	void clear() { items_.clear(); }
private:
	struct Item {
		std::string path;
		struct stat info;
		Preview preview;
	};
	std::list<Item> items_;
};

}

#endif
