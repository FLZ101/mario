#include "preview.h"

extern "C" {
#include <gui/image.h>
}

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

namespace files {
namespace {

constexpr unsigned max_dimension = 1024, thumb_width = 160, thumb_height = 120;
constexpr std::size_t max_encoded = 2 * 1024 * 1024, max_text = 4096;
constexpr std::size_t cache_budget = 1024 * 1024;

bool same_file(const struct stat &first, const struct stat &second)
{
	return first.st_dev == second.st_dev && first.st_ino == second.st_ino &&
		first.st_size == second.st_size && first.st_mtim.tv_sec == second.st_mtim.tv_sec &&
		first.st_mtim.tv_nsec == second.st_mtim.tv_nsec && first.st_ctim.tv_sec == second.st_ctim.tv_sec &&
		first.st_ctim.tv_nsec == second.st_ctim.tv_nsec;
}

bool extension(std::string_view name, std::string_view suffix)
{
	if (name.size() < suffix.size())
		return false;
	name.remove_prefix(name.size() - suffix.size());
	for (std::size_t index = 0; index < name.size(); ++index) {
		const char byte = name[index];
		if ((byte >= 'A' && byte <= 'Z' ? byte + 'a' - 'A' : byte) != suffix[index])
			return false;
	}
	return true;
}

uint32_t be32(const unsigned char *bytes)
{
	return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}

Preview thumbnail(const std::vector<unsigned char> &bytes, bool png)
{
	unsigned width, height;
	if (png) {
		if (bytes.size() < 24 || std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) ||
		    be32(bytes.data() + 8) != 13 || std::memcmp(bytes.data() + 12, "IHDR", 4))
			return {"Invalid PNG"};
		width = be32(bytes.data() + 16);
		height = be32(bytes.data() + 20);
	} else {
		gui_bmp_metadata header;
		if (gui_bmp_metadata_parse(bytes.data(), bytes.size(), &header) < 0)
			return {"Invalid or unsupported BMP"};
		width = header.width;
		height = header.height;
	}
	if (!width || !height || width > max_dimension || height > max_dimension)
		return {"Image exceeds preview dimensions"};
	gui_surface *decoded = nullptr;
	const auto decode = png ? gui_png_decode : gui_bmp_decode;
	if (decode(bytes.data(), bytes.size(), &decoded) < 0)
		return {"Invalid or unsupported image"};
	const std::unique_ptr<gui_surface, decltype(&gui_surface_destroy)> surface(decoded, gui_surface_destroy);
	Preview result;
	const auto ratio = std::min({1., double(thumb_width) / width, double(thumb_height) / height});
	result.width = std::max(1, int(width * ratio));
	result.height = std::max(1, int(height * ratio));
	result.rgb.reserve(result.width * result.height * 3);
	for (int row = 0; row < result.height; ++row)
		for (int col = 0; col < result.width; ++col) {
			const auto pixel = surface->pixels[(row * height / result.height) * surface->stride + col * width / result.width];
			result.rgb.push_back(pixel >> 16);
			result.rgb.push_back(pixel >> 8);
			result.rgb.push_back(pixel);
		}
	return result;
}

Preview text_preview(const std::vector<unsigned char> &bytes, bool truncated)
{
	Preview result;
	std::string line;
	for (auto byte : bytes) {
		if (!byte || (byte < 0x20 && byte != '\n' && byte != '\r' && byte != '\t'))
			return {"No preview for binary files"};
		if (byte == '\n') {
			result.text += display(line) + '\n';
			line.clear();
		} else if (byte == '\t')
			line += "    ";
		else if (byte != '\r')
			line += char(byte);
	}
	result.text += display(line);
	if (truncated)
		result.text += "\n[Preview truncated]";
	if (result.text.empty())
		result.text = "Empty file";
	return result;
}

}

bool image_name(std::string_view name)
{
	return extension(name, ".png") || extension(name, ".bmp");
}

Preview PreviewCache::read(const std::string &path)
{
	struct stat info;
	if (lstat(path.c_str(), &info) < 0 || !S_ISREG(info.st_mode))
		return {"Select a regular file to preview"};
	const bool image = image_name(path);
	if (image) {
		for (auto item = items_.begin(); item != items_.end(); ++item)
			if (item->path == path && same_file(item->info, info)) {
				items_.splice(items_.begin(), items_, item);
				return items_.front().preview;
			}
		if (info.st_size > static_cast<off_t>(max_encoded))
			return {"Image exceeds encoded preview limit"};
	}
	const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return {"Cannot open preview"};
	struct stat opened;
	if (fstat(fd, &opened) < 0 || !S_ISREG(opened.st_mode) || !same_file(info, opened)) {
		close(fd);
		return {"File changed; refresh and reselect"};
	}
	std::vector<unsigned char> bytes(image ? max_encoded + 1 : max_text + 1);
	std::size_t used = 0;
	while (used < bytes.size()) {
		const auto count = ::read(fd, bytes.data() + used, bytes.size() - used);
		if (!count)
			break;
		if (count < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return {"Cannot read preview"};
		}
		used += count;
	}
	close(fd);
	if (!image) {
		bytes.resize(std::min(used, max_text));
		return text_preview(bytes, used > max_text);
	}
	if (used > max_encoded)
		return {"Image exceeds encoded preview limit"};
	bytes.resize(used);
	auto result = thumbnail(bytes, extension(path, ".png"));
	if (!result.rgb.empty()) {
		items_.push_front({path, info, result});
		constexpr auto max_items = cache_budget / (thumb_width * thumb_height * 3);
		if (items_.size() > max_items)
			items_.pop_back();
	}
	return result;
}

}
