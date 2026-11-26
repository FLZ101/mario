#include "telemetry.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <mario/netctl.h>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace monitor {
namespace {

constexpr std::size_t proc_limit = 64 * 1024;
constexpr std::size_t task_limit = 4096;

template <typename Number>
bool integer(std::string_view text, Number &value)
{
	if (text.empty())
		return false;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
	return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

std::string_view word(std::string_view &text)
{
	const auto start = text.find_first_not_of(" \t");
	if (start == std::string_view::npos) {
		text = {};
		return {};
	}
	text.remove_prefix(start);
	const auto end = text.find_first_of(" \t");
	const auto result = text.substr(0, end);
	text.remove_prefix(result.size());
	return result;
}

template <typename Parse>
Status lines(const Text &text, Parse parse)
{
	if (text.status == Status::unavailable)
		return text.status;
	bool malformed = false;
	std::string_view remaining(text.bytes);
	while (!remaining.empty()) {
		const auto end = remaining.find('\n');
		if (end == std::string_view::npos && text.status != Status::complete)
			break;
		malformed |= !parse(remaining.substr(0, end));
		if (end == std::string_view::npos)
			break;
		remaining.remove_prefix(end + 1);
	}
	return malformed ? Status::partial : text.status;
}

std::optional<uint64_t> sum(uint64_t left, uint64_t right)
{
	if (left > UINT64_MAX - right)
		return {};
	return left + right;
}

std::optional<uint64_t> delta(uint64_t user, uint64_t system, uint64_t previous_user,
			      uint64_t previous_system)
{
	if (user < previous_user || system < previous_system)
		return {};
	return sum(user - previous_user, system - previous_system);
}

std::optional<double> uptime(std::string_view text)
{
	text = word(text);
	const auto dot = text.find('.');
	uint64_t whole, fraction;
	if (!integer(text.substr(0, dot), whole))
		return {};
	if (dot == std::string_view::npos)
		return double(whole);
	const auto fractional = text.substr(dot + 1);
	if (fractional.size() > 9 || !integer(fractional, fraction))
		return {};
	double divisor = 1;
	for (std::size_t digit = 0; digit < fractional.size(); ++digit)
		divisor *= 10;
	return double(whole) + fraction / divisor;
}

std::optional<Task> parse_task(std::string_view text, int pid)
{
	const auto open = text.find('('), close = text.rfind(')');
	int read_pid;
	if (open == std::string_view::npos || close == std::string_view::npos || close <= open ||
	    open < 2 || text[open - 1] != ' ' || !integer(text.substr(0, open - 1), read_pid) || read_pid != pid)
		return {};
	Task task{};
	task.identity.pid = pid;
	task.name = text.substr(open + 1, close - open - 1);
	text.remove_prefix(close + 1);
	const auto state = word(text);
	if (state.size() != 1 || !integer(word(text), task.parent) || task.parent < 0)
		return {};
	task.state = state.front();
	for (int field = 5; field < 14; ++field)
		word(text);
	if (!integer(word(text), task.user) || !integer(word(text), task.system))
		return {};
	for (int field = 16; field < 22; ++field)
		word(text);
	if (!integer(word(text), task.identity.start_ticks))
		return {};
	return task;
}

std::optional<Mount> parse_mount(std::string_view line)
{
	const auto source = word(line);
	if (source.empty() || line.size() < 2 || line[1] != '/')
		return {};
	line.remove_prefix(1);
	std::array<std::string_view, 4> suffix;
	for (auto part = suffix.rbegin(); part != suffix.rend(); ++part) {
		const auto separator = line.rfind(' ');
		if (separator == std::string_view::npos || separator + 1 == line.size())
			return {};
		*part = line.substr(separator + 1);
		line = line.substr(0, separator);
	}
	if (line.empty() || (suffix[1] != "ro" && suffix[1] != "rw") || suffix[2] != "0" || suffix[3] != "0")
		return {};
	return Mount{std::string(source), std::string(line), std::string(suffix[0]), std::string(suffix[1]), {}};
}

}

Text ProcSource::read(const std::string &path, std::size_t limit)
{
	const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return {};
	Text text;
	std::array<char, 2048> buffer;
	while (text.bytes.size() <= limit) {
		const auto length = std::min(limit + 1 - text.bytes.size(), buffer.size());
		const auto count = ::read(descriptor, buffer.data(), length);
		if (count > 0) {
			text.bytes.append(buffer.data(), count);
		} else if (!count) {
			text.status = Status::complete;
			break;
		} else if (errno != EINTR) {
			break;
		}
	}
	close(descriptor);
	if (text.bytes.size() > limit)
		text.bytes.resize(limit);
	if (!text.bytes.empty() && text.status != Status::complete)
		text.status = Status::partial;
	return text;
}

Pids ProcSource::tasks(std::size_t limit)
{
	DIR *directory = opendir("/proc");
	if (!directory)
		return {};
	Pids result;
	std::size_t entries = 0;
	for (;;) {
		errno = 0;
		const auto *entry = readdir(directory);
		if (!entry) {
			result.status = errno ? Status::partial : Status::complete;
			break;
		}
		if (++entries > limit + 32) {
			result.status = Status::partial;
			break;
		}
		int pid;
		if (!integer(entry->d_name, pid) || pid <= 0)
			continue;
		if (result.values.size() == limit) {
			result.status = Status::partial;
			break;
		}
		result.values.push_back(pid);
	}
	closedir(directory);
	return result;
}

Space ProcSource::space(const std::string &path)
{
	struct statvfs info{};
	if (statvfs(path.c_str(), &info) < 0)
		return {{}, errno};
	const uint64_t unit = info.f_frsize ? info.f_frsize : info.f_bsize;
	if (!unit || info.f_blocks > UINT64_MAX / unit || info.f_bavail > info.f_blocks)
		return {{}, ERANGE};
	return {Capacity{info.f_blocks * unit, info.f_bavail * unit}, 0};
}

std::optional<uint64_t> ProcSource::monotonic_ns()
{
	timespec now{};
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0 || now.tv_sec < 0 ||
	    uint64_t(now.tv_sec) > (UINT64_MAX - now.tv_nsec) / 1000000000)
		return {};
	return uint64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
}

