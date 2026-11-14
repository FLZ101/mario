#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void run_handshakes(void)
{
	for (unsigned int round = 0; round < 8; round++) {
		struct timespec started;
		struct timespec now;
		int notice[2];
		int reply[2];
		char token = 1;
		pid_t child;
		ssize_t received;
		int status;
		int progressed;

		assert(pipe(notice) == 0 && pipe(reply) == 0);
		assert(fcntl(notice[0], F_SETFL, O_NONBLOCK) == 0);
		assert(fcntl(reply[0], F_SETFL, O_NONBLOCK) == 0);
		assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
		child = fork();
		assert(child >= 0);
		if (child == 0) {
			assert(close(notice[0]) == 0 && close(reply[1]) == 0);
			assert(write(notice[1], &token, 1) == 1);
			while (read(reply[0], &token, 1) < 0)
				assert(errno == EAGAIN);
			_exit(0);
		}
		assert(close(notice[1]) == 0 && close(reply[0]) == 0);
		do {
			received = read(notice[0], &token, 1);
			assert(received == 1 || (received < 0 && errno == EAGAIN));
			assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
		} while (received != 1 && now.tv_sec - started.tv_sec < 5);
		progressed = received == 1;
		assert(write(reply[1], &token, 1) == 1);
		assert(waitpid(child, &status, 0) == child);
		assert(WIFEXITED(status) && WEXITSTATUS(status) == 0 && progressed);
		assert(close(notice[0]) == 0 && close(reply[1]) == 0);
	}
}

int main(void)
{
	cpu_set_t available;
	cpu_set_t selected;
	cpu_set_t bootstrap;
	struct timespec started;
	struct timespec now;
	int gate[2];
	char token = 1;
	char path[64];
	char line[256];
	char state = 0;
	pid_t worker;
	int status;
	int cpu;

	assert(sched_getaffinity(0, sizeof(available), &available) == 0);
	cpu = CPU_ISSET(1, &available) ? 1 : 0;
	CPU_ZERO(&selected);
	CPU_SET(cpu, &selected);
	CPU_ZERO(&bootstrap);
	CPU_SET(0, &bootstrap);
	assert(sched_setaffinity(0, sizeof(bootstrap), &bootstrap) == 0);
	assert(pipe(gate) == 0);
	worker = fork();
	assert(worker >= 0);
	if (worker == 0) {
		assert(close(gate[1]) == 0 && read(gate[0], &token, 1) == 1);
		assert(close(gate[0]) == 0);
		assert(sched_setaffinity(0, sizeof(selected), &selected) == 0);
		run_handshakes();
		_exit(0);
	}
	assert(close(gate[0]) == 0);
	assert(snprintf(path, sizeof(path), "/proc/%d/stat", worker) > 0);
	assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
	do {
		FILE *process = fopen(path, "r");

		assert(process != NULL && fgets(line, sizeof(line), process) != NULL);
		assert(fclose(process) == 0 && sscanf(line, "%*d %*s %c", &state) == 1);
		assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec - started.tv_sec < 5);
		if (state != 'S')
			assert(usleep(1000) == 0);
	} while (state != 'S');
	assert(sched_setaffinity(worker, sizeof(selected), &selected) == 0);
	assert(write(gate[1], &token, 1) == 1 && close(gate[1]) == 0);
	for (;;) {
		pid_t result = waitpid(worker, &status, WNOHANG);
		pid_t churn;

		assert(result == 0 || result == worker);
		if (result == worker)
			break;
		churn = fork();
		assert(churn >= 0);
		if (churn == 0)
			_exit(0);
		assert(waitpid(churn, &status, 0) == churn && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	puts("smp-preempt: same-CPU busy peers make bounded progress without sleeping");
	return 0;
}
