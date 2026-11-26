#include "telemetry.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mario/netctl.h>
#include <unistd.h>

namespace {

using namespace monitor;

class Fixture final : public Source {
public:
	std::map<std::string, Text> files;
	std::vector<int> pids;
	std::optional<Network> counters;
	uint64_t now = 1000000000;
	bool unmount = false;
	int mount_reads = 0;

	Text read(const std::string &path, std::size_t limit) override
	{
		if (path == "/proc/mounts" && ++mount_reads == 2 && unmount)
			return {"mariofs / mariofs rw 0 0\n", Status::complete};
		const auto found = files.find(path);
		if (found == files.end())
			return {};
		auto text = found->second;
		if (text.bytes.size() > limit) {
			text.bytes.resize(limit);
			text.status = Status::partial;
		}
		return text;
	}
	Pids tasks(std::size_t limit) override
	{
		assert(pids.size() <= limit);
		return {pids, Status::complete};
	}
	Space space(const std::string &path) override
	{
		if (path == "/ram/missing")
			return {{}, ENOENT};
		return {Capacity{800, 300}, 0};
	}
	std::optional<uint64_t> monotonic_ns() override { return now; }
	std::optional<Network> network() override { return counters; }
	void file(const std::string &path, const std::string &bytes)
	{
		files[path] = {bytes, Status::complete};
	}
	void task(int pid, const std::string &name, unsigned user, unsigned system, unsigned start)
	{
		const auto base = "/proc/" + std::to_string(pid);
		file(base + "/comm", name + "\n");
		file(base + "/stat", std::to_string(pid) + " (" + name + ") R 1 0 0 0 0 0 0 0 0 0 " +
		     std::to_string(user) + " " + std::to_string(system) + " 0 0 0 0 1 0 " + std::to_string(start));
	}
};

void deterministic()
{
	Fixture source;
	source.file("/proc/stat", "cpu 10 0 5 85\ncpu0 2 0 2 26\ncpu1 nope 0 1 32\n");
	source.file("/proc/meminfo", "MemTotal: 256 kB\nMemFree: 192 kB\nCached: malformed kB\n");
	source.file("/proc/uptime", "42.125 4.0\n");
	source.file("/proc/mounts", "mariofs / mariofs rw 0 0\n"
				    "/dev/hda1 /ram/a volume with spaces ext2 ro 0 0\n"
				    "none /ram/missing proc rw 0 0\n");
	source.pids = {42, 51, 55};
	source.task(42, "a (name) )", 1, 1, 2);
	source.task(51, "old", 1, 1, 2);
	source.task(55, "exits", 1, 1, 2);
	source.counters = Network{MARIO_NET_PRESENT | MARIO_NET_UP | MARIO_NET_CARRIER, 0, 0,
		1ULL << 32, 2048, {}, {}};
	const auto base = sample(source);
	assert(base.cpu_status == Status::partial && base.cpus.size() == 2);
	assert(!base.cpus[0].busy_percent && !base.tasks[0].machine_percent);
	assert(base.network && !base.network->rx_per_second && !base.network->tx_per_second);
	assert(base.tasks[0].name == "a (name) )" && base.tasks[0].state == 'R' && base.tasks[0].parent == 1);
	assert(base.uptime_seconds == 42.125 && base.memory.total_kib == 256 &&
	       base.memory.used_kib == 64 && !base.memory.cached_kib);
	assert(base.mounts.size() == 3 && base.mounts[1].path == "/ram/a volume with spaces" &&
	       base.mounts[1].options == "ro" && base.mounts[1].space.capacity->available_bytes == 300);
	assert(base.mounts[2].space.error == ENOENT && !base.mounts[2].space.capacity);

	source.file("/proc/stat", "cpu 24 0 11 165\ncpu0 5 0 4 31\n");
	source.pids = {42, 51, 59};
	source.task(42, "a (name) )", 9, 3, 2);
	source.task(51, "new", 16, 16, 44);
	source.file("/proc/59/stat", "59 (malformed) R 1");
	source.now += 1000000000;
	source.counters->rx_bytes += 4096;
	source.counters->tx_bytes += 1024;
	auto next = sample(source);
	derive_rates(next, base);
	assert(next.network->rx_per_second == 4096 && next.network->tx_per_second == 1024);
	assert(next.cpus[0].busy_percent == 20 && next.cpus[1].busy_percent == 50);
	assert(next.task_status == Status::partial && next.tasks.size() == 2);
	assert(next.tasks[0].machine_percent == 10 && !next.tasks[1].machine_percent);
	assert(next.tasks[1].identity.start_ticks == 44 && next.tasks[1].name == "new");

	source.now += 1000000000;
	auto zero = sample(source);
	derive_rates(zero, next);
	assert(!zero.cpus[0].busy_percent && !zero.tasks[0].machine_percent);
	assert(zero.network->rx_per_second == 0 && zero.network->tx_per_second == 0);

	source.file("/proc/stat", "cpu 1 0 12 180\ncpu0 8 0 5 40\n");
	source.file("/proc/uptime", "nan\n");
	source.unmount = true;
	source.mount_reads = 0;
	source.counters->rx_bytes = 1;
	auto reset = sample(source);
	derive_rates(reset, next);
	assert(!reset.network->rx_per_second && reset.network->tx_per_second == 0);
	assert(!reset.cpus[0].busy_percent && !reset.tasks[0].machine_percent && reset.cpus[1].busy_percent);
	assert(!reset.uptime_seconds && reset.mount_status == Status::partial && reset.mounts[1].space.error == ESTALE);
	reset.monotonic_ns = *next.monotonic_ns + 10000000000ULL;
	derive_rates(reset, next);
	assert(!reset.cpus[1].busy_percent);
	assert(!reset.network->tx_per_second);
	auto disconnected = next;
	disconnected.network->flags &= ~MARIO_NET_CARRIER;
	derive_rates(disconnected, base);
	assert(!disconnected.network->rx_per_second && !disconnected.network->tx_per_second);
	std::puts("system-monitor: deterministic counters, races and invalid data verified");
}

}

int main(int argc, char **argv)
{
	assert(argc == 2 || argc == 3);
	deterministic();
	monitor::ProcSource source;
	const auto live = monitor::sample(source);
	assert(live.network);
	if (argc == 3) {
		uint32_t address;
		assert(inet_pton(AF_INET, argv[2], &address) == 1);
		const auto &network = *live.network;
		constexpr uint32_t connected = MARIO_NET_PRESENT | MARIO_NET_UP | MARIO_NET_CARRIER;
		assert((network.flags & connected) == connected && network.address == address);
		assert(network.rx_bytes > 0 && network.tx_bytes > 0);
	} else {
		assert(!live.network->flags && !live.network->rx_bytes && !live.network->tx_bytes);
	}
	assert(live.monotonic_ns && live.uptime_seconds && live.memory.total_kib && live.memory.used_kib &&
	       live.memory.cached_kib && !live.mounts.empty());
	assert(live.cpu_status == monitor::Status::complete && !live.cpus.empty() && live.cpus[0].core == -1);
	const std::size_t cores = live.cpus.size() - 1;
	assert(cores > 0 && cores == std::strtoul(argv[1], nullptr, 10));
	assert(std::any_of(live.tasks.begin(), live.tasks.end(), [](const monitor::Task &row) {
		return row.identity.pid == getpid() && !row.name.empty();
	}));
	std::printf("system-monitor: %zu online core rows and live guest telemetry verified\n", cores);
}
