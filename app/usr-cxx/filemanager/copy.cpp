#include "copy_internal.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <memory>
#include <sys/statvfs.h>

namespace files::detail {

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

bool matches(int fd, const Identity &expected)
{
	struct stat info{};
	return fstat(fd, &info) == 0 && Identity(info) == expected;
}

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

ssize_t read_chunk(int fd, char *data, std::size_t size)
{
	ssize_t count;
	do {
		count = read(fd, data, size);
	} while (count < 0 && errno == EINTR);
	return count;
}

bool read_exact(int fd, char *data, std::size_t size)
{
	while (size) {
		const auto count = read_chunk(fd, data, size);
		if (count <= 0)
			return false;
		data += count;
		size -= count;
	}
	return true;
}

int erase(int parent, const std::string &name, mode_t type, const Fd &cwd)
{
	if (type != S_IFDIR)
		return unlinkat(parent, name.c_str(), 0) < 0 ? errno : 0;
	if (!cwd)
		return EBADF;
	if (fchdir(parent) < 0)
		return errno;
	const int error = rmdir(name.c_str()) < 0 ? errno : 0;
	return fchdir(cwd.get()) < 0 ? errno : error;
}

namespace {

constexpr std::size_t depth_limit = 48;
constexpr std::size_t journal_limit = 16384;
constexpr std::size_t root = journal_limit;

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
	std::string name, original;
	Identity identity;
	struct stat before;
	std::size_t end = 0, children = 0;
};

bool unchanged(const struct stat &before, const struct stat &after, bool removing = false)
{
	return Identity(before) == Identity(after) &&
		((removing && S_ISDIR(before.st_mode)) ||
		 (before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
		  before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
		  (removing || (before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
				before.st_ctim.tv_nsec == after.st_ctim.tv_nsec))));
}

class CopyJob {
public:
	CopyJob(CopyObserver &observer, int source, int destination, dev_t source_device, Identity destination_id)
		: observer_(observer), source_(source), destination_(destination), source_device_(source_device),
		  destination_id_(destination_id) {}

	CopyResult item(std::size_t index, const std::string &name, const std::string &new_name,
			bool remove_source, bool offer_conflict, const std::function<int()> &before_remove)
	{
		item_ = index;
		offer_conflict_ = offer_conflict;
		if (!component(new_name))
			return failure(EINVAL);
		auto result = node(source_, name, destination_, root, 0, &new_name);
		if (result.state == CopyState::copied && remove_source) {
			result = verify(0, source_, destination_);
			if (result.state == CopyState::copied) {
				if (observer_.cancelled())
					result = cancelled;
				else if (const int error = before_remove ? before_remove() : 0)
					result = failure(error);
				else
					result = remove();
			}
		}
		if (result.state != CopyState::copied && !result.source_remaining && destination_ >= 0 && !rollback())
			result.partial = true;
		return result;
	}

private:
	CopyResult node(int source, const std::string &name, int destination, std::size_t parent, std::size_t depth,
			const std::string *new_name = nullptr)
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
		if (destination < 0)
			return record(source, name, before, parent, depth);
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

