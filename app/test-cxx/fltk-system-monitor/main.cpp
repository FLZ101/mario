#include "ui.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mario/netctl.h>
#include <mario/process.h>
#include <pthread.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
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
	snapshot.network = monitor::Network{MARIO_NET_PRESENT | MARIO_NET_UP | MARIO_NET_CARRIER,
		0, 0, 1024 + steps * 2048, 1024 + steps * 4096, {}, {}};
	snapshot.uptime_seconds = 3690;
	snapshot.tasks = {{{11, phase < 3 ? 10ULL : 99ULL}, 1, phase < 3 ? "zulu" : "beta", 'R', steps, 0, {}},
		{{22, 20}, 1, "alpha", 'S', 5, 0, {}}};
	snapshot.mounts = {{"/dev/demo", "/demo", "ext2", "rw", {monitor::Capacity{104857600, 26214400}, 0}}};
	snapshot.cpu_status = snapshot.task_status = snapshot.mount_status = monitor::Status::complete;
	return snapshot;
}

monitor::Snapshot tree_fixture()
{
	auto snapshot = fixture();
	const auto phase = snapshot.tasks.front().user;
	snapshot.tasks = {{{11, phase ? 60ULL : 10ULL}, 1, "session", 'S', phase, 0, {}},
		{{22, 20}, 11, "editor", 'R', 12, 3, {}},
		{{33, 30}, 22, "worker", 'S', 6, 1, {}},
		{{44, 40}, 999, "daemon", 'S', 9, 2, {}},
		{{55, 50}, 11, "shell", 'S', 4, 0, {}},
		{{66, 80}, 77, "cycle-a", 'S', 0, 0, {}},
		{{77, 80}, 66, "cycle-b", 'S', 0, 0, {}}};
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
	if (snapshot.network && snapshot.network->rx_per_second && snapshot.network->tx_per_second)
		std::printf("network rx=%.0f tx=%.0f\n", *snapshot.network->rx_per_second,
			*snapshot.network->tx_per_second);
	std::fflush(stdout);
}

void *wait_thread(void *)
{
	for (;;)
		pause();
}

[[noreturn]] void signal_target(bool ignore_term)
{
	if (ignore_term)
		assert(signal(SIGTERM, SIG_IGN) != SIG_ERR);
	pthread_t worker;
	assert(pthread_create(&worker, nullptr, wait_thread, nullptr) == 0);
	char name[16] = {};
	std::strcpy(name, ignore_term ? "monitor-force" : "monitor-end");
	assert(prctl(PR_SET_NAME, name, 0UL, 0UL, 0UL) == 0);
	for (;;)
		pause();
}

int checked_signal(monitor::Identity identity, int sig)
{
	return prctl(MARIO_PR_SIGNAL_PROCESS, static_cast<unsigned long>(identity.pid),
		static_cast<unsigned long>(identity.start_ticks), static_cast<unsigned long>(sig), 0UL);
}

void check_signal(bool force)
{
	const auto pid = fork();
	assert(pid >= 0);
	if (!pid)
		signal_target(force);
	monitor::ProcSource source;
	std::optional<monitor::Identity> leader, worker;
	for (int attempt = 0; attempt < 100 && (!leader || !worker); ++attempt) {
		for (const auto &task : monitor::sample(source).tasks) {
			if (task.tgid != pid)
				continue;
			if (task.identity.pid != pid)
				worker = task.identity;
			else if (task.name == (force ? "monitor-force" : "monitor-end"))
				leader = task.identity;
		}
		if (!leader || !worker)
			usleep(20000);
	}
	assert(leader && worker);
	assert(checked_signal({pid, leader->start_ticks + 1}, SIGKILL) < 0 && errno == ESRCH);
	assert(checked_signal(*worker, SIGKILL) < 0 && errno == ESRCH);
	assert(waitpid(pid, nullptr, WNOHANG) == 0);
	assert(checked_signal(*leader, SIGTERM) == 0);
	if (force) {
		assert(kill(pid, SIGSTOP) == 0);
		assert(waitpid(pid, nullptr, WUNTRACED) == pid);
		assert(checked_signal(*leader, SIGKILL) == 0);
	}
	int status;
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFSIGNALED(status) && WTERMSIG(status) == (force ? SIGKILL : SIGTERM));
	assert(checked_signal(*leader, SIGKILL) < 0 && errno == ESRCH);
	for (const auto &task : monitor::sample(source).tasks)
		assert(task.tgid != pid || task.state == 'Z');
}

void *exit_thread(void *)
{
	std::_Exit(37);
}

int signal_checks()
{
	assert(checked_signal({1, 0}, SIGKILL) < 0 && errno == EPERM);
	assert(checked_signal({getpid(), 0}, SIGKILL) < 0 && errno == EPERM);
	assert(checked_signal({0, 0}, SIGKILL) < 0 && errno == EINVAL);
	check_signal(false);
	check_signal(true);
	const auto pid = fork();
	assert(pid >= 0);
	if (!pid) {
		pthread_t worker;
		assert(pthread_create(&worker, nullptr, exit_thread, nullptr) == 0);
		for (;;)
			pause();
	}
	int status;
	assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 37);
	std::puts("monitor signals: process identity, protection and thread-group exit verified");
	return 0;
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
	if (argc == 2 && !std::strcmp(argv[1], "tree"))
		return monitor::run(tree_fixture, observe);
	if (argc == 2 && !std::strcmp(argv[1], "signal-checks"))
		return signal_checks();
	if (argc == 2 && !std::strcmp(argv[1], "end-target"))
		signal_target(false);
	if (argc == 2 && !std::strcmp(argv[1], "force-target"))
		signal_target(true);
	return monitor::run(argc == 2 && !std::strcmp(argv[1], "fixture") ? fixture : live, observe);
}
