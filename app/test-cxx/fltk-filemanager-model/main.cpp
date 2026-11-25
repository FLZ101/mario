#include "browser.h"
#include "preview.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

void check(bool success, const char *expectation)
{
	if (!success) {
		std::fprintf(stderr, "filemanager model: %s (errno %d)\n", expectation, errno);
		std::exit(1);
	}
}

void create(const std::string &path, std::string_view data)
{
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	check(fd >= 0, path.c_str());
	check(write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()), "write file");
	check(close(fd) == 0, "close file");
}

void finish(files::Scan &scan)
{
	while (scan.pending())
		scan.step(13);
	check(!scan.error, "complete listing");
}

void verify(const std::string &directory, bool vfat)
{
	files::Scan scan;
	scan.start(directory);
	scan.step(1);
	check(scan.pending() && scan.entries.size() <= 1, "bounded batch");
	finish(scan);
	check(!scan.limited, "small listing complete");
	const auto names = [&](auto rows) {
		std::vector<std::string> result;
		for (auto row : rows) {
			auto name = scan.entries[row].name;
			if (vfat)
				for (auto &byte : name)
					if (byte >= 'A' && byte <= 'Z')
						byte += 'a' - 'A';
			result.push_back(std::move(name));
		}
		return result;
	};
	using files::Column;
	const auto ascending = names(files::order(scan.entries, ".txt", false, Column::name, false));
	const std::string spaced = vfat ? "alpha file.txt" : "Alpha File.txt";
	check(ascending == std::vector<std::string>{spaced, "zeta.txt"}, "name ordering and hidden filter");
	check(names(files::order(scan.entries, ".TXT", true, Column::name, true)) ==
		std::vector<std::string>{"zeta.txt", spaced, ".hidden.txt"}, "case search and descending ordering");
	check(names(files::order(scan.entries, ".txt", false, Column::size, false)) ==
		std::vector<std::string>{"zeta.txt", spaced}, "numeric size ordering");
	const std::string invalid = "raw\xff.bin";
	check(files::display(invalid) == "raw\\xff.bin" && files::display("caf\xc3\xa9") == "caf\xc3\xa9", "UTF-8 display encoding");
	if (!vfat) {
		const auto row = std::find_if(scan.entries.begin(), scan.entries.end(), [&](const auto &entry) {
			return entry.name == invalid && entry.label == "raw\\xff.bin";
		});
		check(row != scan.entries.end(), "raw filename survives enumeration");
		std::string resolved;
		check(files::directory_path(files::join(directory, "alias/../"), false, resolved) == ELOOP, "links require consent before traversal");
		check(files::directory_path(files::join(directory, "alias"), true, resolved) == 0 && resolved == files::join(directory, "folder"),
			"explicit link navigation");
		check(files::inspect(directory, "broken").broken, "broken link visible");
	}
	files::PreviewCache preview;
	check(preview.read(files::join(directory, "Alpha File.txt")).text == "abcdefgh\n", "spaced filename text preview");
	if (!vfat)
		check(preview.read(files::join(directory, "broken")).rgb.empty(), "links not decoded");
	scan.start(directory);
	scan.cancel();
	scan.start(files::join(directory, "folder"));
	finish(scan);
	check(scan.entries.empty(), "cancelled scan does not publish stale entries");
}

}

int main(int argc, char **argv)
{
	check(argc == 3, "directory and mode arguments");
	const std::string directory = argv[1], mode = argv[2];
	const bool vfat = mode == "vfat" || mode == "vfat-ro";
	const auto vanished = files::join(directory, "vanished.tmp");
	if (mode != "vfat-ro" && mode != "ext2-ro") {
		check(mkdir(directory.c_str(), 0700) == 0, "create fixture directory");
		check(mkdir(files::join(directory, "folder").c_str(), 0700) == 0, "create child directory");
		create(files::join(directory, "Alpha File.txt"), "abcdefgh\n");
		create(files::join(directory, "zeta.txt"), "z");
		create(files::join(directory, ".hidden.txt"), "hidden");
		create(vanished, "temporary");
		if (!vfat) {
			create(files::join(directory, "raw\xff.bin"), "raw bytes");
			check(symlink("folder", files::join(directory, "alias").c_str()) == 0, "create directory symlink");
			check(symlink("gone", files::join(directory, "broken").c_str()) == 0, "create broken symlink");
		}
		files::Scan before;
		before.start(directory);
		finish(before);
		check(std::any_of(before.entries.begin(), before.entries.end(), [](const auto &entry) {
			return !entry.error && (entry.name == "vanished.tmp" || entry.name == "VANISHED.TMP");
		}), "entry initially appeared in a listing");
		check(unlink(vanished.c_str()) == 0, "remove a listed name before inspecting again");
	}
	check(files::inspect(directory, "vanished.tmp").error == ENOENT, "vanished name is not used as a successful row");
	verify(directory, vfat);
	if (mode == "mariofs") {
		files::Preferences preferences;
		preferences.hidden = preferences.split = true;
		preferences.column = files::Column::modified;
		preferences.bookmarks = {files::join(directory, "some\xff name")};
		check(preferences.save(directory), "save preferences");
		files::Preferences loaded;
		check(loaded.load(directory) && loaded.hidden && loaded.split && loaded.column == preferences.column &&
			loaded.bookmarks == preferences.bookmarks, "preferences round trip preserves raw bookmark bytes");
		check(files::mp4_name("video with spaces.MP4") && files::mp4_name("video.mp4") &&
			!files::mp4_name("video.mp4.txt") && !files::mp4_name("audio.wav"), "mpv file types");
		files::PreviewCache preview;
		const auto bmp = preview.read("/usr/share/mario-tests/gui/checker-64.bmp");
		check(bmp.width == 64 && bmp.height == 64 && bmp.rgb.size() == 64 * 64 * 3, "bounded BMP decode");
		const auto png = preview.read("/usr/share/mario-tests/gui/alpha-128.png");
		check(png.width > 0 && png.width <= 160 && png.height <= 120 && !png.rgb.empty(), "bounded PNG decode");
		const auto large = files::join(directory, "large");
		check(mkdir(large.c_str(), 0700) == 0, "create directory for listing limit");
		for (std::size_t index = 0; index <= files::listing_limit; ++index)
			create(files::join(large, std::to_string(index)), "");
		files::Scan scan;
		scan.start(large);
		finish(scan);
		check(scan.limited && scan.entries.size() == files::listing_limit, "exact 4096 entry limit");
	}
	std::printf("filemanager model: %s passed\n", mode.c_str());
}
