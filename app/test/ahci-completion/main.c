#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 8
#define SECTORS 4
#define SECTOR_BYTES 512

static unsigned char expected[SECTORS][SECTOR_BYTES];

static void check_reads(unsigned int worker)
{
	unsigned char buffer[SECTOR_BYTES];
	struct timespec started;
	struct timespec now;
	int disk = open("/dev/hdc3", O_RDONLY);

	assert(disk >= 0 && clock_gettime(CLOCK_MONOTONIC, &started) == 0);
	for (unsigned int round = 0; round < 256; round++) {
		unsigned int sector = (round + worker) % SECTORS;

		memset(buffer, 0xa5, sizeof(buffer));
		assert(lseek(disk, sector * SECTOR_BYTES, SEEK_SET) == sector * SECTOR_BYTES);
		assert(read(disk, buffer, sizeof(buffer)) == sizeof(buffer));
		assert(memcmp(buffer, expected[sector], sizeof(buffer)) == 0);
		assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec - started.tv_sec < 30);
	}
	assert(close(disk) == 0);
}

int main(void)
{
	cpu_set_t available;
	cpu_set_t selected;
	pid_t children[WORKERS];
	unsigned char tokens[WORKERS] = {0};
	unsigned int cpu_count = 0;
	unsigned int cpus[CPU_SETSIZE];
	int gate[2];
	int disk;

	assert(sched_getaffinity(0, sizeof(available), &available) == 0);
	for (unsigned int cpu = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &available))
			cpus[cpu_count++] = cpu;
	assert(cpu_count != 0);
	CPU_ZERO(&selected);
	CPU_SET(0, &selected);
	assert(sched_setaffinity(0, sizeof(selected), &selected) == 0);
	disk = open("/dev/hdc3", O_RDONLY);
	assert(disk >= 0);
	for (unsigned int sector = 0; sector < SECTORS; sector++)
		assert(read(disk, expected[sector], SECTOR_BYTES) == SECTOR_BYTES);
	assert(expected[2][56] == 0x53 && expected[2][57] == 0xef);
	assert(close(disk) == 0 && pipe(gate) == 0);
	for (unsigned int worker = 0; worker < WORKERS; worker++) {
		struct timespec started;
		struct timespec now;
		char path[64];
		char line[256];
		char state = 0;

		children[worker] = fork();
		assert(children[worker] >= 0);
		if (children[worker] == 0) {
			unsigned char token;

			assert(close(gate[1]) == 0 && read(gate[0], &token, 1) == 1);
			assert(close(gate[0]) == 0);
			check_reads(worker);
			_exit(0);
		}
		assert(snprintf(path, sizeof(path), "/proc/%d/stat", children[worker]) > 0);
		assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
		do {
			FILE *process = fopen(path, "r");

			assert(process != NULL && fgets(line, sizeof(line), process) != NULL);
			assert(fclose(process) == 0 && sscanf(line, "%*d %*s %c", &state) == 1);
			assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec - started.tv_sec < 5);
			if (state != 'S')
				assert(usleep(1000) == 0);
		} while (state != 'S');
		CPU_ZERO(&selected);
		CPU_SET(cpus[cpu_count == 1 ? 0 : 1 + worker % (cpu_count - 1)], &selected);
		assert(sched_setaffinity(children[worker], sizeof(selected), &selected) == 0);
	}
	assert(close(gate[0]) == 0 && write(gate[1], tokens, sizeof(tokens)) == sizeof(tokens));
	assert(close(gate[1]) == 0);
	check_reads(WORKERS);
	for (unsigned int worker = 0; worker < WORKERS; worker++) {
		int status;

		assert(waitpid(children[worker], &status, 0) == children[worker]);
		assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	puts("ahci-completion: concurrent BSP/AP reads preserve exact sector bytes and bounded completion");
	return 0;
}
