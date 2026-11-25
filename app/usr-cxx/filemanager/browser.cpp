#include "browser.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace files {
namespace {

std::string fold(std::string_view text)
{
	std::string result(text);
	for (auto &byte : result)
		if (byte >= 'A' && byte <= 'Z')
			byte += 'a' - 'A';
	return result;
}

std::string read_bounded(const std::string &path, std::size_t limit)
{
	std::string text;
	const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return text;
	char buffer[1024];
	while (text.size() < limit) {
		const auto bytes = read(fd, buffer, std::min(sizeof(buffer), limit - text.size()));
		if (!bytes)
			break;
		if (bytes < 0) {
			if (errno == EINTR)
				continue;
			text.clear();
			break;
		}
		text.append(buffer, bytes);
	}
	close(fd);
	return text;
}

std::string hex(std::string_view value)
{
	constexpr char digits[] = "0123456789abcdef";
	std::string encoded;
	for (unsigned char byte : value) {
		encoded += digits[byte >> 4];
		encoded += digits[byte & 15];
	}
	return encoded;
}

std::string unhex(std::string_view value)
{
	constexpr std::string_view digits = "0123456789abcdef";
	std::string decoded;
	if (value.size() % 2 || value.size() >= 2 * PATH_MAX)
		return decoded;
	for (std::size_t offset = 0; offset < value.size(); offset += 2) {
		const auto high = digits.find(value[offset]), low = digits.find(value[offset + 1]);
		if (high == digits.npos || low == digits.npos || !(high || low))
			return {};
		decoded += static_cast<char>(high * 16 + low);
	}
	return decoded;
}

}

std::string display(std::string_view raw)
{
	std::string result;
	while (!raw.empty()) {
		const unsigned char lead = raw.front();
		const unsigned length = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 :
					lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
		bool valid = length && length <= raw.size() && lead >= 0x20 && lead != 0x7f;
		for (unsigned index = 1; valid && index < length; ++index) {
			const unsigned char byte = raw[index];
			valid = byte >= 0x80 && byte <= 0xbf;
			if (index == 1)
				valid = valid && !(lead == 0xe0 && byte < 0xa0) && !(lead == 0xed && byte >= 0xa0) &&
					!(lead == 0xf0 && byte < 0x90) && !(lead == 0xf4 && byte >= 0x90);
		}
		if (valid && lead != '\\')
			result.append(raw.substr(0, length));
		else {
			result += "\\x";
			result += hex(raw.substr(0, 1));
		}
		raw.remove_prefix(valid ? length : 1);
	}
	return result;
}

std::string join(const std::string &directory, std::string_view name)
{
	return directory + (directory == "/" ? "" : "/") + std::string(name);
}

std::string parent(const std::string &path)
{
	return path.substr(0, std::max<std::size_t>(1, path.rfind('/')));
}

const char *kind(const Entry &entry)
{
	if (entry.error)
		return entry.error == ENOENT ? "Vanished" : "Unavailable";
	if (S_ISDIR(entry.info.st_mode))
		return "Directory";
	if (S_ISLNK(entry.info.st_mode))
		return entry.broken ? "Broken link" : "Link";
	return S_ISREG(entry.info.st_mode) ? "File" : "Special";
}

Entry inspect(const std::string &directory, const std::string &name)
{
	Entry entry{name, display(name)};
	const auto path = join(directory, name);
	if (lstat(path.c_str(), &entry.info) < 0)
		entry.error = errno;
	else if (S_ISLNK(entry.info.st_mode)) {
		struct stat target;
		entry.broken = stat(path.c_str(), &target) < 0 &&
			(errno == ENOENT || errno == ENOTDIR || errno == ELOOP);
	}
	return entry;
}

std::vector<std::size_t> order(const std::vector<Entry> &entries, std::string_view filter,
			       bool hidden, Column column, bool descending)
{
	const auto needle = fold(filter);
	std::vector<std::size_t> rows;
	std::vector<std::string> names;
	names.reserve(entries.size());
	for (const auto &entry : entries) {
		names.push_back(fold(entry.label));
		if ((hidden || entry.name.front() != '.') && names.back().find(needle) != std::string::npos)
			rows.push_back(names.size() - 1);
	}
	std::sort(rows.begin(), rows.end(), [&](auto left, auto right) {
		const auto &first = entries[descending ? right : left];
		const auto &second = entries[descending ? left : right];
		if (column == Column::type) {
			const int types = std::strcmp(kind(first), kind(second));
			if (types)
				return types < 0;
		}
		if (column == Column::size && first.info.st_size != second.info.st_size)
			return first.info.st_size < second.info.st_size;
		if (column == Column::modified) {
			const auto &a_time = first.info.st_mtim, &b_time = second.info.st_mtim;
			if (a_time.tv_sec != b_time.tv_sec)
				return a_time.tv_sec < b_time.tv_sec;
			if (a_time.tv_nsec != b_time.tv_nsec)
				return a_time.tv_nsec < b_time.tv_nsec;
		}
		if (names[left] == names[right])
			return first.name < second.name;
		return descending ? names[right] < names[left] : names[left] < names[right];
	});
	return rows;
}

