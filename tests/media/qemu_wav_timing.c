#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static size_t (*write_data)(const void *, size_t, size_t, FILE *);
static const char *capture_path;
static FILE *timing;
static _Atomic(FILE *) capture;
static off_t offset;

__attribute__((constructor)) static void init(void)
{
	write_data = dlsym(RTLD_NEXT, "fwrite");
	capture_path = getenv("MARIO_WAV_CAPTURE");
	const char *timing_path = getenv("MARIO_WAV_TIMING");
	if (!write_data || !capture_path || !timing_path || !(timing = fopen(timing_path, "w")))
		_exit(125);
}

size_t fwrite(const void *data, size_t size, size_t count, FILE *stream)
{
	FILE *target = atomic_load_explicit(&capture, memory_order_relaxed);
	if (!target) {
		struct stat file, wanted;
		if (stat(capture_path, &wanted) == 0 && fstat(fileno(stream), &file) == 0 &&
		    file.st_dev == wanted.st_dev && file.st_ino == wanted.st_ino) {
			target = stream;
			atomic_store_explicit(&capture, target, memory_order_relaxed);
		}
	}
	if (stream != target)
		return write_data(data, size, count, stream);

	off_t position = ftello(stream);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	size_t written = write_data(data, size, count, stream);
	if (position == offset && written) {
		size_t bytes = written * size;
		fprintf(timing, "%lld.%09ld %lld %zu\n", (long long)now.tv_sec,
		        now.tv_nsec, (long long)position, bytes);
		offset += bytes;
	}
	return written;
}
