#include "copy.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using files::CopyAction;
using files::CopyDecision;
using files::CopyRequest;
using files::CopyResult;
using files::CopyState;

void check(bool success, const char *expectation)
{
	if (!success) {
		std::fprintf(stderr, "filemanager move: %s (errno %d)\n", expectation, errno);
		std::exit(1);
	}
}

void directory(const std::string &path)
{
	check(mkdir(path.c_str(), 0755) == 0, "create fixture directory");
}

void write_file(const std::string &path, const std::string &data, int flags = O_CREAT | O_EXCL)
{
	const int fd = open(path.c_str(), O_WRONLY | flags, 0644);
	check(fd >= 0, "open file for fixture write");
	check(write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()), "write fixture bytes");
	check(close(fd) == 0, "close fixture file");
}

void exact(const std::string &path, const std::string &expected)
{
	const int fd = open(path.c_str(), O_RDONLY);
	check(fd >= 0, "open expected file");
	std::string data(expected.size() + 1, '\0');
	check(read(fd, data.data(), data.size()) == static_cast<ssize_t>(expected.size()) &&
	      std::string_view(data.data(), expected.size()) == expected, "exact file bytes");
	check(close(fd) == 0, "close expected file");
}

bool absent(const std::string &path)
{
	struct stat info{};
	return lstat(path.c_str(), &info) < 0 && errno == ENOENT;
}

class Observer final : public files::CopyObserver {
public:
	bool cancelled() override { return stopping; }
	CopyDecision conflict(std::size_t, std::string_view) override { ++conflicts; return decision; }
	void progress(std::size_t, uint64_t bytes) override { if (hook) hook(bytes); }
	void result(std::size_t, CopyResult result) override { results.push_back(result); }

	void run(const CopyRequest &request, CopyState state = CopyState::copied, int error = 0)
	{
		results.clear();
		files::copy_items(request, *this);
		check(results.size() == request.names.size(), "one result per selection");
		if (results.front().state != state || results.front().error != error)
			std::fprintf(stderr, "action %u on %s/%s: got state %u, error %d; expected state %u, error %d\n",
				unsigned(request.action), request.source.c_str(), request.names.front().c_str(),
				unsigned(results.front().state), results.front().error, unsigned(state), error);
		check(results.front().state == state && results.front().error == error && !results.front().partial,
		      "expected result without partial output");
	}

	std::function<void(uint64_t)> hook;
	std::vector<CopyResult> results;
	CopyDecision decision = CopyDecision::skip;
	unsigned conflicts = 0;
	bool stopping = false;
};

void moves(const std::string &target)
{
	const std::string source = "/tmp/" + target.substr(target.rfind('/') + 1) + "-src";
	directory(source);
	Observer job;
	job.run({source, {}, {"tree"}, CopyAction::mkdir});
	job.run({source, {}, {"tree"}, CopyAction::mkdir}, CopyState::failed, EEXIST);
	job.run({source, {}, {"../escape"}, CopyAction::mkdir}, CopyState::failed, EINVAL);
	job.run({source, source, {"tree"}, CopyAction::move}, CopyState::failed, EINVAL);
	directory(source + "/tree/nested");
	write_file(source + "/tree/nested/bytes", "cross-volume data\n");
	write_file(source + "/tree/empty", "");
	job.run({source, target, {"tree"}, CopyAction::move});
	exact(target + "/tree/nested/bytes", "cross-volume data\n");
	exact(target + "/tree/empty", "");
	check(absent(source + "/tree"), "remove source only after complete cross-volume copy");
	job.run({target, "renamed", {"tree"}, CopyAction::rename});
	check(absent(target + "/tree"), "old directory name removed");
	exact(target + "/renamed/nested/bytes", "cross-volume data\n");
	write_file(target + "/old", "source");
	write_file(target + "/new", "incumbent");
	job.run({target, "new", {"old"}, CopyAction::rename}, CopyState::failed, EEXIST);
	check(!job.conflicts, "rename never offers a different name");
	exact(target + "/old", "source");
	exact(target + "/new", "incumbent");
	write_file(source + "/new", "source");
	job.run({source, target, {"new"}, CopyAction::move}, CopyState::skipped);
	check(job.conflicts == 1, "move asks about an incumbent");
	exact(source + "/new", "source");
	exact(target + "/new", "incumbent");
	job.decision = CopyDecision::keep_both;
	job.run({source, target, {"new"}, CopyAction::move});
	exact(target + "/new (copy 1)", "source");
	check(absent(source + "/new"), "keep both completes the move");
	job.run({target, {}, {"renamed"}, CopyAction::remove});
	check(absent(target + "/renamed"), "permanent delete removes a tree");
	check(symlink("missing", (source + "/link").c_str()) == 0, "contained source symlink");
	std::array<char, 16> link{};
	if (target == "/mnt/fm-vfat") {
		job.run({source, target, {"link"}, CopyAction::move}, CopyState::unsupported, ENOTSUP);
		check(absent(target + "/link") && readlink((source + "/link").c_str(), link.data(), link.size()) == 7,
		      "unsupported symlink remains on source volume");
	} else {
		job.run({source, target, {"link"}, CopyAction::move});
		check(absent(source + "/link") && readlink((target + "/link").c_str(), link.data(), link.size()) == 7 &&
		      std::string_view(link.data(), 7) == "missing", "broken target moved as a symlink");
	}
}

