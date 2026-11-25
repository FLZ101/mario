#include "copy.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace files {
namespace {

constexpr std::size_t depth_limit = 48;
constexpr std::size_t journal_limit = 16384;
constexpr std::size_t path_limit = 2047;
constexpr int directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
constexpr std::size_t root = journal_limit;

class Fd {
public:
	explicit Fd(int fd = -1) : fd_(fd) {}
	~Fd() { if (fd_ >= 0) close(fd_); }
	Fd(const Fd &) = delete;
	Fd &operator=(const Fd &) = delete;
	Fd(Fd &&other) noexcept : fd_(other.release()) {}
	Fd &operator=(Fd &&other) noexcept
	{
		if (fd_ >= 0)
			close(fd_);
		fd_ = other.release();
		return *this;
	}
	int get() const { return fd_; }
	explicit operator bool() const { return fd_ >= 0; }
	int release()
	{
		const int fd = fd_;
		fd_ = -1;
		return fd;
	}

private:
	int fd_;
};

bool component(std::string_view name)
{
	return !name.empty() && name.size() <= copy_name_limit && name != "." && name != ".." &&
		name.find('/') == name.npos && name.find('\0') == name.npos;
}

Fd open_directory(std::string_view path)
{
	if (path.empty() || path.front() != '/' || path.size() > path_limit) {
		errno = EINVAL;
		return Fd{};
	}
	Fd directory(open("/", directory_flags));
	path.remove_prefix(1);
	while (directory && !path.empty()) {
		const auto slash = path.find('/');
		const auto name = path.substr(0, slash);
		if (!component(name)) {
			errno = EINVAL;
			return Fd{};
		}
		Fd next(openat(directory.get(), std::string(name).c_str(), directory_flags));
		if (!next)
			return Fd{};
		directory = std::move(next);
		if (slash == path.npos)
			break;
		path.remove_prefix(slash + 1);
	}
	return directory;
}

struct Identity {
	dev_t device = 0;
	ino_t inode = 0;
	mode_t type = 0;
	explicit Identity(const struct stat &info = {}) : device(info.st_dev), inode(info.st_ino),
		type(info.st_mode & S_IFMT) {}
	bool operator==(const Identity &other) const
	{
		return device == other.device && inode == other.inode && type == other.type;
	}
};

bool matches(int fd, const Identity &expected)
{
	struct stat info{};
	return fstat(fd, &info) == 0 && Identity(info) == expected;
}

CopyResult failure(int error) { return {CopyState::failed, error}; }
constexpr CopyResult copied{CopyState::copied};
constexpr CopyResult skipped{CopyState::skipped};
constexpr CopyResult cancelled{CopyState::cancelled};

int check_ancestors(int fd, const Identity &source)
{
	Fd directory(dup(fd));
	if (!directory)
		return errno;
	for (std::size_t depth = 0; depth < path_limit; ++depth) {
		struct stat current{}, parent{};
		if (fstat(directory.get(), &current) < 0)
			return errno;
		if (Identity(current) == source)
			return EINVAL;
		Fd next(openat(directory.get(), "..", directory_flags));
		if (!next || fstat(next.get(), &parent) < 0)
			return errno;
		if (Identity(current) == Identity(parent))
			return 0;
		directory = std::move(next);
	}
	return ELOOP;
}

std::string alternate(std::string_view original, unsigned number, std::size_t max_size)
{
	const auto dot = original.rfind('.');
	auto stem = original.substr(0, dot == 0 ? original.npos : dot);
	auto extension = original.substr(stem.size());
	const auto suffix = " (copy " + std::to_string(number) + ")";
	if (extension.size() + suffix.size() >= max_size)
		extension = {};
	if (stem.size() > max_size - extension.size() - suffix.size()) {
		auto end = max_size - extension.size() - suffix.size();
		while (end && (static_cast<unsigned char>(stem[end]) & 0xc0) == 0x80)
			--end;
		stem = stem.substr(0, end);
	}
	return std::string(stem) + suffix + std::string(extension);
}

struct Created {
	std::size_t parent;
	std::string name;
	Identity identity;
};

class CopyJob {
public:
	CopyJob(CopyObserver &observer, int destination, dev_t source_device, Identity destination_id)
		: observer_(observer), destination_(destination), source_device_(source_device),
		  destination_id_(destination_id) {}

