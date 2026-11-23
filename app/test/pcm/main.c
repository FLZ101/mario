#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../../src/include/uapi/mario/pcm.h"

#define DEVICE "/dev/pcm0"
#define FIXTURE "/usr/share/mario-tests/media/stereo-markers.wav"
#define FIXTURE_FRAMES (MARIO_PCM_RATE * 2)
#define SHORT_FRAMES 123
#define CAPACITY 3840

static int16_t samples[FIXTURE_FRAMES][2];
static int16_t discarded[CAPACITY + 1][2];
static int16_t short_marker[SHORT_FRAMES][2];

static struct mario_pcm_status status(int device)
{
	struct mario_pcm_status info;
	assert(ioctl(device, MARIO_PCM_STATUS, &info) == 0);
	assert(info.version == MARIO_PCM_ABI_VERSION);
	assert(info.capacity_frames == CAPACITY);
	assert(info.pending_frames <= CAPACITY && info.writable_frames <= CAPACITY);
	return info;
}

static struct mario_pcm_status wait_for(int device, unsigned int state)
{
	struct timespec started, now;
	struct mario_pcm_status info;
	assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
	while ((info = status(device)).state != state) {
		assert(info.state != MARIO_PCM_FAILED);
		assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
		assert(now.tv_sec - started.tv_sec < 3);
		assert(usleep(1000) == 0);
	}
	return info;
}

static void assert_empty(int device)
{
	struct mario_pcm_status info = status(device);
	assert(info.state == MARIO_PCM_STOPPED && info.pending_frames == 0);
	assert(info.writable_frames == CAPACITY && info.consumed_frames == 0);
	assert(info.underruns == 0);
}

static void interrupt_writer(int signal_number)
{
}

static void wait_for_writer(pid_t child)
{
	char path[64], line[256], state;
	struct timespec started, now;
	assert(snprintf(path, sizeof(path), "/proc/%d/stat", child) > 0);
	assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
	do {
		FILE *process = fopen(path, "r");
		assert(process && fgets(line, sizeof(line), process));
		assert(fclose(process) == 0 && sscanf(line, "%*d %*s %c", &state) == 1);
		assert(state != 'Z' && clock_gettime(CLOCK_MONOTONIC, &now) == 0);
		assert(now.tv_sec - started.tv_sec < 3);
		if (state != 'S')
			assert(usleep(1000) == 0);
	} while (state != 'S');
}

static void check_writers(int device)
{
	int duplicate = dup(device);
	int gate[2], child_status;
	struct pollfd readiness = { .fd = device, .events = POLLOUT };
	assert(duplicate >= 0 && close(duplicate) == 0);
	assert(open(DEVICE, O_WRONLY) == -1 && errno == EBUSY);
	assert(write(device, discarded, 3) == -1 && errno == EINVAL);
	assert(write(device, discarded, sizeof(discarded)) == CAPACITY * 4);
	assert(write(device, discarded, 4) == -1 && errno == EAGAIN);
	assert(poll(&readiness, 1, 0) == 0);
	assert(fcntl(device, F_SETFL, 0) == 0);
	assert(pipe(gate) == 0);
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		struct sigaction action = { .sa_handler = interrupt_writer };
		assert(sigemptyset(&action.sa_mask) == 0);
		assert(sigaction(SIGUSR1, &action, NULL) == 0);
		assert(close(gate[0]) == 0 && write(gate[1], "x", 1) == 1);
		ssize_t result = write(device, discarded, 4);
		if (result != -1 || errno != EINTR) {
			fprintf(stderr, "pcm: interrupted write returned %d, errno=%d\n",
				(int)result, errno);
			_exit(1);
		}
		_exit(0);
	}
	char ready;
	assert(close(gate[1]) == 0 && read(gate[0], &ready, 1) == 1);
	assert(close(gate[0]) == 0);
	wait_for_writer(child);
	assert(kill(child, SIGUSR1) == 0 && waitpid(child, &child_status, 0) == child);
	assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
	assert(fcntl(device, F_SETFL, O_NONBLOCK) == 0);
	assert(ioctl(device, MARIO_PCM_RESET) == 0);
	assert_empty(device);
	assert(poll(&readiness, 1, 0) == 1 && readiness.revents == POLLOUT);
}

