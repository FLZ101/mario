#include "copy_internal.h"

#include <cerrno>
#include <cstdio>
#include <time.h>

namespace files {

using namespace detail;

namespace {

constexpr char trash_name[] = ".mario-trash";
constexpr char owner_name[] = "owner";
constexpr char trash_version[] = "Mario File Manager Trash 1\n";
constexpr char original_name[] = "original";
constexpr char payload_name[] = "item";
constexpr char ready_name[] = "ready";

std::string parent_path(const std::string &path)
{
	const auto slash = path.rfind('/');
	return slash == 0 ? "/" : path.substr(0, slash);
}

std::string child_path(const std::string &path, std::string_view name)
{
	return path + (path == "/" ? "" : "/") + std::string(name);
}

bool valid_path(std::string_view path)
{
	if (path.size() < 2 || path.size() > path_limit || path.front() != '/' || path.back() == '/')
		return false;
	path.remove_prefix(1);
	while (!path.empty()) {
		const auto slash = path.find('/');
		if (!component(path.substr(0, slash)))
			return false;
		if (slash == path.npos)
			return true;
		path.remove_prefix(slash + 1);
	}
	return false;
}

bool slot_name(std::string_view name)
{
	return name.size() == 24 && name.substr(0, 5) == "item-" &&
		name.find_first_not_of("0123456789abcdef", 5) == name.npos;
}

int write_small(int directory, const char *name, std::string_view value)
{
	Fd fd(openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
	if (!fd)
		return errno;
	while (!value.empty()) {
		const auto count = write(fd.get(), value.data(), value.size());
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return count < 0 ? errno : EIO;
		value.remove_prefix(count);
	}
	return close(fd.release()) < 0 ? errno : 0;
}

bool read_small(int directory, const char *name, std::string &value)
{
	Fd fd(openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
	struct stat info{};
	if (!fd || fstat(fd.get(), &info) < 0 || !S_ISREG(info.st_mode) || info.st_size < 0 || info.st_size > path_limit)
		return false;
	value.resize(info.st_size);
	char extra;
	return read_exact(fd.get(), value.data(), value.size()) && read_chunk(fd.get(), &extra, 1) == 0;
}

struct Volume {
	std::string path;
	Fd directory;
	dev_t device = 0;

	int find(const std::string &start, int fd)
	{
		struct stat info{};
		if (fstat(fd, &info) < 0)
			return errno;
		device = info.st_dev;
		path = start;
		directory = Fd(dup(fd));
		if (!directory)
			return errno;
		while (path != "/") {
			auto above = parent_path(path);
			Fd next = open_directory(above);
			if (!next || fstat(next.get(), &info) < 0)
				return errno;
			if (info.st_dev != device)
				break;
			path = std::move(above);
			directory = std::move(next);
		}
		return 0;
	}

	int trash(Fd &store, bool create = false) const
	{
		bool created = false;
		if (create) {
			created = mkdirat(directory.get(), trash_name, 0700) == 0;
			if (!created && errno != EEXIST)
				return errno;
		}
		store = Fd(openat(directory.get(), trash_name, directory_flags));
		if (!store)
			return errno;
		struct stat info{};
		if (fstat(store.get(), &info) < 0)
			return errno;
		if (info.st_dev != device || info.st_uid != geteuid())
			return EPERM;
		if (created)
			return write_small(store.get(), owner_name, trash_version);
		std::string version;
		return read_small(store.get(), owner_name, version) && version == trash_version ? 0 : EPERM;
	}

	bool protected_entry(int fd, const std::string &name) const
	{
		struct stat store{}, entry{};
		if (name == trash_name && fstat(directory.get(), &store) == 0 && matches(fd, Identity(store)))
			return true;
		if (fstatat(directory.get(), trash_name, &store, AT_SYMLINK_NOFOLLOW) < 0)
			return errno != ENOENT;
		if (fstatat(fd, name.c_str(), &entry, AT_SYMLINK_NOFOLLOW) == 0 && Identity(entry) == Identity(store))
			return true;
		return S_ISDIR(store.st_mode) && check_ancestors(fd, Identity(store)) != 0;
	}
};

bool same_directory(int first, int second)
{
	struct stat info{};
	return fstat(first, &info) == 0 && matches(second, Identity(info));
}

int inspect_slot(int slot, TrashItem &item)
{
	item = {};
	if (!read_small(slot, original_name, item.original) || !valid_path(item.original))
		item.original.clear();
	struct stat payload{};
	if (fstatat(slot, payload_name, &payload, AT_SYMLINK_NOFOLLOW) == 0)
		item.has_payload = true;
	else if (errno != ENOENT)
		return errno;
	std::string ready;
	item.ready = !item.original.empty() && item.has_payload && read_small(slot, ready_name, ready) && ready.empty();
	return 0;
}

int clear_slot(int parent, Fd &slot, const std::string &name)
{
	struct stat check{};
	if (fstatat(parent, name.c_str(), &check, AT_SYMLINK_NOFOLLOW) < 0 || !matches(slot.get(), Identity(check)))
		return ESTALE;
	for (const auto *marker : {ready_name, original_name})
		if (unlinkat(slot.get(), marker, 0) < 0 && errno != ENOENT)
			return errno;
	slot = Fd{};
	return erase(parent, name, S_IFDIR, Fd(open(".", directory_flags)));
}

CopyResult send_to_trash(const Volume &volume, CopyObserver &observer, std::size_t index, int source,
			 const std::string &path, const std::string &name)
{
	const auto original = child_path(path, name);
	if (!valid_path(original))
		return failure(EINVAL);
	struct stat info{};
	if (fstatat(source, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) < 0)
		return failure(errno);
	if (info.st_dev != volume.device)
		return {CopyState::unsupported, EXDEV};
	Fd store;
	int error = volume.trash(store, true);
	if (error)
		return failure(error);
	std::array<char, 25> name_buffer;
	for (unsigned attempt = 0;; ++attempt) {
		if (observer.cancelled())
			return cancelled;
		std::snprintf(name_buffer.data(), name_buffer.size(), "item-%08x%08x%03x",
			      static_cast<unsigned>(time(nullptr)), static_cast<unsigned>(getpid()), attempt);
		if (mkdirat(store.get(), name_buffer.data(), 0700) == 0)
			break;
		if (errno != EEXIST || attempt == 255)
			return failure(errno);
	}
	Fd slot(openat(store.get(), name_buffer.data(), directory_flags));
	struct stat target{};
	if (!slot || fstat(slot.get(), &target) < 0 || target.st_dev != volume.device)
		return {CopyState::failed, ESTALE, true};
	error = write_small(slot.get(), original_name, original);
	CopyResult result = error ? failure(error) : TreeJob{source, slot.get(), true, false}
		.run(observer, index, name, payload_name);
	if (result.state == CopyState::copied) {
		error = write_small(slot.get(), ready_name, {});
		if (error)
			return {CopyState::failed, error, true, true};
	} else if (!result.partial && !result.destination_complete) {
		result.partial = clear_slot(store.get(), slot, name_buffer.data()) != 0;
	}
	return result;
}

CopyResult restore_slot(const CopyRequest &request, const Volume &volume, CopyObserver &observer,
			std::size_t index, int source, const std::string &name)
{
	Fd store;
	const int error = volume.trash(store);
	if (error || !same_directory(store.get(), source) || !slot_name(name))
		return failure(error ? error : EPERM);
	Fd slot(openat(source, name.c_str(), directory_flags));
	struct stat from{};
	if (!slot || fstat(slot.get(), &from) < 0 || from.st_dev != volume.device)
		return failure(ESTALE);
	TrashItem item;
	if (const int error = inspect_slot(slot.get(), item))
		return failure(error);
	const bool restore = request.action == CopyAction::restore;
	if (!item.has_payload || item.ready != restore || (restore && item.original != request.destination))
		return failure(ESTALE);
	const auto path = restore ? parent_path(item.original) : request.destination;
	const auto output = item.original.empty() ? name : item.original.substr(item.original.rfind('/') + 1);
	Fd destination = open_directory(path);
	if (!destination)
		return failure(errno);
	Volume output_volume;
	if (const int error = output_volume.find(path, destination.get()))
		return failure(error);
	if (restore && output_volume.device != volume.device)
		return failure(EXDEV);
	if (output_volume.protected_entry(destination.get(), output))
		return failure(EPERM);
	const auto before_remove = [&]() { return unlinkat(slot.get(), ready_name, 0) < 0 ? errno : 0; };
	auto result = TreeJob{slot.get(), destination.get(), restore, true}.run(observer, index, payload_name, output, before_remove);
	if (restore && result.state == CopyState::copied) {
		if (const int error = clear_slot(source, slot, name))
			return {CopyState::failed, error, true, true};
	}
	return result;
}

}

int trash_directory(const std::string &path, std::string &directory)
{
	Fd from = open_directory(path), store;
	Volume volume;
	const int error = from ? volume.find(path, from.get()) : errno;
	if (error)
		return error;
	if (const int error = volume.trash(store))
		return error;
	directory = child_path(volume.path, trash_name);
	return 0;
}

int trash_item(const std::string &directory, const std::string &name, TrashItem &item)
{
	Fd store = open_directory(directory), verified;
	Volume volume;
	const int error = store ? volume.find(directory, store.get()) : errno;
	if (error)
		return error;
	if (const int error = volume.trash(verified))
		return error;
	if (!slot_name(name) || !same_directory(store.get(), verified.get()))
		return EINVAL;
	Fd slot(openat(store.get(), name.c_str(), directory_flags));
	struct stat info{};
	if (!slot || fstat(slot.get(), &info) < 0 || info.st_dev != volume.device)
		return ESTALE;
	return inspect_slot(slot.get(), item);
}

void copy_items(const CopyRequest &request, CopyObserver &observer)
{
	if (request.names.empty() || request.names.size() > copy_selection_limit)
		return;
	const auto action = request.action;
	const bool exact = action == CopyAction::rename;
	const bool two_directories = action == CopyAction::copy || action == CopyAction::move || exact;
	const bool mutating_source = action == CopyAction::move || exact || action == CopyAction::trash || action == CopyAction::remove;
	Fd source = open_directory(request.source), destination;
	Volume volume, output_volume;
	int error = source ? volume.find(request.source, source.get()) : errno;
	if (!error && two_directories) {
		destination = open_directory(exact ? request.source : request.destination);
		if (!destination)
			error = errno;
		else if (action == CopyAction::move && same_directory(source.get(), destination.get()))
			error = EINVAL;
		else
			error = output_volume.find(exact ? request.source : request.destination, destination.get());
	}
	Fd store;
	const bool in_trash_root = !error && action == CopyAction::remove && !volume.trash(store) &&
		same_directory(source.get(), store.get());
	const auto perform = [&](std::size_t index) -> CopyResult {
		const auto &name = request.names[index];
		if (!component(name))
			return failure(EINVAL);
		if (mutating_source && volume.protected_entry(source.get(), name) && !(in_trash_root && slot_name(name)))
			return failure(EPERM);
		if (action == CopyAction::mkdir) {
			if (volume.protected_entry(source.get(), name))
				return failure(EPERM);
			return mkdirat(source.get(), name.c_str(), 0755) < 0 ? failure(errno) : copied;
		}
		if (action == CopyAction::trash)
			return send_to_trash(volume, observer, index, source.get(), request.source, name);
		if (action == CopyAction::restore || action == CopyAction::recover)
			return restore_slot(request, volume, observer, index, source.get(), name);
		const auto &target = exact ? request.destination : name;
		if (two_directories && output_volume.protected_entry(destination.get(), target))
			return failure(EPERM);
		return TreeJob{source.get(), destination.get(), mutating_source, !exact}.run(observer, index, name, target);
	};
	bool stopping = false;
	for (std::size_t index = 0; index < request.names.size(); ++index) {
		stopping = stopping || observer.cancelled();
		const auto outcome = stopping ? cancelled : error ? failure(error) : perform(index);
		stopping |= outcome.state == CopyState::cancelled;
		observer.result(index, outcome);
	}
}

}