	CopyResult item(std::size_t index, int source, const std::string &name)
	{
		item_ = index;
		auto result = node(source, name, destination_, root, 0);
		if (result.state != CopyState::copied && !rollback())
			result.partial = true;
		journal_.clear();
		return result;
	}

private:
	CopyResult node(int source, const std::string &name, int destination, std::size_t parent, std::size_t depth)
	{
		if (observer_.cancelled())
			return cancelled;
		if (!component(name))
			return failure(EINVAL);
		if (depth >= depth_limit || journal_.size() >= journal_limit)
			return failure(E2BIG);
		struct stat before{};
		if (fstatat(source, name.c_str(), &before, AT_SYMLINK_NOFOLLOW) < 0)
			return failure(errno);
		if (before.st_dev != source_device_)
			return {CopyState::unsupported, EXDEV};
		const bool directory = S_ISDIR(before.st_mode), link = S_ISLNK(before.st_mode);
		if (!directory && !link && !S_ISREG(before.st_mode))
			return {CopyState::unsupported, ENOTSUP};
		const Identity original(before);
		if (original == destination_id_)
			return failure(EINVAL);
		Fd input;
		std::string target;
		if (link) {
			std::array<char, path_limit + 1> text;
			const auto length = readlinkat(source, name.c_str(), text.data(), text.size());
			if (length < 0)
				return failure(errno);
			struct stat after{};
			if (fstatat(source, name.c_str(), &after, AT_SYMLINK_NOFOLLOW) < 0 || !(Identity(after) == original))
				return failure(ESTALE);
			if (static_cast<std::size_t>(length) >= text.size())
				return {CopyState::unsupported, ENAMETOOLONG};
			target.assign(text.data(), length);
		} else {
			input = Fd(openat(source, name.c_str(), directory ? directory_flags :
				O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
			if (!input)
				return failure(errno);
			if (!matches(input.get(), original))
				return failure(ESTALE);
			if (directory && parent == root) {
				const int error = check_ancestors(destination, original);
				if (error)
					return failure(error);
			}
		}

		std::string output_name = name;
		Fd output;
		unsigned number = 0;
		for (;;) {
			if (observer_.cancelled())
				return cancelled;
			int status;
			if (directory)
				status = mkdirat(destination, output_name.c_str(), 0755);
			else if (link)
				status = symlinkat(target.c_str(), destination, output_name.c_str());
			else {
				output = Fd(openat(destination, output_name.c_str(),
					O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644));
				status = output.get();
			}
			if (status >= 0)
				break;
			const int error = errno;
			if (link && (error == EBADF || error == EOPNOTSUPP || error == ENOSYS))
				return {CopyState::unsupported, ENOTSUP};
			if (number && error == ENAMETOOLONG && max_name_ == copy_name_limit) {
				struct statvfs volume{};
				if (fstatvfs(destination, &volume) == 0 && volume.f_namemax >= 12 &&
				    volume.f_namemax < output_name.size()) {
					max_name_ = volume.f_namemax;
					output_name = alternate(name, number, max_name_);
					continue;
				}
			}
			if (link && error == ENAMETOOLONG)
				return {CopyState::unsupported, ENAMETOOLONG};
			if (error != EEXIST)
				return failure(error);
			if (!number) {
				const auto decision = observer_.conflict(item_, name);
				if (decision == CopyDecision::cancel)
					return cancelled;
				if (decision == CopyDecision::skip)
					return skipped;
			}
			if (++number > 999)
				return failure(EEXIST);
			output_name = alternate(name, number, max_name_);
		}

		struct stat created{};
		const int status = output ? fstat(output.get(), &created) :
			fstatat(destination, output_name.c_str(), &created, AT_SYMLINK_NOFOLLOW);
		if (status < 0 || created.st_dev != destination_id_.device ||
		    (created.st_mode & S_IFMT) != original.type)
			return {CopyState::failed, status < 0 ? errno : ESTALE, true};
		const auto slot = journal_.size();
		journal_.push_back({parent, output_name, Identity(created)});
		if (observer_.cancelled())
			return cancelled;
		if (directory) {
			output = Fd(openat(destination, output_name.c_str(), directory_flags));
			if (!output || !matches(output.get(), journal_[slot].identity))
				return failure(ESTALE);
			return children(input, output.get(), slot, depth + 1);
		}
		if (link)
			return copied;
		return data(input.get(), output, before);
	}

	CopyResult children(Fd &input, int destination, std::size_t parent, std::size_t depth)
	{
		std::unique_ptr<DIR, decltype(&closedir)> entries(fdopendir(input.get()), closedir);
		if (!entries)
			return failure(errno);
		input.release();
		while (!observer_.cancelled()) {
			errno = 0;
			const auto *entry = readdir(entries.get());
			if (!entry)
				return errno ? failure(errno) : copied;
			const std::string name(entry->d_name);
			if (name == "." || name == "..")
				continue;
			const auto result = node(dirfd(entries.get()), name, destination, parent, depth);
			if (result.state != CopyState::copied)
				return result;
		}
		return cancelled;
	}

	CopyResult data(int input, Fd &output, const struct stat &before)
	{
		off_t remaining = before.st_size;
		observer_.progress(item_, bytes_);
		while (remaining > 0) {
			if (observer_.cancelled())
				return cancelled;
			const auto length = read(input, buffer_.data(), std::min<off_t>(remaining, buffer_.size()));
			if (length < 0 && errno == EINTR)
				continue;
			if (length <= 0)
				return failure(length ? errno : ESTALE);
			ssize_t offset = 0;
			while (offset < length) {
				if (observer_.cancelled())
					return cancelled;
				const auto written = write(output.get(), buffer_.data() + offset, length - offset);
				if (written < 0 && errno == EINTR)
					continue;
				if (written <= 0)
					return failure(written ? errno : EIO);
				offset += written;
				bytes_ += written;
			}
			remaining -= length;
			observer_.progress(item_, bytes_);
		}
		if (observer_.cancelled())
			return cancelled;
		struct stat after{};
		if (fstat(input, &after) < 0)
			return failure(errno);
		if (after.st_size != before.st_size || after.st_mtim.tv_sec != before.st_mtim.tv_sec ||
		    after.st_mtim.tv_nsec != before.st_mtim.tv_nsec)
			return failure(ESTALE);
		if (close(output.release()) < 0)
			return failure(errno);
		return copied;
	}

	bool rollback()
	{
		if (journal_.empty())
			return true;
		Fd cwd(open(".", directory_flags));
		bool complete = true;
		for (auto node = journal_.rbegin(); node != journal_.rend(); ++node) {
			std::array<std::size_t, depth_limit> chain;
			std::size_t depth = 0;
			for (auto slot = node->parent; slot != root; slot = journal_[slot].parent)
				chain[depth++] = slot;
			int parent = destination_;
			Fd current;
			bool valid = true;
			while (depth) {
				const auto &part = journal_[chain[--depth]];
				Fd next(openat(parent, part.name.c_str(), directory_flags));
				if (!next || !matches(next.get(), part.identity)) {
					valid = false;
					break;
				}
				current = std::move(next);
				parent = current.get();
			}
			if (!valid) {
				complete = false;
				continue;
			}
			struct stat info{};
			if (fstatat(parent, node->name.c_str(), &info, AT_SYMLINK_NOFOLLOW) < 0) {
				complete &= errno == ENOENT;
				continue;
			}
			if (!(Identity(info) == node->identity)) {
				complete = false;
				continue;
			}
			if (node->identity.type == S_IFDIR) {
				if (!cwd || fchdir(parent) < 0) {
					complete = false;
					continue;
				}
				complete &= rmdir(node->name.c_str()) == 0;
				if (fchdir(cwd.get()) < 0)
					return false;
			} else
				complete &= unlinkat(parent, node->name.c_str(), 0) == 0;
		}
		return complete;
	}

	CopyObserver &observer_;
	int destination_;
	dev_t source_device_;
	Identity destination_id_;
	std::size_t max_name_ = copy_name_limit;
	std::size_t item_ = 0;
	uint64_t bytes_ = 0;
	std::array<char, copy_chunk> buffer_;
	std::vector<Created> journal_;
};

}

void copy_items(const CopyRequest &request, CopyObserver &observer)
{
	if (request.names.empty() || request.names.size() > copy_selection_limit)
		return;
	Fd source = open_directory(request.source);
	int error = source ? 0 : errno;
	Fd destination = open_directory(request.destination);
	if (!error && !destination)
		error = errno;
	struct stat from{}, to{};
	if (!error && (fstat(source.get(), &from) < 0 || fstat(destination.get(), &to) < 0))
		error = errno;
	CopyJob job(observer, destination.get(), from.st_dev, Identity(to));
	bool stopping = false;
	for (std::size_t index = 0; index < request.names.size(); ++index) {
		stopping = stopping || observer.cancelled();
		const auto outcome = stopping ? cancelled : error ? failure(error) :
			job.item(index, source.get(), request.names[index]);
		stopping |= outcome.state == CopyState::cancelled;
		observer.result(index, outcome);
	}
}

}
