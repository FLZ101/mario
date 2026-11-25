#include "copy.h"
#include "copy_worker.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using files::CopyDecision;
using files::CopyState;

void check(bool success, const char *expectation)
{
	if (!success) {
		std::fprintf(stderr, "filemanager copy: %s (errno %d)\n", expectation, errno);
		std::exit(1);
	}
}

void contend_for_directory(int directory)
{
	int start[2];
	check(pipe(start) == 0, "create directory race barrier");
	std::array<pid_t, 2> children;
	for (auto &child : children) {
		child = fork();
		check(child >= 0, "fork mkdir contender");
		if (!child) {
			close(start[1]);
			char ready;
			if (read(start[0], &ready, 1) != 1)
				_exit(2);
			_exit(mkdirat(directory, "fm-probe", 0700) == 0 ? 0 : errno == EEXIST ? 1 : 2);
		}
	}
	close(start[0]);
	check(write(start[1], "xx", 2) == 2, "start both mkdir contenders");
	close(start[1]);
	unsigned winners = 0;
	for (const auto child : children) {
		int status;
		check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) <= 1,
			"mkdir returns success or EEXIST");
		winners += WEXITSTATUS(status) == 0;
	}
	check(winners == 1, "exactly one mkdir creates the directory");
}

void probe(const char *path, bool vfat)
{
	const int directory = open(path, O_RDONLY | O_DIRECTORY);
	check(directory >= 0, "open fixture parent");
	contend_for_directory(directory);
	check(mkdirat(directory, "fm-probe", 0700) < 0 && errno == EEXIST, "exclusive mkdir");
	const int child = openat(directory, "fm-probe", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	check(child >= 0, "open relative directory");
	const int file = openat(child, "data", O_WRONLY | O_CREAT | O_EXCL, 0600);
	check(file >= 0 && write(file, "abc", 3) == 3, "create relative file");
	check(openat(child, "data", O_WRONLY | O_CREAT | O_EXCL, 0600) < 0 && errno == EEXIST,
		"exclusive open");
	struct stat relative{}, opened{};
	check(fstatat(child, "data", &relative, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(relative.st_mode) &&
		relative.st_size == 3, "relative stat via statx");
	check(fstat(file, &opened) == 0 && opened.st_dev == relative.st_dev && opened.st_ino == relative.st_ino,
		"consistent inode identity");
	check(close(file) == 0, "close fixture file");
	if (vfat) {
		check(symlinkat("gone", child, "alias") < 0 && errno == EBADF, "VFAT does not support symlinks");
	} else {
		check(symlinkat("gone", child, "alias") == 0, "create relative symlink");
		check(symlinkat("data", child, "alias") < 0 && errno == EEXIST, "exclusive symlink");
		check(openat(child, "alias", O_RDONLY | O_NOFOLLOW) < 0 && errno == ELOOP, "refuse to open symlink");
		check(fstatat(child, "alias", &relative, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(relative.st_mode),
			"relative stat does not follow links");
		char target[8];
		check(readlinkat(child, "alias", target, sizeof(target)) == 4 && std::string_view(target, 4) == "gone",
			"read relative symlink");
		check(unlinkat(child, "alias", 0) == 0, "remove relative symlink");
	}
	if (linkat(child, "data", child, "hard", 0) == 0)
		check(unlinkat(child, "hard", 0) == 0, "remove relative hard link");
	check(unlinkat(child, "data", 0) == 0, "remove relative file");
	check(close(child) == 0 && fchdir(directory) == 0 && rmdir("fm-probe") == 0 && chdir("/") == 0,
		"remove directory relative to a pinned parent");
	check(close(directory) == 0, "close fixture parent");
}

void directory(const std::string &path)
{
	check(mkdir(path.c_str(), 0755) == 0, path.c_str());
}

void create(const std::string &path, std::string_view data)
{
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	check(fd >= 0, path.c_str());
	while (!data.empty()) {
		const auto count = write(fd, data.data(), data.size());
		check(count > 0, path.c_str());
		data.remove_prefix(count);
	}
	check(close(fd) == 0, "close fixture data");
}

void exact(const std::string &path, std::string_view expected)
{
	const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
	check(fd >= 0, path.c_str());
	std::array<char, 4096> data;
	while (!expected.empty()) {
		const auto count = read(fd, data.data(), std::min(data.size(), expected.size()));
		check(count > 0 && expected.substr(0, count) == std::string_view(data.data(), count), "exact copied bytes");
		expected.remove_prefix(count);
	}
	check(read(fd, data.data(), data.size()) == 0 && close(fd) == 0, "exact copied size");
}

bool absent(const std::string &path)
{
	struct stat info{};
	return lstat(path.c_str(), &info) < 0 && errno == ENOENT;
}

class Observer final : public files::CopyObserver {
public:
	bool cancelled() override { return stop; }
	CopyDecision conflict(std::size_t, std::string_view) override
	{
		++conflicts;
		return decision;
	}
	void progress(std::size_t, uint64_t bytes) override
	{
		written = bytes;
		if (limit && bytes >= limit) {
			if (!foreign.empty())
				create(foreign, "not ours");
			stop = true;
		}
	}
	void result(std::size_t index, files::CopyResult result) override
	{
		check(index == results.size(), "ordered results");
		results.push_back(result);
	}
	void expect(CopyState state, int error = 0, bool partial = false)
	{
		check(!results.empty() && results.front().state == state && results.front().error == error &&
			results.front().partial == partial, "expected copy result");
	}

	std::vector<files::CopyResult> results;
	std::string foreign;
	CopyDecision decision = CopyDecision::skip;
	bool stop = false;
	unsigned conflicts = 0;
	uint64_t limit = 0, written = 0;
};

void worker_conflict(const files::CopyRequest &request)
{
	files::CopyWorker worker;
	check(worker.start(request) == 0, "start helper");
	check(worker.start(request) == EBUSY, "only one active job");
	bool conflict = false, result = false;
	for (;;) {
		pollfd pending{worker.fd(), POLLIN, 0};
		check(poll(&pending, 1, 5000) > 0, "helper event arrives");
		files::CopyEvent event{};
		const auto state = worker.next(event);
		if (state == files::CopyWorker::Read::closed)
			break;
		if (state != files::CopyWorker::Read::event)
			continue;
		if (event.type == files::CopyEventType::conflict) {
			check(!conflict && std::string_view(event.name.data()) == "payload.bin", "helper conflict name");
			conflict = true;
			worker.cancel();
		} else if (event.type == files::CopyEventType::result) {
			check(conflict && !result && event.result.state == CopyState::cancelled, "cancel a waiting helper");
			result = true;
		}
	}
	worker.close_events();
	bool success = false;
	for (unsigned retry = 0; retry < 100 && !worker.reap(success); ++retry)
		usleep(10000);
	check(conflict && result && success && !worker.active() && waitpid(-1, nullptr, WNOHANG) < 0 && errno == ECHILD,
		"worker is reaped");
}

void matrix(const std::string &base, bool vfat)
{
	const auto source = base + "/fm-src", destination = base + "/fm-dst";
	directory(source);
	directory(destination);
	std::string payload(files::copy_chunk * 4 + 187, '\0');
	for (std::size_t index = 0; index < payload.size(); ++index)
		payload[index] = static_cast<char>((index ^ (index >> 8)) & 0xff);
	create(source + "/payload.bin", payload);
	directory(source + "/tree");
	directory(source + "/tree/deeper");
	create(source + "/tree/deeper/big", payload);
	create(source + "/tree/note", "a note");
	create(source + "/empty", "");
	Observer cancelled;
	cancelled.limit = files::copy_chunk;
	files::copy_items({source, destination, {"tree", "empty"}}, cancelled);
	cancelled.expect(CopyState::cancelled);
	check(cancelled.written == files::copy_chunk && cancelled.results.size() == 2 &&
		cancelled.results[1].state == CopyState::cancelled && absent(destination + "/tree") &&
		absent(destination + "/empty"), "cancel at a known chunk and remove only the incomplete tree");
	Observer foreign;
	foreign.limit = files::copy_chunk;
	foreign.foreign = destination + "/tree/not-ours";
	files::copy_items({source, destination, {"tree"}}, foreign);
	foreign.expect(CopyState::cancelled, 0, true);
	exact(foreign.foreign, "not ours");
	check(absent(destination + "/tree/deeper") && unlink(foreign.foreign.c_str()) == 0 &&
		rmdir((destination + "/tree").c_str()) == 0, "rollback does not delete foreign entries");

	create(destination + "/payload.bin", "incumbent");
	create(destination + "/payload (copy 1).bin", "incumbent alternate");
	const files::CopyRequest conflict{source, destination, {"payload.bin"}};
	Observer skipped;
	files::copy_items(conflict, skipped);
	skipped.expect(CopyState::skipped);
	check(skipped.conflicts == 1 && absent(destination + "/payload (copy 2).bin"), "skip creates nothing");
	worker_conflict(conflict);
	Observer copied;
	copied.decision = CopyDecision::keep_both;
	files::copy_items({source, destination, {"payload.bin", "tree", "empty"}}, copied);
	check(copied.results.size() == 3 && std::all_of(copied.results.begin(), copied.results.end(), [](auto result) {
		return result.state == CopyState::copied;
	}), "whole selection copied");
	exact(destination + "/payload.bin", "incumbent");
	exact(destination + "/payload (copy 1).bin", "incumbent alternate");
	exact(destination + "/payload (copy 2).bin", payload);
	exact(destination + "/tree/deeper/big", payload);
	exact(destination + "/tree/note", "a note");
	exact(destination + "/empty", "");
	Observer inside;
	files::copy_items({source, source + "/tree/deeper", {"tree"}}, inside);
	inside.expect(CopyState::failed, EINVAL);
	check(absent(source + "/tree/deeper/tree"), "source cannot contain destination");
	Observer merge;
	files::copy_items({source, destination, {"tree"}}, merge);
	merge.expect(CopyState::skipped);
	exact(destination + "/tree/note", "a note");
	const std::string long_name = "abcdefghijklmnopqrstuvwxy.txt";
	create(source + "/" + long_name, "name limit");
	Observer same_folder;
	same_folder.decision = CopyDecision::keep_both;
	files::copy_items({source, source, {long_name}}, same_folder);
	same_folder.expect(CopyState::copied);
	exact(source + "/" + long_name, "name limit");
	exact(source + (base == "/tmp" ? "/abcdefghijklmnop (copy 1).txt" :
		"/abcdefghijklmnopqrstuvwxy (copy 1).txt"), "name limit");
	if (base != "/tmp") {
		Observer cross_volume;
		files::copy_items({"/tmp/fm-src", destination, {"abcdefghijklmnopqrstuvwxy.txt"}}, cross_volume);
		cross_volume.expect(CopyState::copied);
		exact(destination + "/" + long_name, "name limit");
	}
	if (!vfat) {
		check(symlink("gone", (source + "/broken").c_str()) == 0, "broken source symlink");
		check(symlink("tree", (source + "/alias").c_str()) == 0, "source symlink to directory");
		Observer links;
		files::copy_items({source, destination, {"broken", "alias"}}, links);
		for (const auto &name : {"broken", "alias"}) {
			char target[8];
			check(readlink((destination + "/" + name).c_str(), target, sizeof(target)) == 4 &&
				std::string_view(target, 4) == (std::string_view(name) == "broken" ? "gone" : "tree"),
				"symlink target preserved without traversal");
		}
	}
}

void edge(const std::string &destination, std::string_view mode)
{
	const std::string source = "/tmp/fm-edge";
	if (absent(source)) {
		directory(source);
		create(source + "/fm-new", std::string(files::copy_chunk * 4, 'x'));
		check(symlink("missing", (source + "/fm-link").c_str()) == 0, "create unsupported link fixture");
		check(mkfifo((source + "/fm-fifo").c_str(), 0600) == 0, "create unsupported special file fixture");
	}
	Observer result;
	if (mode.find("-ro") != mode.npos) {
		files::copy_items({source, destination, {"fm-new"}}, result);
		result.expect(CopyState::failed, EROFS);
		check(absent(destination + "/fm-new"), "read-only destination unchanged");
	} else if (mode == "mount") {
		files::copy_items({"/tmp", "/tmp/fm-mount/volume", {"fm-mount"}}, result);
		result.expect(CopyState::failed, EINVAL);
		Observer traversal;
		files::copy_items({"/tmp", destination, {"fm-mount"}}, traversal);
		traversal.expect(CopyState::unsupported, EXDEV);
		check(absent(destination + "/fm-mount"), "nested mount is not copied");
	} else {
		check(mode == "mariofs-full" || mode == "ext2-full" || mode == "vfat-full", "known full fixture");
		if (mode == "ext2-full") {
			const auto long_link = destination + "/fm-long-link";
			check(symlink(std::string(1300, 'x').c_str(), long_link.c_str()) < 0 && errno == ENAMETOOLONG &&
				absent(long_link), "ext2 rejects a target bigger than its symlink block");
		}
		if (mode == "vfat-full") {
			Observer links;
			files::copy_items({source, destination, {"fm-link", "fm-fifo"}}, links);
			links.expect(CopyState::unsupported, ENOTSUP);
			check(links.results.size() == 2 && links.results[1].state == CopyState::unsupported &&
				absent(destination + "/fm-link") && absent(destination + "/fm-fifo"), "unsupported types not flattened");
		}
		const auto reserve = destination + "/fm-reserve";
		const int fd = open(reserve.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
		check(fd >= 0, "create space reservation");
		struct statvfs volume{};
		check(fstatvfs(fd, &volume) == 0, "query available blocks");
		check(uint64_t(volume.f_bavail) * volume.f_frsize <= 2 * 1024 * 1024, "use a small full-volume fixture");
		std::array<char, 64 * 1024> data{};
		while (uint64_t(volume.f_bavail) * volume.f_frsize > 48 * 1024) {
			const auto left = uint64_t(volume.f_bavail) * volume.f_frsize - 48 * 1024;
			const auto count = write(fd, data.data(), std::min<uint64_t>(left, data.size()));
			if (count < 0 && errno == ENOSPC)
				break;
			check(count > 0, "reserve filesystem space");
			check(fstatvfs(fd, &volume) == 0, "recheck available blocks");
		}
		check(close(fd) == 0, "close space reservation");
		files::copy_items({source, destination, {"fm-new"}}, result);
		result.expect(CopyState::failed, ENOSPC);
		check(result.written > 0 && absent(destination + "/fm-new"), "full destination rolls back a partially written file");
		check(unlink(reserve.c_str()) == 0, "release space reservation");
	}
}

}

int main(int argc, char **argv)
{
	check(argc == 3, "directory and filesystem arguments");
	const std::string_view mode = argv[2];
	if (mode == "mariofs" || mode == "ext2" || mode == "vfat") {
		probe(argv[1], mode == "vfat");
		matrix(argv[1], mode == "vfat");
	} else
		edge(argv[1], mode);
	std::printf("filemanager copy: %s passed\n", argv[2]);
}
