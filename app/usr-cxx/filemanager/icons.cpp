#include "icons.h"
#include "browser.h"

#include <font-icons.h>

#include <array>
#include <initializer_list>
#include <memory>

namespace files {
namespace {

constexpr uint32_t ink = 0x35465b, blue = 0x24588e, violet = 0x654294;
constexpr uint32_t gold = 0x936014, teal = 0x22664f, red = 0xa43639;

struct Glyph {
	Icon id;
	uint32_t codepoint, color;
};

constexpr Glyph glyphs[] = {
	{Icon::folder, 0xf07b, gold}, {Icon::file, 0xf15b, ink},
	{Icon::text, 0xf15c, blue}, {Icon::image, 0xf1c5, teal},
	{Icon::archive, 0xf1c6, gold}, {Icon::audio, 0xf1c7, violet},
	{Icon::video, 0xf1c8, violet}, {Icon::code, 0xf1c9, blue},
	{Icon::executable, 0xf120, teal}, {Icon::link, 0xf0c1, blue},
	{Icon::broken_link, 0xf127, red}, {Icon::unavailable, 0xf071, red},
	{Icon::special, 0xf013, ink}, {Icon::home, 0xf015, blue},
	{Icon::bookmark, 0xf02e, gold},
	{Icon::back, 0xf060, ink}, {Icon::forward, 0xf061, ink},
	{Icon::up, 0xf062, ink}, {Icon::split, 0xf0db, ink},
};

auto load_icons()
{
	std::array<std::unique_ptr<Fl_RGB_Image>, static_cast<std::size_t>(Icon::count)> images;
	mario::IconFont font;
	for (const auto &spec : glyphs)
		images[static_cast<std::size_t>(spec.id)] = font.render(spec.codepoint, spec.color);
	return images;
}

bool matches(std::string_view suffix, std::initializer_list<std::string_view> choices)
{
	for (auto choice : choices) {
		if (choice.size() != suffix.size())
			continue;
		std::size_t index = 0;
		for (; index < suffix.size(); ++index) {
			const char ch = suffix[index];
			if ((ch >= 'A' && ch <= 'Z' ? ch + 'a' - 'A' : ch) != choice[index])
				break;
		}
		if (index == suffix.size())
			return true;
	}
	return false;
}

}

Fl_RGB_Image *icon(Icon id)
{
	static const auto images = load_icons();
	return images[static_cast<std::size_t>(id)].get();
}

Icon entry_icon(const Entry &entry)
{
	if (entry.error)
		return Icon::unavailable;
	const auto mode = entry.info.st_mode;
	if (S_ISDIR(mode))
		return Icon::folder;
	if (S_ISLNK(mode))
		return entry.broken ? Icon::broken_link : Icon::link;
	if (!S_ISREG(mode))
		return Icon::special;
	const auto dot = entry.name.rfind('.');
	const auto suffix = dot == std::string::npos || !dot ? std::string_view{} : std::string_view(entry.name).substr(dot + 1);
	if (matches(suffix, {"bmp", "png", "jpg", "jpeg", "gif", "webp", "svg", "tif", "tiff", "ico"}))
		return Icon::image;
	if (matches(suffix, {"zip", "tar", "gz", "xz", "bz2", "zst", "7z", "rar"}))
		return Icon::archive;
	if (matches(suffix, {"wav", "mp3", "flac", "ogg", "opus", "m4a", "aac"}))
		return Icon::audio;
	if (matches(suffix, {"mp4", "mkv", "mov", "avi", "webm", "m4v", "mpeg"}))
		return Icon::video;
	if (matches(suffix, {"c", "h", "cc", "cpp", "hpp", "py", "sh", "rs", "go", "js", "lua", "json", "mk"}))
		return Icon::code;
	if (matches(suffix, {"txt", "log", "md", "conf", "ini", "csv"}))
		return Icon::text;
	if (matches(suffix, {"exe", "elf"}) || (mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
		return Icon::executable;
	return Icon::file;
}

}
