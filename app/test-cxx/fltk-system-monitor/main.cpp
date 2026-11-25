#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/prctl.h>
#include <unistd.h>

namespace {

monitor::Snapshot fixture()
{
	int phase = 0;
	if (auto *file = std::fopen("/tmp/monitor-phase", "r")) {
		std::fscanf(file, "%d", &phase);
		std::fclose(file);
	}
	const auto steps = uint64_t(phase);
	monitor::Snapshot snapshot;
	snapshot.monotonic_ns = 1000000000ULL + steps * 1000000000ULL;
	snapshot.cpus = {{-1, 100 + steps * 75, 0, 100 + steps * 25, {}},
		{0, 40 + steps * 25, 0, 60 + steps * 25, {}},
		{1, 60 + steps * 50, 0, 40, {}}};
	snapshot.memory = {102400, 51200, 25600};
	snapshot.uptime_seconds = 3690;
	snapshot.tasks = {{{11, phase < 3 ? 10ULL : 99ULL}, 1, phase < 3 ? "zulu" : "beta", 'R', steps, 0, {}},
		{{22, 20}, 1, "alpha", 'S', 5, 0, {}}};
	snapshot.mounts = {{"/dev/demo", "/demo", "ext2", "rw", {monitor::Capacity{104857600, 26214400}, 0}}};
	snapshot.cpu_status = snapshot.task_status = snapshot.mount_status = monitor::Status::complete;
	return snapshot;
}

monitor::Snapshot live()
{
	if (access("/tmp/monitor-hold", F_OK) == 0) {
		if (auto *file = std::fopen("/tmp/monitor-worker", "w")) {
			std::fprintf(file, "%d\n", getpid());
			std::fclose(file);
		}
		sleep(60);
	}
	monitor::ProcSource source;
	return monitor::sample(source);
}

void observe(const monitor::Snapshot &snapshot)
{
	const auto cores = std::count_if(snapshot.cpus.begin(), snapshot.cpus.end(), [](const auto &cpu) { return cpu.core >= 0; });
	const auto task = std::find_if(snapshot.tasks.begin(), snapshot.tasks.end(), [](const auto &row) { return row.name == "monitor-busy"; });
	const bool mounted = std::any_of(snapshot.mounts.begin(), snapshot.mounts.end(), [](const auto &mount) {
		return mount.path == "/mnt/monitor" && mount.space.capacity && mount.space.capacity->total_bytes > 0;
	});
	static unsigned samples = 0;
	std::printf("sample=%u cores=%zu work=%d ticks=%llu rated=%d mounted=%d phase=%llu\n",
		++samples, std::size_t(cores), task == snapshot.tasks.end() ? 0 : task->identity.pid,
		task == snapshot.tasks.end() ? 0ULL : static_cast<unsigned long long>(task->user + task->system),
		int(task != snapshot.tasks.end() && task->machine_percent && *task->machine_percent > 0.), int(mounted),
		static_cast<unsigned long long>(snapshot.tasks.empty() ? 0 : snapshot.tasks.front().user));
	std::fflush(stdout);
}

}

int main(int argc, char **argv)
{
	if (argc == 2 && !std::strcmp(argv[1], "busy")) {
		const char name[16] = "monitor-busy";
		prctl(PR_SET_NAME, name, 0UL, 0UL, 0UL);
		for (;;)
			asm volatile("");
	}
	return monitor::run(argc == 2 && !std::strcmp(argv[1], "fixture") ? fixture : live, observe);
}
