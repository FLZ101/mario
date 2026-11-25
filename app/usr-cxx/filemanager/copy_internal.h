#ifndef MARIO_FILEMANAGER_COPY_INTERNAL_H
#define MARIO_FILEMANAGER_COPY_INTERNAL_H

#include "copy.h"

#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

namespace files::detail {

constexpr std::size_t path_limit = 2047;
constexpr int directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

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

inline CopyResult failure(int error) { return {CopyState::failed, error}; }
constexpr CopyResult copied{CopyState::copied};
constexpr CopyResult skipped{CopyState::skipped};
constexpr CopyResult cancelled{CopyState::cancelled};

bool component(std::string_view name);
Fd open_directory(std::string_view path);
bool matches(int fd, const Identity &expected);
int check_ancestors(int fd, const Identity &source);
ssize_t read_chunk(int fd, char *data, std::size_t size);
bool read_exact(int fd, char *data, std::size_t size);
int erase(int parent, const std::string &name, mode_t type, const Fd &cwd);

struct TreeJob {
	int source, destination;
	bool remove_source, offer_conflict;

	CopyResult run(CopyObserver &observer, std::size_t index, const std::string &name,
		       const std::string &output, const std::function<int()> &before_remove = {}) const;
};

}

#endif
