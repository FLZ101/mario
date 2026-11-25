#ifndef MARIO_FILEMANAGER_BROWSER_H
#define MARIO_FILEMANAGER_BROWSER_H

#include <dirent.h>
#include <sys/stat.h>

#include <string>
#include <string_view>
#include <vector>

namespace files {

constexpr std::size_t listing_limit = 4096;

struct Entry {
	std::string name, label;
	struct stat info{};
	int error = 0;
	bool broken = false;
};

enum class Column { name, type, size, modified };

std::string display(std::string_view raw);
std::string join(const std::string &parent, std::string_view name);
std::string parent(const std::string &path);
const char *kind(const Entry &entry);
bool mp4_name(std::string_view name);
Entry inspect(const std::string &directory, const std::string &name);
std::vector<std::size_t> order(const std::vector<Entry> &entries, std::string_view filter,
			       bool hidden, Column column, bool descending);

int directory_path(const std::string &input, bool follow, std::string &resolved);

class Scan {
public:
	Scan() = default;
	~Scan() { cancel(); }
	Scan(const Scan &) = delete;
	Scan &operator=(const Scan &) = delete;
	void start(const std::string &path);
	void cancel();
	void step(std::size_t budget = 24);
	bool pending() const { return directory_ != nullptr; }

	std::vector<Entry> entries;
	int error = 0;
	bool limited = false;

private:
	DIR *directory_ = nullptr;
	std::string path_;
};

struct Preferences {
	bool hidden = false, split = false;
	Column column = Column::name;
	bool descending = false;
	std::vector<std::string> bookmarks;
	bool load(const std::string &home);
	bool save(const std::string &home) const;
};

std::vector<std::string> mounts();

}

#endif