static void read_fixture(const char *path)
{
	FILE *wav = fopen(path, "rb");
	char riff[12], kind[4];
	uint32_t size;
	assert(wav && fread(riff, 1, sizeof(riff), wav) == sizeof(riff));
	assert(!memcmp(riff, "RIFF", 4) && !memcmp(riff + 8, "WAVE", 4));
	for (;;) {
		assert(fread(kind, 1, 4, wav) == 4 && fread(&size, 4, 1, wav) == 1);
		if (!memcmp(kind, "data", 4))
			break;
		assert(size <= sizeof(samples));
		assert(fseek(wav, (size + 1) & ~1U, SEEK_CUR) == 0);
	}
	assert(size == sizeof(samples));
	assert(fread(samples, 1, size, wav) == size && fclose(wav) == 0);
}

static void play_fixture(int device)
{
	struct mario_pcm_status info;
	struct pollfd readiness = { .fd = device, .events = POLLOUT };
	int paused = 0;
	unsigned int sent = 0;
	assert(write(device, samples, CAPACITY * 4) == CAPACITY * 4);
	sent = CAPACITY;
	assert(ioctl(device, MARIO_PCM_START) == 0);
	while (sent < FIXTURE_FRAMES) {
		ssize_t bytes;
		info = status(device);
		assert(info.state == MARIO_PCM_RUNNING && !info.underruns);
		if (!paused && info.consumed_frames > 12000) {
			assert(ioctl(device, MARIO_PCM_PAUSE) == 0);
			info = wait_for(device, MARIO_PCM_PAUSED);
			assert(info.consumed_frames + info.pending_frames == sent);
			printf("pcm: paused after %u fixture frames\n", (unsigned int)info.consumed_frames);
			assert(usleep(100000) == 0);
			assert(status(device).consumed_frames == info.consumed_frames);
			bytes = write(device, samples[sent], (FIXTURE_FRAMES - sent) * 4);
			assert(bytes > 0 && bytes % 4 == 0);
			sent += bytes / 4;
			assert(ioctl(device, MARIO_PCM_RESUME) == 0);
			paused = 1;
			continue;
		}
		assert(poll(&readiness, 1, 500) == 1 && readiness.revents == POLLOUT);
		bytes = write(device, samples[sent], (FIXTURE_FRAMES - sent) * 4);
		assert(bytes > 0 && bytes % 4 == 0);
		sent += bytes / 4;
	}
	assert(paused && ioctl(device, MARIO_PCM_DRAIN) == 0);
	info = wait_for(device, MARIO_PCM_STOPPED);
	assert(info.pending_frames == 0 && info.consumed_frames == FIXTURE_FRAMES);
	assert(info.underruns == 0);
}

static pid_t start_disk_reader(int device, int *stop)
{
	int lifetime[2], ready[2];
	char token;
	assert(pipe(lifetime) == 0 && pipe(ready) == 0);
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		char expected[512], buffer[512];
		unsigned int reads = 0;
		struct pollfd input = { .fd = lifetime[0], .events = POLLIN };
		assert(close(device) == 0 && close(lifetime[1]) == 0 && close(ready[0]) == 0);
		int disk = open("/dev/hdc3", O_RDONLY);
		assert(disk >= 0 && read(disk, expected, sizeof(expected)) == sizeof(expected));
		assert(write(ready[1], "x", 1) == 1 && close(ready[1]) == 0);
		while (poll(&input, 1, 0) == 0) {
			assert(lseek(disk, 0, SEEK_SET) == 0);
			assert(read(disk, buffer, sizeof(buffer)) == sizeof(buffer));
			assert(memcmp(expected, buffer, sizeof(buffer)) == 0);
			reads++;
		}
		assert(reads >= 8 && close(disk) == 0);
		puts("pcm: concurrent AHCI reads preserved sector bytes");
		fflush(stdout);
		_exit(0);
	}
	assert(close(lifetime[0]) == 0 && close(ready[1]) == 0);
	assert(read(ready[0], &token, 1) == 1 && close(ready[0]) == 0);
	*stop = lifetime[1];
	return child;
}