std::optional<Network> ProcSource::network()
{
	const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return {};
	mario_net_stats stats{};
	stats.version = MARIO_NET_STATS_VERSION;
	const int result = ioctl(fd, MARIO_NET_GET_STATS, &stats);
	close(fd);
	if (result < 0)
		return {};
	return Network{stats.flags, stats.address, stats.prefix, stats.rx_bytes, stats.tx_bytes, {}, {}};
}

Snapshot sample(Source &source)
{
	Snapshot result;
	result.monotonic_ns = source.monotonic_ns();
	result.network = source.network();
	result.cpu_status = lines(source.read("/proc/stat", proc_limit), [&](std::string_view row) {
		const auto label = word(row);
		if (label.substr(0, 3) != "cpu")
			return true;
		CPU cpu{-1, 0, 0, 0, {}};
		uint64_t nice;
		if ((label != "cpu" && (!integer(label.substr(3), cpu.core) || cpu.core < 0)) ||
		    !integer(word(row), cpu.user) || !integer(word(row), nice) ||
		    !integer(word(row), cpu.system) || !integer(word(row), cpu.idle) ||
		    std::any_of(result.cpus.begin(), result.cpus.end(), [&](const CPU &seen) { return seen.core == cpu.core; }))
			return false;
		result.cpus.push_back(cpu);
		return true;
	});
	std::sort(result.cpus.begin(), result.cpus.end(), [](const CPU &left, const CPU &right) { return left.core < right.core; });
	if (result.cpu_status != Status::unavailable && result.cpus.empty())
		result.cpu_status = Status::invalid;
	std::optional<uint64_t> free_kib;
	lines(source.read("/proc/meminfo", 4096), [&](std::string_view row) {
		const auto label = word(row);
		uint64_t kib;
		if (!integer(word(row), kib) || word(row) != "kB")
			return false;
		if (label == "MemTotal:" && kib) result.memory.total_kib = kib;
		if (label == "MemFree:") free_kib = kib;
		if (label == "Cached:") result.memory.cached_kib = kib;
		return true;
	});
	if (free_kib && result.memory.total_kib && *free_kib <= *result.memory.total_kib)
		result.memory.used_kib = *result.memory.total_kib - *free_kib;
	const auto up = source.read("/proc/uptime", 128);
	if (up.status == Status::complete)
		result.uptime_seconds = uptime(up.bytes);
	const auto pids = source.tasks(task_limit);
	result.task_status = pids.status;
	for (int pid : pids.values) {
		const auto path = "/proc/" + std::to_string(pid);
		const auto stat = source.read(path + "/stat", 1024);
		auto task = stat.status == Status::complete ? parse_task(stat.bytes, pid) : std::optional<Task>{};
		if (!task) {
			result.task_status = Status::partial;
			continue;
		}
		auto comm = source.read(path + "/comm", 256);
		if (comm.status == Status::complete && !comm.bytes.empty() && comm.bytes.back() == '\n') {
			comm.bytes.pop_back();
			if (comm.bytes != task->name)
				result.task_status = Status::partial;
		}
		const auto status = source.read(path + "/status", 256);
		if (status.status == Status::complete)
			lines(status, [&](std::string_view row) {
				if (word(row) == "Tgid:")
					return integer(word(row), task->tgid);
				return true;
			});
		result.tasks.push_back(std::move(*task));
	}
	std::sort(result.tasks.begin(), result.tasks.end(), [](const Task &left, const Task &right) {
		return left.identity.pid < right.identity.pid;
	});
	const auto mounts = source.read("/proc/mounts", proc_limit);
	result.mount_status = lines(mounts, [&](std::string_view row) {
		auto mount = parse_mount(row);
		if (!mount)
			return false;
		mount->space = source.space(mount->path);
		result.mounts.push_back(std::move(*mount));
		return true;
	});
	if (!result.mounts.empty()) {
		const auto checked = source.read("/proc/mounts", proc_limit);
		if (checked.bytes != mounts.bytes || checked.status != mounts.status) {
			std::vector<Mount> live;
			lines(checked, [&](std::string_view row) {
				auto mount = parse_mount(row);
				if (mount) live.push_back(std::move(*mount));
				return bool(mount);
			});
			for (auto &mount : result.mounts) {
				const bool unchanged = std::any_of(live.begin(), live.end(), [&](const Mount &row) {
					return row.path == mount.path && row.source == mount.source &&
					       row.filesystem == mount.filesystem && row.options == mount.options;
				});
				if (!unchanged)
					mount.space = {{}, ESTALE};
			}
			result.mount_status = Status::partial;
		}
	}
	return result;
}