int directory_path(const std::string &input, bool follow, std::string &resolved)
{
	if (input.empty() || input.front() != '/')
		return EINVAL;
	if (input.size() >= PATH_MAX)
		return ENAMETOOLONG;
	std::string path = "/";
	if (follow) {
		char canonical[PATH_MAX];
		if (!realpath(input.c_str(), canonical))
			return errno;
		path = canonical;
		struct stat info;
		if (stat(path.c_str(), &info) < 0)
			return errno;
		if (!S_ISDIR(info.st_mode))
			return ENOTDIR;
	} else {
		std::string_view remaining(input);
		while (!remaining.empty()) {
			const auto end = remaining.find('/');
			const auto component = remaining.substr(0, end);
			remaining.remove_prefix(end == remaining.npos ? remaining.size() : end + 1);
			if (component == "..")
				path = parent(path);
			else if (!component.empty() && component != ".") {
				path = join(path, component);
				struct stat info;
				if (lstat(path.c_str(), &info) < 0)
					return errno;
				if (S_ISLNK(info.st_mode))
					return ELOOP;
				if (!S_ISDIR(info.st_mode))
					return ENOTDIR;
			}
		}
	}
	resolved = std::move(path);
	return 0;
}

void Scan::start(const std::string &path)
{
	cancel();
	entries.clear();
	limited = false;
	path_ = path;
	const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) {
		error = errno;
		return;
	}
	directory_ = fdopendir(fd);
	error = directory_ ? 0 : errno;
	if (!directory_)
		close(fd);
}

void Scan::cancel()
{
	if (directory_)
		closedir(directory_);
	directory_ = nullptr;
}

void Scan::step(std::size_t budget)
{
	while (directory_ && budget--) {
		errno = 0;
		const auto *record = readdir(directory_);
		if (!record) {
			error = errno;
			cancel();
			return;
		}
		const std::string name(record->d_name);
		if (name == "." || name == "..")
			continue;
		if (entries.size() == listing_limit) {
			limited = true;
			cancel();
			return;
		}
		entries.push_back(inspect(path_, name));
	}
}

bool Preferences::load(const std::string &home)
{
	if (home.empty() || home.front() != '/')
		return false;
	const auto text = read_bounded(join(home, ".config/mario/filemanager.conf"), 64 * 1024);
	std::string_view remaining(text);
	while (true) {
		const auto end = remaining.find('\n');
		if (end == remaining.npos)
			break;
		const auto line = remaining.substr(0, end);
		remaining.remove_prefix(end + 1);
		if (line == "hidden=1")
			hidden = true;
		else if (line == "split=1")
			split = true;
		else if (line == "descending=1")
			descending = true;
		else if (line.substr(0, 7) == "column=" && line.size() == 8 && line.back() >= '0' && line.back() <= '3')
			column = static_cast<Column>(line.back() - '0');
		else if (line.substr(0, 9) == "bookmark=" && bookmarks.size() < 32) {
			auto path = unhex(line.substr(9));
			if (!path.empty() && path.front() == '/' && std::find(bookmarks.begin(), bookmarks.end(), path) == bookmarks.end())
				bookmarks.push_back(std::move(path));
		}
	}
	return !text.empty();
}

bool Preferences::save(const std::string &home) const
{
	if (home.empty() || home.front() != '/')
		return false;
	const auto config = join(home, ".config"), folder = join(config, "mario");
	if ((mkdir(config.c_str(), 0700) < 0 && errno != EEXIST) ||
	    (mkdir(folder.c_str(), 0700) < 0 && errno != EEXIST))
		return false;
	std::string text = "hidden=" + std::to_string(hidden) + "\nsplit=" + std::to_string(split) +
		"\ncolumn=" + std::to_string(static_cast<int>(column)) + "\ndescending=" + std::to_string(descending) + '\n';
	for (const auto &bookmark : bookmarks)
		text += "bookmark=" + hex(bookmark) + '\n';
	if (text.size() > 64 * 1024)
		return false;
	auto temporary = join(folder, ".filemanager.XXXXXX");
	const int fd = mkstemp(temporary.data());
	if (fd < 0)
		return false;
	std::size_t offset = 0;
	while (offset < text.size()) {
		const auto bytes = write(fd, text.data() + offset, text.size() - offset);
		if (bytes > 0)
			offset += bytes;
		else if (!bytes || errno != EINTR)
			break;
	}
	const bool closed = close(fd) == 0;
	if (offset == text.size() && closed && rename(temporary.c_str(), join(folder, "filemanager.conf").c_str()) == 0)
		return true;
	unlink(temporary.c_str());
	return false;
}

std::vector<std::string> mounts()
{
	const auto text = read_bounded("/proc/mounts", 32 * 1024);
	std::string_view remaining(text);
	std::vector<std::string> paths;
	while (paths.size() < 64) {
		const auto end = remaining.find('\n');
		if (end == remaining.npos)
			break;
		auto line = remaining.substr(0, end);
		remaining.remove_prefix(end + 1);
		const auto source_end = line.find(' ');
		if (source_end == line.npos)
			continue;
		line.remove_prefix(source_end + 1);
		for (int field = 0; field < 4 && !line.empty(); ++field) {
			const auto separator = line.rfind(' ');
			line = separator == line.npos ? std::string_view{} : line.substr(0, separator);
		}
		if (!line.empty() && line.front() == '/')
			paths.emplace_back(line);
	}
	return paths;
}

}