void interrupted()
{
	const std::string source = "/tmp/fm-source", target = "/tmp/fm-target";
	directory(source);
	directory(target);
	Observer job;
	const std::string large(files::copy_chunk * 3, 'x');
	write_file(source + "/large", large);
	job.hook = [&](uint64_t bytes) { if (bytes >= files::copy_chunk) job.stopping = true; };
	job.run({source, target, {"large"}, CopyAction::move}, CopyState::cancelled);
	exact(source + "/large", large);
	check(absent(target + "/large"), "cancel removes job-owned output only");

	job.stopping = false;
	bool changed = false;
	job.hook = [&](uint64_t bytes) {
		if (bytes == large.size() && !changed) {
			changed = true;
			const auto path = source + "/large";
			struct stat previous{};
			check(stat(path.c_str(), &previous) == 0, "capture source timestamps");
			write_file(path, "y", 0);
			const timespec times[] = {previous.st_atim, previous.st_mtim};
			check(utimensat(AT_FDCWD, path.c_str(), times, 0) == 0, "restore source mtime");
		}
	};
	job.run({source, target, {"large"}, CopyAction::move}, CopyState::failed, ESTALE);
	exact(source + "/large", "y" + large.substr(1));
	check(absent(target + "/large"), "same-size modified source is not removed");

	directory(source + "/tree");
	write_file(source + "/tree/first", large);
	write_file(source + "/tree/second", "source second");
	bool injected = false;
	job.hook = [&](uint64_t bytes) {
		if (bytes == large.size() && !injected) {
			injected = true;
			write_file(target + "/tree/second", "external incumbent");
		}
	};
	files::copy_items({source, target, {"tree"}, CopyAction::move}, job);
	const auto skipped = job.results.back();
	check(injected && skipped.state == CopyState::skipped && skipped.partial && !skipped.source_remaining &&
	      !skipped.destination_complete, "skipped descendant retains the entire source");
	exact(source + "/tree/first", large);
	exact(source + "/tree/second", "source second");
	exact(target + "/tree/second", "external incumbent");
	check(absent(target + "/tree/first"), "rollback leaves the incumbent only");

	job.hook = {};
	check(mkfifo((source + "/tree/special").c_str(), 0600) == 0, "create unsupported descendant");
	check(rmdir((target + "/tree").c_str()) < 0 && unlink((target + "/tree/second").c_str()) == 0 &&
	      rmdir((target + "/tree").c_str()) == 0, "remove conflict fixture");
	job.run({source, target, {"tree"}, CopyAction::move}, CopyState::unsupported, ENOTSUP);
	check(absent(target + "/tree") && !absent(source + "/tree/special"), "unsupported descendant preserves the source tree");
	exact(source + "/tree/first", large);

	directory(source + "/busy");
	write_file(source + "/busy/data", "completed destination survives");
	const int pinned = open((source + "/busy").c_str(), O_DIRECTORY | O_RDONLY);
	check(pinned >= 0, "keep the MarioFS directory busy during removal");
	job.run({source, target, {"busy"}, CopyAction::move}, CopyState::failed, EBUSY);
	const auto retained = job.results.front();
	check(retained.destination_complete && retained.source_remaining == 1 &&
	      std::string_view(retained.first_remaining.data()) == "busy", "report the remaining source directory");
	exact(target + "/busy/data", "completed destination survives");
	check(absent(source + "/busy/data") && close(pinned) == 0, "child was removed before the busy directory");
}

std::string find_slot(const std::string &store)
{
	DIR *entries = opendir(store.c_str());
	check(entries != nullptr, "open volume trash");
	std::string name;
	while (const auto *entry = readdir(entries)) {
		if (std::string_view(entry->d_name).substr(0, 5) != "item-")
			continue;
		check(name.empty(), "one exclusive trash slot");
		name = entry->d_name;
	}
	check(closedir(entries) == 0 && !name.empty(), "find trash slot");
	return name;
}