static int check_reopen(int device)
{
	int child_status;
	assert(ioctl(device, MARIO_PCM_RESET) == 0);
	assert(write(device, discarded, CAPACITY * 4) == CAPACITY * 4);
	assert(ioctl(device, MARIO_PCM_START) == 0);
	assert(ioctl(device, MARIO_PCM_RESET) == 0);
	assert_empty(device);
	assert(close(device) == 0);
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		int opened = open(DEVICE, O_WRONLY | O_NONBLOCK);
		assert(opened >= 0 && write(opened, discarded, CAPACITY * 4) == CAPACITY * 4);
		assert(ioctl(opened, MARIO_PCM_START) == 0);
		_exit(0);
	}
	assert(waitpid(child, &child_status, 0) == child);
	assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
	device = open(DEVICE, O_WRONLY | O_NONBLOCK);
	assert(device >= 0);
	assert_empty(device);
	return device;
}

static void play_short(int device, int recover)
{
	struct mario_pcm_status info;
	for (unsigned int index = 0; index < SHORT_FRAMES; index++) {
		short_marker[index][0] = recover ? 0 : 2345;
		short_marker[index][1] = recover ? -3456 : 0;
	}
	assert(write(device, short_marker, sizeof(short_marker)) == sizeof(short_marker));
	assert(ioctl(device, MARIO_PCM_START) == 0);
	if (recover)
		assert(ioctl(device, MARIO_PCM_DRAIN) == 0);
	info = wait_for(device, recover ? MARIO_PCM_STOPPED : MARIO_PCM_UNDERRUN);
	assert(info.pending_frames == 0 && info.consumed_frames == SHORT_FRAMES * (1 + recover));
	assert(info.underruns == 1);
}

static void check_late_poll(int device)
{
	struct mario_pcm_status info;
	for (unsigned int index = 0; index < SHORT_FRAMES; index++)
		short_marker[index][0] = 4100;
	assert(write(device, short_marker, sizeof(short_marker)) == sizeof(short_marker));
	assert(ioctl(device, MARIO_PCM_START) == 0);
	info = wait_for(device, MARIO_PCM_UNDERRUN);
	assert(info.pending_frames == 0 && info.consumed_frames == 0 && info.underruns == 1);
	assert(ioctl(device, MARIO_PCM_RESET) == 0);
	play_short(device, 0);
	assert(close(device) == 0);
	puts("pcm: missed poll invalidates position, and playback restarts");
}

int main(int argc, char **argv)
{
	int device;
	int stop = -1;
	pid_t reader = -1;
	if (argc == 2 && !strcmp(argv[1], "absent")) {
		assert(open(DEVICE, O_WRONLY) == -1 && errno == ENODEV);
		puts("pcm: unavailable hardware returns ENODEV");
		return 0;
	}
	assert(open(DEVICE, O_RDONLY) == -1 && errno == EACCES);
	device = open(DEVICE, O_WRONLY | O_NONBLOCK);
	assert(device >= 0);
	assert_empty(device);
	if (argc == 2 && !strcmp(argv[1], "late")) {
		check_late_poll(device);
		return 0;
	}
	check_writers(device);
	int stress = argc == 2 && !strcmp(argv[1], "stress");
	read_fixture(argc == 2 && !stress ? argv[1] : FIXTURE);
	if (stress)
		reader = start_disk_reader(device, &stop);
	play_fixture(device);
	if (stress) {
		int child_status;
		assert(close(stop) == 0 && waitpid(reader, &child_status, 0) == reader);
		assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
	}
	for (unsigned int index = CAPACITY / 2; index < CAPACITY; index++)
		discarded[index][0] = discarded[index][1] = 12345;
	device = check_reopen(device);
	play_short(device, 0);
	play_short(device, 1);
	assert(close(device) == 0);
	puts("pcm: wrap, pause, drain, reset, underrun and reopen passed");
	return 0;
}