void derive_rates(Snapshot &current, const Snapshot &previous)
{
	const bool fresh = current.monotonic_ns && previous.monotonic_ns &&
		*current.monotonic_ns > *previous.monotonic_ns && *current.monotonic_ns - *previous.monotonic_ns <= stale_ns;
	if (current.network) {
		auto &network = *current.network;
		network.rx_per_second.reset();
		network.tx_per_second.reset();
		constexpr uint32_t connected = MARIO_NET_PRESENT | MARIO_NET_UP | MARIO_NET_CARRIER;
		if (fresh && previous.network && (network.flags & connected) == connected &&
		    (previous.network->flags & connected) == connected) {
			const double seconds = (*current.monotonic_ns - *previous.monotonic_ns) / 1e9;
			if (network.rx_bytes >= previous.network->rx_bytes)
				network.rx_per_second = (network.rx_bytes - previous.network->rx_bytes) / seconds;
			if (network.tx_bytes >= previous.network->tx_bytes)
				network.tx_per_second = (network.tx_bytes - previous.network->tx_bytes) / seconds;
		}
	}
	std::optional<uint64_t> machine;
	for (auto &cpu : current.cpus) {
		cpu.busy_percent.reset();
		if (!fresh)
			continue;
		const auto older = std::lower_bound(previous.cpus.begin(), previous.cpus.end(), cpu.core,
			[](const CPU &row, int core) { return row.core < core; });
		if (older == previous.cpus.end() || older->core != cpu.core || cpu.idle < older->idle)
			continue;
		const auto busy = delta(cpu.user, cpu.system, older->user, older->system);
		const auto total = busy ? sum(*busy, cpu.idle - older->idle) : std::optional<uint64_t>{};
		if (total && *total) {
			cpu.busy_percent = 100.0 * *busy / *total;
			if (cpu.core < 0)
				machine = total;
		}
	}
	for (auto &task : current.tasks) {
		task.machine_percent.reset();
		if (!machine)
			continue;
		const auto pid = task.identity.pid;
		const auto older = std::lower_bound(previous.tasks.begin(), previous.tasks.end(), pid,
			[](const Task &row, int key) { return row.identity.pid < key; });
		if (older == previous.tasks.end() || !(older->identity == task.identity))
			continue;
		const auto busy = delta(task.user, task.system, older->user, older->system);
		if (busy && *busy <= *machine)
			task.machine_percent = 100.0 * *busy / *machine;
	}
}

}