void trash(const std::string &volume)
{
	const auto source = (volume == "/" ? "/tmp" : volume) + std::string("/fm-trash");
	directory(source);
	const auto path = source + "/payload";
	write_file(path, "trash bytes");
	Observer job;
	const auto trash_path = (volume == "/" ? "" : volume) + "/.mario-trash";
	directory(trash_path);
	job.run({source, {}, {"payload"}, CopyAction::trash}, CopyState::failed, EPERM);
	exact(path, "trash bytes");
	check(rmdir(trash_path.c_str()) == 0, "unowned directory is not used as Trash");
	job.run({source, {}, {"payload"}, CopyAction::trash});
	check(absent(path), "trash removed original after verification");
	std::string store;
	check(files::trash_directory(source, store) == 0 && store == trash_path,
	      "trash stays on its volume");
	const auto name = find_slot(store);
	const auto slot = store + "/" + name;
	files::TrashItem info;
	check(files::trash_item(store, name, info) == 0 && info.ready && info.has_payload && info.original == path,
	      "trash records exact original path and ready marker");
	exact(slot + "/item", "trash bytes");
	job.run({volume, {}, {".mario-trash"}, CopyAction::remove}, CopyState::failed, EPERM);
	write_file(path, "incumbent");
	job.run({store, path, {name}, CopyAction::restore}, CopyState::skipped);
	exact(path, "incumbent");
	exact(slot + "/item", "trash bytes");
	check(unlink((slot + "/ready").c_str()) == 0, "simulate interrupted trash");
	check(files::trash_item(store, name, info) == 0 && !info.ready && info.has_payload && info.original == path,
	      "missing commit marker exposes an incomplete slot");
	job.run({store, path, {name}, CopyAction::restore}, CopyState::failed, ESTALE);
	job.decision = CopyDecision::keep_both;
	job.run({store, source, {name}, CopyAction::recover});
	exact(path, "incumbent");
	exact(source + "/payload (copy 1)", "trash bytes");
	exact(slot + "/item", "trash bytes");
	write_file(slot + "/ready", "");
	job.run({store, path, {name}, CopyAction::restore});
	exact(source + "/payload (copy 2)", "trash bytes");
	check(absent(slot), "successful restore cleaned its slot");
	job.run({source, {}, {"payload"}, CopyAction::trash});
	const auto discard = find_slot(store);
	job.run({store, {}, {discard}, CopyAction::remove});
	check(absent(store + "/" + discard) && absent(path), "purge a single slot without touching the store");
	if (volume == "/") {
		const auto tree = source + "/tree";
		directory(tree);
		write_file(tree + "/last", "complete restored copy");
		job.run({source, {}, {"tree"}, CopyAction::trash});
		const auto busy = find_slot(store);
		const auto saved_tree = store + "/" + busy + "/item";
		const int pinned = open(saved_tree.c_str(), O_DIRECTORY | O_RDONLY);
		check(pinned >= 0, "pin the saved directory during restore");
		job.run({store, tree, {busy}, CopyAction::restore}, CopyState::failed, EBUSY);
		check(files::trash_item(store, busy, info) == 0 && !info.ready && info.has_payload && close(pinned) == 0,
		      "a partially removed payload is never offered as a full restore");
		exact(tree + "/last", "complete restored copy");
		job.run({store, {}, {busy}, CopyAction::remove});
	}
}

void protection(const std::string &mount)
{
	Observer job;
	job.run({"/", {}, {"/"}, CopyAction::remove}, CopyState::failed, EINVAL);
	for (const auto action : {CopyAction::remove, CopyAction::trash, CopyAction::rename})
		job.run({"/", "moved", {"mnt"}, action}, CopyState::unsupported, EXDEV);
	directory("/tmp/fm-protect");
	directory("/tmp/fm-protect/dest");
	job.run({"/tmp", "/tmp/fm-protect/dest", {"fm-protect"}, CopyAction::move}, CopyState::failed, EINVAL);
	write_file("/tmp/fm-protect/new", "do not delete");
	job.run({"/tmp/fm-protect", mount, {"new"}, CopyAction::move}, CopyState::failed, EROFS);
	exact("/tmp/fm-protect/new", "do not delete");
	job.run({mount, {}, {"original"}, CopyAction::trash}, CopyState::failed, EROFS);
	exact(mount + "/original", "read-only source");
}

}

int main(int argc, char **argv)
{
	check(argc >= 2, "test mode");
	const std::string_view mode = argv[1];
	if (mode == "moves" && argc == 3)
		moves(argv[2]);
	else if (mode == "interrupted")
		interrupted();
	else if (mode == "trash" && argc == 3)
		trash(argv[2]);
	else if (mode == "protected" && argc == 3)
		protection(argv[2]);
	else
		check(false, "known test mode");
	std::printf("filemanager move: %s%s%s passed\n", argv[1], argc == 3 ? " " : "", argc == 3 ? argv[2] : "");
}
