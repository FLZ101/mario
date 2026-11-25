#include "sampler.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>

namespace monitor {
namespace {

constexpr std::size_t transfer_limit = 8 * 1024 * 1024;

template <class Archive, class... Fields>
bool field(Archive &archive, Fields &...values)
{
	return (archive.value(values) && ...);
}

template <class Archive> bool record(Archive &io, CPU &cpu)
{
	return field(io, cpu.core, cpu.user, cpu.system, cpu.idle);
}

template <class Archive> bool record(Archive &io, Memory &memory)
{
	return field(io, memory.total_kib, memory.used_kib, memory.cached_kib);
}

template <class Archive> bool record(Archive &io, Task &task)
{
	return field(io, task.identity.pid, task.identity.start_ticks, task.parent, task.name,
		task.state, task.user, task.system, task.tgid);
}

template <class Archive> bool record(Archive &io, Mount &mount)
{
	return field(io, mount.source, mount.path, mount.filesystem, mount.options,
		mount.space.capacity, mount.space.error);
}

template <class Archive> bool record(Archive &io, Capacity &capacity)
{
	return field(io, capacity.total_bytes, capacity.available_bytes);
}

template <class Archive> bool record(Archive &io, Snapshot &snapshot)
{
	return field(io, snapshot.monotonic_ns, snapshot.cpus, snapshot.memory, snapshot.uptime_seconds,
		snapshot.tasks, snapshot.mounts, snapshot.cpu_status, snapshot.task_status, snapshot.mount_status);
}

template <class Stream> struct Archive {
	Stream &stream;

	template <class Value> bool value(Value &item)
	{
		if constexpr (std::is_arithmetic_v<Value> || std::is_enum_v<Value>)
			return stream.transfer(&item, sizeof(item));
		else
			return record(*this, item);
	}

	template <class Value> bool value(std::optional<Value> &item)
	{
		uint8_t present = item.has_value();
		if (!value(present) || present > 1)
			return false;
		if (!present) {
			item.reset();
			return true;
		}
		if (!item)
			item.emplace();
		return value(*item);
	}

	bool value(std::string &item)
	{
		uint32_t count = item.size();
		if (!value(count) || count > transfer_limit)
			return false;
		item.resize(count);
		return stream.transfer(item.data(), count);
	}

	template <class Value> bool value(std::vector<Value> &items)
	{
		uint32_t count = items.size();
		if (!value(count) || count > transfer_limit / sizeof(Value))
			return false;
		items.resize(count);
		for (auto &item : items)
			if (!value(item))
				return false;
		return true;
	}
};

struct Encoder {
	std::string bytes;

	bool transfer(const void *value, std::size_t size)
	{
		if (size > transfer_limit - bytes.size())
			return false;
		bytes.append(static_cast<const char *>(value), size);
		return true;
	}
};

struct Decoder {
	std::string_view bytes;

	bool transfer(void *value, std::size_t size)
	{
		if (size > bytes.size())
			return false;
		std::memcpy(value, bytes.data(), size);
		bytes.remove_prefix(size);
		return true;
	}
};

int collect(int output, const Sampler &sampler)
{
	auto snapshot = sampler();
	Encoder encoder;
	if (!Archive<Encoder>{encoder}.value(snapshot))
		return 1;
	std::string_view left = encoder.bytes;
	while (!left.empty()) {
		const auto written = write(output, left.data(), left.size());
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return 1;
		left.remove_prefix(written);
	}
	return 0;
}

}

SampleWorker::~SampleWorker()
{
	stop();
}

bool SampleWorker::start(const Sampler &sampler)
{
	if (active())
		return false;
	int events[2];
	if (pipe2(events, O_CLOEXEC) < 0)
		return false;
	if (fcntl(events[0], F_SETFL, O_NONBLOCK) < 0) {
		close(events[0]);
		close(events[1]);
		return false;
	}
	struct rlimit limit{};
	const auto max_fd = getrlimit(RLIMIT_NOFILE, &limit) == 0 ? limit.rlim_max : 256;
	pid_ = fork();
	if (!pid_) {
		const char name[16] = "monitor-sampler";
		prctl(PR_SET_NAME, name, 0UL, 0UL, 0UL);
		for (int fd = 0; fd < static_cast<int>(max_fd); ++fd)
			if (fd != events[1])
				close(fd);
		_exit(collect(events[1], sampler));
	}
	close(events[1]);
	if (pid_ < 0) {
		close(events[0]);
		return false;
	}
	fd_ = events[0];
	bytes_.clear();
	pipe_ok_ = false;
	return true;
}

bool SampleWorker::drain()
{
	char buffer[8192];
	for (int batch = 0; batch < 8; ++batch) {
		const auto count = read(fd_, buffer, sizeof(buffer));
		if (count < 0 && (errno == EINTR || errno == EAGAIN))
			return false;
		if (count <= 0 || static_cast<std::size_t>(count) > transfer_limit - bytes_.size()) {
			pipe_ok_ = count == 0;
			return true;
		}
		bytes_.append(buffer, count);
	}
	return false;
}

bool SampleWorker::finish(std::optional<Snapshot> &result)
{
	close_pipe();
	int status;
	const auto waited = waitpid(pid_, &status, WNOHANG);
	if (!waited || (waited < 0 && errno == EINTR))
		return false;
	pid_ = -1;
	if (pipe_ok_ && waited > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		Decoder decoder{bytes_};
		result.emplace();
		if (!Archive<Decoder>{decoder}.value(*result) || !decoder.bytes.empty())
			result.reset();
	}
	bytes_.clear();
	return true;
}

void SampleWorker::close_pipe()
{
	if (fd_ >= 0) {
		close(fd_);
		fd_ = -1;
	}
}

void SampleWorker::stop()
{
	close_pipe();
	if (active()) {
		kill(pid_, SIGKILL);
		while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) { }
		pid_ = -1;
	}
}

}