		const auto &requested = new_name ? *new_name : name;
		std::string output_name = requested;
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
					output_name = alternate(requested, number, max_name_);
					continue;
				}
			}
			if (link && error == ENAMETOOLONG)
				return {CopyState::unsupported, ENAMETOOLONG};
			if (error != EEXIST || !offer_conflict_)
				return failure(error);
			if (parent != root)
				return skipped;
			if (!number) {
				const auto decision = observer_.conflict(item_, requested);
				if (decision == CopyDecision::cancel)
					return cancelled;
				if (decision == CopyDecision::skip)
					return skipped;
			}
			if (++number > 999)
				return failure(EEXIST);
			output_name = alternate(requested, number, max_name_);
		}

		struct stat created{};
		const int status = output ? fstat(output.get(), &created) :
			fstatat(destination, output_name.c_str(), &created, AT_SYMLINK_NOFOLLOW);
		if (status < 0 || created.st_dev != destination_id_.device ||
		    (created.st_mode & S_IFMT) != original.type)
			return {CopyState::failed, status < 0 ? errno : ESTALE, true};
		const auto slot = journal_.size();
		journal_.push_back({parent, output_name, name, Identity(created), before, slot + 1});
		if (parent != root)
			++journal_[parent].children;
		if (observer_.cancelled())
			return cancelled;
		if (directory) {
			output = Fd(openat(destination, output_name.c_str(), directory_flags));
			if (!output || !matches(output.get(), journal_[slot].identity))
				return failure(ESTALE);
			const auto result = children(input, output.get(), slot, depth + 1);
			journal_[slot].end = journal_.size();
			return result;
		}
		if (link)
			return copied;
		return data(input.get(), output, before);
	}

	CopyResult record(int parent_fd, const std::string &name, const struct stat &before, std::size_t parent,
			  std::size_t depth)
	{
		const auto slot = journal_.size();
		journal_.push_back({parent, {}, name, Identity{}, before, slot + 1});
		if (parent != root)
			++journal_[parent].children;
		if (!S_ISDIR(before.st_mode))
			return copied;
		Fd input(openat(parent_fd, name.c_str(), directory_flags));
		if (!input || !matches(input.get(), Identity(before)))
			return failure(ESTALE);
		const auto result = children(input, -1, slot, depth + 1);
		journal_[slot].end = journal_.size();
		return result;
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

	CopyResult verify(std::size_t slot, int source, int destination)
	{
		if (observer_.cancelled())
			return cancelled;
		const auto &entry = journal_[slot];
		struct stat current{}, output{};
		if (fstatat(source, entry.original.c_str(), &current, AT_SYMLINK_NOFOLLOW) < 0 ||
		    !unchanged(entry.before, current))
			return failure(ESTALE);
		if (destination >= 0 && (fstatat(destination, entry.name.c_str(), &output, AT_SYMLINK_NOFOLLOW) < 0 ||
		    !(Identity(output) == entry.identity)))
			return failure(ESTALE);
		if (S_ISDIR(current.st_mode)) {
			Fd from(openat(source, entry.original.c_str(), directory_flags));
			if (!from || !matches(from.get(), Identity(current)))
				return failure(ESTALE);
			Fd to;
			if (destination >= 0) {
				to = Fd(openat(destination, entry.name.c_str(), directory_flags));
				if (!to || !matches(to.get(), entry.identity))
					return failure(ESTALE);
			}
			for (auto child = slot + 1; child < entry.end; child = journal_[child].end) {
				const auto result = verify(child, from.get(), to.get());
				if (result.state != CopyState::copied)
					return result;
			}
			const auto result = count_children(from, entry.children);
			if (result.state != CopyState::copied || !to)
				return result;
			return count_children(to, entry.children);
		}
		if (destination < 0)
			return copied;
		if (S_ISLNK(current.st_mode)) {
			const auto first = readlinkat(source, entry.original.c_str(), buffer_.data(), buffer_.size());
			const auto second = readlinkat(destination, entry.name.c_str(), compare_.data(), compare_.size());
			struct stat after{}, final_output{};
			return first >= 0 && static_cast<std::size_t>(first) < buffer_.size() && first == second &&
			       std::equal(buffer_.data(), buffer_.data() + first, compare_.data()) &&
			       fstatat(source, entry.original.c_str(), &after, AT_SYMLINK_NOFOLLOW) == 0 && unchanged(current, after) &&
			       fstatat(destination, entry.name.c_str(), &final_output, AT_SYMLINK_NOFOLLOW) == 0 &&
			       unchanged(output, final_output) ? copied : failure(ESTALE);
		}
		if (current.st_size != output.st_size)
			return failure(ESTALE);
		constexpr int flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;
		Fd from(openat(source, entry.original.c_str(), flags));
		Fd to(openat(destination, entry.name.c_str(), flags));
		if (!from || !to || !matches(from.get(), Identity(current)) || !matches(to.get(), entry.identity))
			return failure(ESTALE);
		for (off_t left = current.st_size; left > 0;) {
			if (observer_.cancelled())
				return cancelled;
			const auto size = std::min<off_t>(left, buffer_.size());
			if (!read_exact(from.get(), buffer_.data(), size) || !read_exact(to.get(), compare_.data(), size) ||
			    !std::equal(buffer_.data(), buffer_.data() + size, compare_.data()))
				return failure(ESTALE);
			left -= size;
		}
		struct stat after{}, final_output{};
		return fstat(from.get(), &after) == 0 && unchanged(current, after) &&
		       fstat(to.get(), &final_output) == 0 && unchanged(output, final_output) ? copied : failure(ESTALE);
	}

	CopyResult count_children(Fd &directory, std::size_t expected)
	{
		std::unique_ptr<DIR, decltype(&closedir)> entries(fdopendir(directory.get()), closedir);
		if (!entries)
			return failure(errno);
		directory.release();
		while (!observer_.cancelled()) {
			errno = 0;
			const auto *entry = readdir(entries.get());
			if (!entry)
				return errno ? failure(errno) : expected ? failure(ESTALE) : copied;
			const std::string_view name(entry->d_name);
			if (name != "." && name != "..") {
				if (!expected)
					return failure(ESTALE);
				--expected;
			}
		}
		return cancelled;
	}

	int open_parent(const Created &node, bool destination, Fd &current, std::string *path = nullptr)
	{
		std::array<std::size_t, depth_limit> chain;
		std::size_t depth = 0;
		for (auto slot = node.parent; slot != root; slot = journal_[slot].parent)
			chain[depth++] = slot;
		if (path)
			for (auto index = depth; index > 0; --index)
				*path += journal_[chain[index - 1]].original + '/';
		int parent = destination ? destination_ : source_;
		while (depth) {
			const auto &part = journal_[chain[--depth]];
			const auto &name = destination ? part.name : part.original;
			Fd next(openat(parent, name.c_str(), directory_flags));
			if (!next || !matches(next.get(), destination ? part.identity : Identity(part.before)))
				return -1;
			current = std::move(next);
			parent = current.get();
		}
		return parent;
	}

	CopyResult remove()
	{
		Fd cwd(open(".", directory_flags));
		for (std::size_t remaining = journal_.size(); remaining > 0; --remaining) {
			const auto &entry = journal_[remaining - 1];
			Fd current;
			const int parent = open_parent(entry, false, current);
			struct stat info{};
			int error = ESTALE;
			if (parent >= 0 && fstatat(parent, entry.original.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
			    unchanged(entry.before, info, true))
				error = erase(parent, entry.original, info.st_mode & S_IFMT, cwd);
			if (error) {
				auto result = failure(error);
				result.destination_complete = destination_ >= 0;
				result.source_remaining = remaining;
				std::string path;
				Fd ignored;
				open_parent(entry, false, ignored, &path);
				path += entry.original;
				const auto cap = result.first_remaining.size() - 1;
				if (path.size() > cap)
					path = ".../" + path.substr(path.size() - (cap - 4));
				std::copy(path.begin(), path.end(), result.first_remaining.begin());
				return result;
			}
		}
		return copied;
	}

	bool rollback()
	{
		if (journal_.empty())
			return true;
		Fd cwd(open(".", directory_flags));
		bool complete = true;
		for (auto node = journal_.rbegin(); node != journal_.rend(); ++node) {
			Fd current;
			const int parent = open_parent(*node, true, current);
			if (parent < 0) {
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
			complete &= erase(parent, node->name, node->identity.type, cwd) == 0;
		}
		return complete;
	}

	CopyObserver &observer_;
	int source_, destination_;
	dev_t source_device_;
	Identity destination_id_;
	std::size_t max_name_ = copy_name_limit;
	std::size_t item_ = 0;
	uint64_t bytes_ = 0;
	bool offer_conflict_ = true;
	std::array<char, copy_chunk> buffer_, compare_;
	std::vector<Created> journal_;
};

}

CopyResult TreeJob::run(CopyObserver &observer, std::size_t index, const std::string &name,
		       const std::string &output, const std::function<int()> &before_remove) const
{
	struct stat from{}, to{};
	if (fstat(source, &from) < 0 || (destination >= 0 && fstat(destination, &to) < 0))
		return failure(errno);
	return CopyJob(observer, source, destination, from.st_dev, Identity(to))
		.item(index, name, output, remove_source, offer_conflict, before_remove);
}

}
