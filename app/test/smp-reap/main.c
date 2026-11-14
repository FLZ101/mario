#define _GNU_SOURCE
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
	cpu_set_t available;
	cpu_set_t selected;
	cpu_set_t bootstrap;

	assert(sched_getaffinity(0, sizeof(available), &available) == 0);
	CPU_ZERO(&selected);
	CPU_SET(CPU_ISSET(1, &available) ? 1 : 0, &selected);
	CPU_ZERO(&bootstrap);
	CPU_SET(0, &bootstrap);
	assert(sched_setaffinity(0, sizeof(bootstrap), &bootstrap) == 0);
	for (unsigned int round = 0; round < 128; round++) {
		struct timespec started;
		struct timespec now;
		char path[64];
		char line[256];
		char state = 0;
		char token = 1;
		int gate[2];
		int status;
		pid_t child;
		pid_t result;

		assert(pipe(gate) == 0);
		child = fork();
		assert(child >= 0);
		if (child == 0) {
			assert(close(gate[1]) == 0 && read(gate[0], &token, 1) == 1);
			assert(close(gate[0]) == 0);
			_exit(round % 127 + 1);
		}
		assert(close(gate[0]) == 0);
		assert(snprintf(path, sizeof(path), "/proc/%d/stat", child) > 0);
		assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
		do {
			FILE *process = fopen(path, "r");

			assert(process != NULL && fgets(line, sizeof(line), process) != NULL);
			assert(fclose(process) == 0 && sscanf(line, "%*d %*s %c", &state) == 1);
			assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec - started.tv_sec < 5);
			if (state != 'S')
				assert(usleep(1000) == 0);
		} while (state != 'S');
		assert(sched_setaffinity(child, sizeof(selected), &selected) == 0);
		assert(write(gate[1], &token, 1) == 1 && close(gate[1]) == 0);
		do {
			result = waitpid(child, &status, WNOHANG);
			assert(result == 0 || result == child);
			assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec - started.tv_sec < 5);
		} while (result == 0);
		assert(WIFEXITED(status) && WEXITSTATUS(status) == round % 127 + 1);
	}
	puts("smp-reap: cross-CPU polling reaps only parked children with exact statuses");
	return 0;
}
