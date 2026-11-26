#ifndef MARIO_SYSTEM_MONITOR_TELEMETRY_H
#define MARIO_SYSTEM_MONITOR_TELEMETRY_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace monitor {

constexpr uint64_t stale_ns = 5 * 1000000000ULL;

enum class Status { unavailable, complete, partial, invalid };

struct Text {
	std::string bytes;
	Status status = Status::unavailable;
};

struct Pids {
	std::vector<int> values;
	Status status = Status::unavailable;
};

struct Capacity {
	uint64_t total_bytes, available_bytes;
};

struct Space {
	std::optional<Capacity> capacity;
	int error = 0;
};

struct Network {
	uint32_t flags, address, prefix;
	uint64_t rx_bytes, tx_bytes;
	std::optional<double> rx_per_second, tx_per_second;
};

class Source {
public:
	virtual ~Source() = default;
	virtual Text read(const std::string &path, std::size_t limit) = 0;
	virtual Pids tasks(std::size_t limit) = 0;
	virtual Space space(const std::string &path) = 0;
	virtual std::optional<Network> network() = 0;
	virtual std::optional<uint64_t> monotonic_ns() = 0;
};

class ProcSource final : public Source {
public:
	Text read(const std::string &path, std::size_t limit) override;
	Pids tasks(std::size_t limit) override;
	Space space(const std::string &path) override;
	std::optional<Network> network() override;
	std::optional<uint64_t> monotonic_ns() override;
};

struct CPU {
	int core;
	uint64_t user, system, idle;
	std::optional<double> busy_percent;
};

struct Memory {
	std::optional<uint64_t> total_kib, used_kib, cached_kib;
};

struct Identity {
	int pid;
	uint64_t start_ticks;
	bool operator==(const Identity &other) const
	{
		return pid == other.pid && start_ticks == other.start_ticks;
	}
};

struct Task {
	Identity identity;
	int parent;
	std::string name;
	char state;
	uint64_t user, system;
	std::optional<double> machine_percent;
	int tgid = 0;
};

struct Mount {
	std::string source, path, filesystem, options;
	Space space;
};

struct Snapshot {
	std::optional<uint64_t> monotonic_ns;
	std::vector<CPU> cpus;
	Memory memory;
	std::optional<Network> network;
	std::optional<double> uptime_seconds;
	std::vector<Task> tasks;
	std::vector<Mount> mounts;
	Status cpu_status = Status::unavailable, task_status = Status::unavailable,
	       mount_status = Status::unavailable;
};

Snapshot sample(Source &source);
void derive_rates(Snapshot &current, const Snapshot &previous);

}

#endif
